#include <windows.h>

#include <format>
#include <unordered_map>
#include <utility>

#include "graft/engine.hpp"
#include "graft/loader.hpp"

// Хранилище результата загрузки, отдельно от самой загрузки.
//
// Не косметика: класс Graft (host_natives.cpp) читает эти списки, а компилируется он и
// в хост, и в graft.exe — инструменту загрузчик не нужен и не может быть нужен, там нет
// ни игры, ни плагинов. Так у него оказываются те же символы, но пустые, а не отсутствующие.
namespace graft::loader {
namespace {

std::vector<plugins::entry> g_registry;
std::vector<row> g_rows;
std::vector<plugins::collision> g_collisions;
std::vector<void (*)(float)> g_ticks;

}  // namespace

void add_tick(void (*fn)(float)) {
    if (fn) {
        g_ticks.push_back(fn);
    }
}

// Зовётся с натива GraftTick, то есть по скриптовому потоку. Обработчик может звать
// движок — там же, где это делает сам скрипт.
void run_ticks(float dt) {
    for (void (*fn)(float) : g_ticks) {
        fn(dt);
    }
}

std::size_t tick_count() {
    return g_ticks.size();
}

namespace {
void* g_root = nullptr;
}

void set_script_root(void* game) {
    if (game) {
        g_root = game;
    }
}

void* script_root() {
    return g_root;
}

namespace {
std::size_t g_faults = 0;
std::string g_last_fault;
// Сколько строк в журнал отдано на один трамплин. Натив, падающий каждый кадр, иначе
// пишет по строке шестьдесят раз в секунду и добивает сервер тем, от чего его спасли.
// Счётчик падений при этом растёт всегда — молчаливая потеря хуже шумной.
std::unordered_map<void*, std::size_t> g_fault_lines;
constexpr std::size_t kMaxLinesPerNative = 3;
}

// Кто упал — ищем по адресу трамплина в реестре: там и имя плагина, и имя натива в
// скрипте. Ничего другого у нас на руках нет, а этого достаточно, чтобы в журнале была
// не «где-то в dwmapi», а «плагин SIXW_GRAFT, натив SeraphNode.Id».
void note_fault(void* impl, std::uint32_t code, const void* at, const char* what) {
    ++g_faults;
    const char* owner = "?";
    const char* class_name = nullptr;
    const char* name = "?";
    for (const plugins::entry& e : registry()) {
        if (e.desc && e.desc->impl == impl) {
            owner = e.owner ? e.owner : "?";
            class_name = e.desc->class_name;
            name = e.desc->name;
            break;
        }
    }
    const std::string who =
        class_name ? std::format("{}.{}", class_name, name) : std::string{name};
    g_last_fault =
        what ? std::format("[{}] {}: исключение — {}", owner, who, what)
             : std::format("[{}] {}: сбой {:#x} по адресу {}", owner, who, code, at);
    const std::size_t said = g_fault_lines[impl]++;
    if (said < kMaxLinesPerNative) {
        graft::log("! " + g_last_fault + " — вызов отменён, игра продолжает работу");
    } else if (said == kMaxLinesPerNative) {
        graft::log("! " + who + ": падает постоянно — дальше молча, счётчик в GraftFaultCount()");
    }
}

std::size_t fault_count() {
    return g_faults;
}

std::string last_fault() {
    return g_last_fault;
}

namespace detail {
std::vector<plugins::entry>& registry_ref() {
    return g_registry;
}
std::vector<row>& rows_ref() {
    return g_rows;
}
std::vector<plugins::collision>& collisions_ref() {
    return g_collisions;
}
}  // namespace detail

const std::vector<plugins::entry>& registry() {
    return g_registry;
}

const std::vector<row>& rows() {
    return g_rows;
}

const std::vector<plugins::collision>& collisions() {
    return g_collisions;
}

// Отметка времени: ею размечены шаги установки. Порядок «хуки -> плагины -> маяк» —
// единственное место в библиотеке с настоящей гонкой, и увидеть его перестановку можно
// только так.
void mark(const char* what) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    graft::log(std::format("[{:02}:{:02}:{:02}.{:03}] {}", t.wHour, t.wMinute, t.wSecond,
                           t.wMilliseconds, what ? what : ""));
}

}  // namespace graft::loader
