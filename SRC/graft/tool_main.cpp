// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
// graft.exe — обслуживание установки: поставить хост, посмотреть что загружено,
// понять почему натив не завёлся, сгенерировать объявления, развернуть шаблон.
//
// Генерация работает с ЛЮБЫМ плагином, а не только со своим: плагин сам себе реестр —
// он экспортирует graft_plugin_entry, и с host == NULL тот обязан просто отдать
// дескрипторы. Поэтому персональный protogen в каждом проекте не нужен.
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include "graft/abi.h"
#include "graft/enfparse.hpp"
#include "graft/native.hpp"
#include "graft/plugins.hpp"

namespace fs = std::filesystem;

namespace {

int fail(const std::string& text) {
    std::println(stderr, "graft: {}", text);
    return 1;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), need,
                        nullptr, nullptr);
    return out;
}

// Файл перезаписывается только при изменении, чтобы сборка не дёргала PBO зря.
bool write_if_changed(const fs::path& path, const std::string& text) {
    {
        std::ifstream in(path, std::ios::binary);
        const std::string old{std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>()};
        if (in && old == text) {
            return true;
        }
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::println(stderr, "graft: не открыть {}", path.string());
        return false;
    }
    out << text;
    return true;
}

struct opened {
    HMODULE module = nullptr;
    graft_plugin_info info{};
    std::uint32_t status = GRAFT_ERR_INTERNAL;
    std::string why;
};

// Открыть плагин БЕЗ хоста: движка в этом процессе нет, и entry обязан это пережить.
opened open_plugin(const fs::path& path) {
    opened out;
    out.module = LoadLibraryW(path.wstring().c_str());
    if (!out.module) {
        out.why = "не загрузилась (нет зависимостей или не x64)";
        return out;
    }
    auto entry = reinterpret_cast<graft_plugin_entry_fn>(
        GetProcAddress(out.module, GRAFT_PLUGIN_ENTRY_NAME));
    if (!entry) {
        out.why = "нет экспорта " GRAFT_PLUGIN_ENTRY_NAME " — это не плагин graft";
        FreeLibrary(out.module);
        out.module = nullptr;
        return out;
    }
    const std::uint32_t code = entry(nullptr, &out.info);
    out.status = code == GRAFT_OK ? graft::plugins::check(out.info) : code;
    out.why = graft::plugins::explain(out.status);
    return out;
}

std::vector<const graft_native_desc*> descriptors(const graft_plugin_info& info) {
    const std::span all{info.natives, info.count};
    return {std::from_range, all | std::views::transform([](const auto& d) { return &d; })};
}

// Имя файла объявлений несёт имя плагина: в одном моде может жить несколько плагинов,
// и общий grafted_natives.c они бы затирали друг другу.
std::string decl_file(const std::string& plugin) {
    return "grafted_natives_" + plugin + ".c";
}

int generate(const std::string& plugin, const std::vector<const graft_native_desc*>& all,
             const fs::path& scripts) {
    for (const std::string& module : graft::proto_modules(all)) {
        if (!write_if_changed(scripts / module / decl_file(plugin),
                              graft::proto_file(all, module.c_str()))) {
            return 1;
        }
    }
    std::println("graft: {} -> {}", plugin, scripts.string());
    return 0;
}

// Реестр самого хоста слинкован в этот exe (host_natives.cpp) — отдельная загрузка
// dwmapi.dll ради него не нужна.
std::vector<graft_native_desc> host_descs() {
    std::vector<const graft::native*> ordered;
    for (const graft::native* n = graft::natives(); n; n = n->next) {
        ordered.push_back(n);
    }
    std::vector<graft_native_desc> out;
    out.reserve(ordered.size());
    for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
        const graft::native& n = **it;
        out.push_back({n.class_name, n.name, n.impl, n.ret, n.args, n.module, n.declare_as,
                       static_cast<std::uint8_t>(n.is_static),
                       static_cast<std::uint8_t>(n.marshalled),
                       static_cast<std::uint8_t>(n.generate)});
    }
    return out;
}

int cmd_protogen(int argc, char** argv) {
    if (argc < 4) {
        return fail("protogen <плагин.dll|--host> <каталог scripts>");
    }
    const fs::path scripts = argv[3];
    if (std::string{argv[2]} == "--host") {
        const std::vector<graft_native_desc> own = host_descs();
        std::vector<const graft_native_desc*> all;
        for (const graft_native_desc& d : own) {
            all.push_back(&d);
        }
        return generate("graft", all, scripts);
    }
    const opened plugin = open_plugin(argv[2]);
    if (plugin.status != GRAFT_OK) {
        return fail(std::string{argv[2]} + ": " + plugin.why);
    }
    return generate(plugin.info.name, descriptors(plugin.info), scripts);
}

// ── Зеркало движкового API ───────────────────────────────────────────────────
// Обратное направление protogen: тот печатает СКРИПТУ объявления наших нативов, этот
// печатает НАМ объявления движковых. Источник истины у обоих один — файл, по которому
// сигнатуру проверяет компилятор Enforce.
int cmd_apigen(int argc, char** argv) {
    if (argc < 4) {
        return fail("apigen <каталог scripts> <куда>");
    }
    const fs::path scripts = argv[2];
    const fs::path out_dir = argv[3];
    if (!fs::exists(scripts)) {
        return fail("нет каталога " + scripts.string());
    }
    const graft::enf::unit all = graft::enf::parse_tree(scripts.string());
    const std::vector<std::string> modules = graft::enf::mirror_modules(all);
    if (modules.empty()) {
        return fail("в " + scripts.string() + " не нашлось ни одного `proto`");
    }

    std::error_code ec;
    fs::create_directories(out_dir, ec);
    std::string previous;
    for (const std::string& module : modules) {
        const std::string file = module + ".hpp";
        const std::string text = graft::enf::mirror(all, module, previous);
        std::ofstream(out_dir / file, std::ios::binary) << text;
        std::println("{:<12} -> {}", module, (out_dir / file).string());
        // Сосед по каталогу — простым именем: зеркало кладут куда угодно, и подставлять
        // ему свой путь нельзя. Полный путь тут ещё и заставлял clang-cl ругаться
        // (-Wmicrosoft-include): такой include находится только по правилам MSVC.
        previous = file;
    }

    // Разница между «разобрано» и «выпущено» — не потеря, а перечень осознанных отказов.
    // Её надо видеть цифрой: молчаливо неполное зеркало хуже явно неполного.
    const graft::enf::coverage c = graft::enf::measure(all);
    std::println("\nклассов {} из них с методами {}", c.classes, c.emitted_classes);
    std::println("объявлений {}: прямых {}, маршалируемых {}", c.functions, c.emitted_native,
                 c.emitted_marshalled);
    std::println("пропущено: статических {}, с невыразимым типом {}", c.skipped_static,
                 c.skipped_type);

    // Условия препроцессора не разрешены, а перенесены в C++ как есть: какие из них
    // определены в КОНКРЕТНОЙ сборке игры, знает только её владелец. Печатаем список,
    // чтобы было что сверить со строкой `defines:` в script-логе сервера.
    if (!all.conditions.empty()) {
        std::vector<std::string> sorted = all.conditions;
        std::ranges::sort(sorted);
        std::println("\nусловий препроцессора {} — определи те же, что у твоей сборки игры:",
                     sorted.size());
        std::string line;
        for (const std::string& name : sorted) {
            if (line.size() + name.size() > 76) {
                std::println("  {}", line);
                line.clear();
            }
            line += (line.empty() ? "" : " ") + name;
        }
        if (!line.empty()) {
            std::println("  {}", line);
        }
    }
    return 0;
}

// ── Установка ────────────────────────────────────────────────────────────────

bool is_our_host(const fs::path& dll) {
    HMODULE m = LoadLibraryExW(dll.wstring().c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (!m) {
        return false;
    }
    // Как файл данных экспорты не разрешаются, поэтому ищем имя в самом образе: этого
    // достаточно, чтобы отличить свою dwmapi от чужого прокси.
    FreeLibrary(m);
    std::ifstream in(dll, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>()};
    return bytes.find("graft_host_version") != std::string::npos;
}

int cmd_install(int argc, char** argv) {
    if (argc < 3) {
        return fail("install <каталог игры> [dwmapi.dll]");
    }
    const fs::path game = argv[2];
    if (!fs::is_directory(game)) {
        return fail("нет каталога " + game.string());
    }
    fs::path source = argc > 3 ? fs::path{argv[3]}
                               : fs::path{narrow(std::wstring(1024, 0))};
    if (argc <= 3) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        source = fs::path{self}.parent_path() / "dwmapi.dll";
    }
    if (!fs::exists(source)) {
        return fail("не найден " + source.string());
    }

    const fs::path target = game / "dwmapi.dll";
    if (fs::exists(target)) {
        if (!is_our_host(target)) {
            // Мирить два прокси мы не умеем и делать вид не будем.
            return fail("в " + game.string() +
                        " уже лежит ЧУЖАЯ dwmapi.dll. Переименуй её или разберись, чья "
                        "она — перезаписывать не буду");
        }
        std::error_code ec;
        if (fs::last_write_time(target, ec) > fs::last_write_time(source, ec)) {
            std::println("graft: установленный хост новее, не трогаю");
            return 0;
        }
    }
    std::error_code ec;
    fs::create_directories(game / "grafted", ec);
    fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return fail("не скопировать: " + ec.message() + " (игра запущена?)");
    }
    std::println("graft: хост установлен -> {}", target.string());
    // Два места, и оба настоящие: общая папка удобна на разработке, папка мода едет
    // вместе с модом. Печатаем оба, чтобы выбор был осознанным, а не единственным.
    std::println("graft: плагины клади в {} или в @МОД\\grafted рядом с игрой",
                 (game / "grafted").string());
    return 0;
}

// ── Осмотр ───────────────────────────────────────────────────────────────────

std::vector<fs::path> plugin_files(const fs::path& game) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const fs::path& dir : {game / "grafted"}) {
        if (!fs::is_directory(dir, ec)) {
            continue;
        }
        for (const auto& item : fs::directory_iterator(dir, ec)) {
            if (item.path().extension() == ".dll") {
                out.push_back(item.path());
            }
        }
    }
    // Папки модов рядом с игрой: @МОД/grafted/*.dll. Мод узнаётся по наличию grafted/,
    // а не по префиксу @: хост идёт по -mod= и о префиксах не знает, а имя папки выбирает
    // её автор. Префикс знал только этот обход — и ровно поэтому врал.
    for (const auto& item : fs::directory_iterator(game, ec)) {
        if (!item.is_directory()) {
            continue;
        }
        const fs::path dir = item.path() / "grafted";
        if (!fs::is_directory(dir, ec)) {
            continue;
        }
        for (const auto& dll : fs::directory_iterator(dir, ec)) {
            if (dll.path().extension() == ".dll") {
                out.push_back(dll.path());
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Обратная сторона install: игра возвращается в исходное состояние. Снимаем ровно то,
// что клали сами, — хост и пустую общую папку. Плагины в модах не наши: мод ставил
// пользователь, и удалять чужой каталог за компанию нельзя. Их просто перечисляем.
int cmd_uninstall(int argc, char** argv) {
    if (argc < 3) {
        return fail("uninstall <каталог игры>");
    }
    const fs::path game = argv[2];
    if (!fs::is_directory(game)) {
        return fail("нет каталога " + game.string());
    }
    const fs::path host = game / "dwmapi.dll";
    std::error_code ec;
    if (!fs::exists(host)) {
        std::println("graft: хоста нет — снимать нечего");
    } else if (!is_our_host(host)) {
        return fail("в " + game.string() + " лежит ЧУЖАЯ dwmapi.dll — не трогаю");
    } else if (!fs::remove(host, ec)) {
        return fail("не снять хост: " + ec.message() + " (игра запущена?)");
    } else {
        std::println("graft: хост снят -> {}", host.string());
    }
    // remove() удаляет каталог, только если он пуст: чужие плагины в общей папке
    // переживут снятие хоста, и это правильно — их туда клали не мы.
    if (fs::remove(game / "grafted", ec)) {
        std::println("graft: пустая {} убрана", (game / "grafted").string());
    }
    const auto left = plugin_files(game);
    if (!left.empty()) {
        std::println("graft: плагины остались лежать в модах — без хоста они мертвы,");
        std::println("       удаляй вместе с самим модом:");
        for (const fs::path& path : left) {
            std::println("       {}", path.string());
        }
    }
    return 0;
}

int cmd_list(int argc, char** argv) {
    if (argc < 3) {
        return fail("list <каталог игры>");
    }
    const fs::path game = argv[2];
    const fs::path host = game / "dwmapi.dll";
    std::println("хост:   {}", !fs::exists(host)  ? "НЕ УСТАНОВЛЕН"
                               : is_our_host(host) ? "установлен"
                                                   : "ЧУЖАЯ dwmapi.dll");
    const auto files = plugin_files(game);
    if (files.empty()) {
        std::println("плагинов не найдено");
        return 0;
    }
    std::println("{:<20} {:<8} {:<8} {}", "плагин", "версия", "нативов", "файл");
    for (const fs::path& path : files) {
        const opened p = open_plugin(path);
        if (p.status != GRAFT_OK) {
            std::println("{:<20} {:<8} {:<8} {}  <- {}", "?", "-", "-",
                         path.filename().string(), p.why);
            continue;
        }
        std::println("{:<20} {:<8} {:<8} {}", p.info.name, p.info.version, p.info.count,
                     path.filename().string());
    }
    return 0;
}

int cmd_doctor(int argc, char** argv) {
    if (argc < 3) {
        return fail("doctor <каталог игры>");
    }
    const fs::path game = argv[2];
    int problems = 0;

    const fs::path host = game / "dwmapi.dll";
    if (!fs::exists(host)) {
        std::println("[!] хост не установлен: graft install {}", game.string());
        ++problems;
    } else if (!is_our_host(host)) {
        std::println("[!] dwmapi.dll в каталоге игры — не наша");
        ++problems;
    }

    // Сверяем каждый плагин с ЭТОЙ версией инструмента: она собрана из того же ABI,
    // что и хост, поэтому её вердикт совпадёт с тем, что скажет игра.
    std::vector<graft::plugins::entry> all;
    std::vector<opened> live;
    for (const fs::path& path : plugin_files(game)) {
        opened p = open_plugin(path);
        if (p.status != GRAFT_OK) {
            std::println("[!] {}: {}", path.filename().string(), p.why);
            ++problems;
            continue;
        }
        live.push_back(p);
    }
    for (const opened& p : live) {
        for (const graft_native_desc& d : std::span{p.info.natives, p.info.count}) {
            all.push_back({&d, p.info.name});
        }
    }
    std::vector<graft::plugins::collision> bad;
    graft::plugins::merge(all, bad);
    for (const graft::plugins::collision& c : bad) {
        std::println("[!] {}", graft::plugins::describe(c));
        ++problems;
    }
    if (graft::plugins::disposes_clash(all)) {
        std::println("[!] два плагина держат состояние на одном скриптовом классе");
        ++problems;
    }

    const fs::path journal = game / "graft.log";
    if (!fs::exists(journal)) {
        std::println("[i] журнала graft.log нет — игра с этим хостом ещё не запускалась");
    } else {
        std::ifstream in(journal);
        std::string line;
        std::vector<std::string> complaints;
        while (std::getline(in, line)) {
            if (!line.empty() && line[0] == '!') {
                complaints.push_back(line);
            }
        }
        // Жалобы журнала печатаем, но в счёт проблем не берём: часть из них ставят
        // сами тесты, проверяя, что диагностика работает. Решает человек.
        for (const std::string& c : complaints) {
            std::println("[журнал] {}", c);
        }
        if (!complaints.empty()) {
            std::println("[i] строк-жалоб в журнале: {} (часть может быть от тестов)",
                         complaints.size());
        }
    }

    if (problems) {
        std::println("\nнайдено проблем: {}", problems);
        return 1;
    }
    std::println("\nустановка в порядке");
    return 0;
}

// Шаблон — пример hello: самый маленький рабочий мод. Ищем его вверх от exe: инструмент
// живёт в build/<пресет>/bin, а исходники — в корне репозитория, и глубина вложенности
// зависит от пресета. Не нашли — путь передаётся третьим аргументом.
fs::path find_template() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    fs::path dir = fs::path{self}.parent_path();
    for (int up = 0; up < 6 && !dir.empty(); ++up) {
        // Ищем именно КОРЕНЬ исходников, а не первый попавшийся examples/hello: рядом с
        // exe лежит каталог сборки примера с тем же именем, и он не шаблон.
        if (fs::is_regular_file(dir / "cmake" / "graft_plugin.cmake")) {
            return dir / "examples" / "hello";
        }
        dir = dir.parent_path();
    }
    return {};
}

int cmd_new(int argc, char** argv) {
    if (argc < 4) {
        return fail("new <имя> <куда> [шаблон]");
    }
    const std::string name = argv[2];
    const fs::path dest = fs::path{argv[3]} / name;
    const fs::path templ = argc > 4 ? fs::path{argv[4]} : find_template();
    if (templ.empty() || !fs::is_directory(templ)) {
        return fail("не найден шаблон (передай путь третьим аргументом: graft new "
                    "<имя> <куда> <шаблон>)");
    }
    if (fs::exists(dest)) {
        return fail(dest.string() + " уже существует");
    }
    std::error_code ec;
    fs::copy(templ, dest, fs::copy_options::recursive, ec);
    if (ec) {
        return fail("не скопировать шаблон: " + ec.message());
    }
    // Каталоги сборки шаблона новому проекту не нужны: развёрнутый проект должен быть
    // чистым, а не приезжать с чужими бинарями внутри.
    for (const char* junk : {"build", "out", "dist"}) {
        fs::remove_all(dest / junk, ec);
    }
    std::println("graft: шаблон развёрнут -> {}", dest.string());
    std::println("graft: имя плагина правится в CMakeLists.txt (NAME) и в src/hello.cpp");
    std::println("graft: собрать -> cmake --preset release && cmake --build --preset release");
    return 0;
}

int usage() {
    std::print(
        "graft — обслуживание graft-установки\n\n"
        "  graft install  <каталог игры> [dwmapi.dll]   поставить хост\n"
        "  graft uninstall <каталог игры>               снять хост\n"
        "  graft list     <каталог игры>                что установлено\n"
        "  graft doctor   <каталог игры>                почему не работает\n"
        "  graft protogen <плагин.dll|--host> <scripts> объявления для PBO\n"
        "  graft apigen   <scripts> <куда>              зеркало движкового API на C++\n"
        "  graft defines  <script_*.log>                дефайны игры для сборки зеркала\n"
        "  graft new      <имя> <куда> [шаблон]         развернуть шаблон плагина\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage();
    }
    const std::string cmd = argv[1];
    if (cmd == "install") {
        return cmd_install(argc, argv);
    }
    if (cmd == "uninstall") {
        return cmd_uninstall(argc, argv);
    }
    if (cmd == "list") {
        return cmd_list(argc, argv);
    }
    if (cmd == "doctor") {
        return cmd_doctor(argc, argv);
    }
    if (cmd == "protogen") {
        return cmd_protogen(argc, argv);
    }
    if (cmd == "apigen") {
        return cmd_apigen(argc, argv);
    }
    if (cmd == "new") {
        return cmd_new(argc, argv);
    }
    return usage();
}
