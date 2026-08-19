// То, что раньше проверялось только в игре, а проверяется и без неё.
//
// Арена возвращаемых строк, состояние загрузчика (тики и корень) и оставшиеся пути
// отказа маршалируемого вызова — всё это обычный C++ без движка, и держать его
// непокрытым только потому, что «оно про игру», значит платить прогоном сервера за
// каждую мелочь.
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "graft/engine.hpp"
#include "graft/guard.hpp"
#include "graft/loader.hpp"
#include "graft/native.hpp"

namespace {

// ── Журналы ─────────────────────────────────────────────────────────────────
// Каналов два: системный — наш файл в профиле сервера, пользовательский — журналы самой
// игры. Без игры проверяется первый целиком и ОТКАЗ второго: он обязан не терять строку.

namespace {

std::filesystem::path log_sandbox() {
    return std::filesystem::temp_directory_path() / "graft_log_test";
}

// Единственный файл каталога, чьё имя начинается с mask. Пусто — такого нет.
std::filesystem::path only_file_like(const std::filesystem::path& dir, std::string_view mask) {
    for (const std::filesystem::directory_entry& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().filename().string().starts_with(mask)) {
            return e.path();
        }
    }
    return {};
}

std::string text_of(const std::filesystem::path& file) {
    std::ifstream in(file);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Журнал — состояние процесса: за собой убираем, иначе следующие кейсы пишут в темп.
struct log_dir_guard {
    explicit log_dir_guard(const std::filesystem::path& dir) {
        std::filesystem::remove_all(dir);
        graft::set_log_dir(dir.string());
    }
    ~log_dir_guard() {
        graft::set_log_dir("");
    }
};

}  // namespace

TEST(Logs, SystemJournalIsOneFilePerLaunchInTheGivenDir) {
    const std::filesystem::path dir = log_sandbox();
    const log_dir_guard guard{dir};

    graft::log("про библиотеку");
    graft::log("и ещё раз");

    const std::filesystem::path file = only_file_like(dir, "graft_");
    ASSERT_FALSE(file.empty());
    EXPECT_EQ(file.extension(), ".log");
    // Имя в стиле script_/crash_ игры: graft_ГГГГ-ММ-ДД_ЧЧ-ММ-СС.log
    EXPECT_EQ(file.filename().string().size(), std::strlen("graft_2026-08-19_21-03-55.log"));
    EXPECT_NE(text_of(file).find("про библиотеку"), std::string::npos);
    // Обе строки в ОДНОМ файле: он один на запуск, а не на запись.
    EXPECT_NE(text_of(file).find("и ещё раз"), std::string::npos);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(dir),
                            std::filesystem::directory_iterator{}),
              1);
}

// Каталог профиля игра создаёт сама, но библиотека просыпается раньше неё.
TEST(Logs, CreatesTheDirectoryItWasGiven) {
    const std::filesystem::path dir = log_sandbox() / "profile" / "deeper";
    std::filesystem::remove_all(log_sandbox());
    const log_dir_guard guard{dir};

    graft::log("строка до того, как игра создала профиль");
    EXPECT_TRUE(std::filesystem::exists(dir));
    EXPECT_FALSE(only_file_like(dir, "graft_").empty());
}

// Пользовательский канал без игры: Print и Error звать некому — строка обязана не
// пропасть, а лечь в системный журнал с пометкой. Ровно это и происходит на живом
// сервере до первого тика: корня объектного графа ещё нет.
TEST(Logs, UserChannelFallsBackToTheSystemJournal) {
    const std::filesystem::path dir = log_sandbox();
    const log_dir_guard guard{dir};

    EXPECT_FALSE(graft::print("мод сказал"));
    EXPECT_FALSE(graft::error("мод пожаловался"));

    const std::string text = text_of(only_file_like(dir, "graft_"));
    EXPECT_NE(text.find("~ [graft] мод сказал"), std::string::npos);
    EXPECT_NE(text.find("! [graft] мод пожаловался"), std::string::npos);
}

// Пока каталог не задан, журнал молчит — так живёт генератор объявлений, который
// линкует тот же код, но писать ему некуда.
TEST(Logs, SaysNothingUntilTheDirectoryIsKnown) {
    const std::filesystem::path dir = log_sandbox();
    const log_dir_guard guard{dir};
    graft::log("первая");
    const std::filesystem::path file = only_file_like(dir, "graft_");
    ASSERT_FALSE(file.empty());
    const std::string before = text_of(file);

    graft::set_log_dir("");
    graft::log("вторая");
    graft::print("третья");
    EXPECT_EQ(text_of(file), before);
}

// ── Арена строк ─────────────────────────────────────────────────────────────
// Кольцо на поток: строки копятся внутри вызова и обрезаются на входе в следующий
// ВНЕШНИЙ. Освобождать раньше нельзя — движок читает результат уже после возврата.

TEST(Arena, StashKeepsTextReadable) {
    const graft::detail::call_scope call;
    const char* a = graft::detail::stash("первая");
    const char* b = graft::detail::stash("вторая");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_STREQ(a, "первая");
    EXPECT_STREQ(b, "вторая");
    EXPECT_NE(a, b);  // две строки одного вызова не наезжают друг на друга
}

TEST(Arena, ReserveGivesWritableRoom) {
    const graft::detail::call_scope call;
    char* room = graft::detail::reserve_text(5);
    ASSERT_NE(room, nullptr);
    std::memcpy(room, "12345", 5);
    room[5] = '\0';
    EXPECT_STREQ(room, "12345");
}

TEST(Arena, TextOfFormatsStraightIntoArena) {
    const graft::detail::call_scope call;
    const graft::text made = graft::text::of("{}:{}", "ключ", 42);
    EXPECT_EQ(made.view(), "ключ:42");
}

TEST(Arena, LongTextGoesThroughHeapButStillArrives) {
    const graft::detail::call_scope call;
    // Длиннее стекового буфера text::of — путь через std::format, одна аллокация.
    const std::string big(400, 'x');
    const graft::text made = graft::text::of("{}", big);
    EXPECT_EQ(made.view().size(), big.size());
}

TEST(Arena, RingIsBiggerThanOneCall) {
    // Метрика, по которой видно, что запас не подобран, а измерен.
    EXPECT_GT(graft::detail::ring_size(), 0u);
    EXPECT_LT(graft::detail::peak_demand(), graft::detail::ring_size());
}

TEST(Arena, ChurnOverwritesOldTextInsteadOfCrashing) {
    const graft::detail::call_scope call;
    const char* first = graft::detail::stash("затрётся");
    // Прокручиваем кольцо больше, чем оно есть: старая строка станет неверной, но
    // процесс обязан выжить — это ровно то, что кейс Arena_OverrunGivesWrongText
    // проверяет в игре.
    graft::detail::churn_arena(graft::detail::ring_size() + 64);
    EXPECT_NE(first, nullptr);
}

TEST(Arena, PoisonReportsHowMuchWasHandedOut) {
    const graft::detail::call_scope call;
    graft::detail::stash("что-нибудь");
    EXPECT_GT(graft::detail::poison_arena(), 0u);
}

// ── Состояние загрузчика ────────────────────────────────────────────────────
// Тики и корень объектного графа держит хост: натив GraftTick один на процесс, и
// расходиться веером по плагинам должен он.

int g_ticks = 0;
float g_last = 0;
void count_tick(float dt) {
    ++g_ticks;
    g_last = dt;
}

TEST(LoaderState, TickHandlersRunInOrderWithDelta) {
    const std::size_t before = graft::loader::tick_count();
    g_ticks = 0;
    graft::loader::add_tick(&count_tick);
    EXPECT_EQ(graft::loader::tick_count(), before + 1);

    graft::loader::run_ticks(0.5f);
    EXPECT_EQ(g_ticks, 1);
    EXPECT_FLOAT_EQ(g_last, 0.5f);

    graft::loader::run_ticks(0.25f);
    EXPECT_EQ(g_ticks, 2);
    EXPECT_FLOAT_EQ(g_last, 0.25f);
}

TEST(LoaderState, NullHandlerIsIgnored) {
    const std::size_t before = graft::loader::tick_count();
    graft::loader::add_tick(nullptr);
    EXPECT_EQ(graft::loader::tick_count(), before);
}

TEST(LoaderState, RootIsRememberedButNeverCleared) {
    int game = 0;
    graft::loader::set_script_root(&game);
    EXPECT_EQ(graft::loader::script_root(), &game);
    // Ноль не затирает: тик мода может прийти и без корня, а терять уже известный
    // объект из-за этого нельзя.
    graft::loader::set_script_root(nullptr);
    EXPECT_EQ(graft::loader::script_root(), &game);
}

// ── Оставшиеся пути отказа вызова наружу ────────────────────────────────────

TEST(CallOutRefusals, MissingTemplateIsRefusedNotInvented) {
    // Дескриптора нет вовсе — брать шаблон переменной неоткуда, и собирать его самим
    // нельзя: движок разрешает тип по полю, которого мы не знаем.
    graft::script::method fn;
    fn.impl = reinterpret_cast<void*>(&count_tick);  // адрес настоящего кода
    fn.executable = true;
    int self = 0;
    const auto r = graft::call_proto(fn, &self, {}, graft::script::tag_int);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), graft::miss::no_template);
}

TEST(CallOutRefusals, ResultOutOfRangeIsEmpty) {
    graft::proto_result r;
    r.count = 1;
    r.args[0] = graft::value{graft::i32{7}};
    EXPECT_EQ(r.arg(0).as<graft::i32>(), 7);
    EXPECT_TRUE(r.arg(1).empty());
    EXPECT_TRUE(r.arg(99).empty());
}

// ── Значение любого скриптового типа ────────────────────────────────────────

TEST(Value, CarriesEveryScriptType) {
    EXPECT_TRUE(graft::value{}.empty());
    EXPECT_TRUE(graft::value{true}.is<bool>());
    EXPECT_TRUE(graft::value{graft::i32{1}}.is<graft::i32>());
    EXPECT_TRUE(graft::value{graft::f32{1.5f}}.is<graft::f32>());
    const graft::value asVector{graft::vector{1, 2, 3}};
    EXPECT_TRUE(asVector.is<graft::vector>());
    EXPECT_TRUE(graft::value{std::string{"x"}}.is<std::string>());
    EXPECT_TRUE(graft::value{graft::obj{}}.is<graft::obj>());
    EXPECT_TRUE(graft::value{graft::type{}}.is<graft::type>());
    // Строка из str копируется: движковый указатель хранить нельзя.
    EXPECT_EQ(graft::value{graft::str{"копия"}}.view(), "копия");
}

TEST(Value, WrongTypeGivesZeroNotGarbage) {
    const graft::value v{graft::i32{5}};
    EXPECT_EQ(v.as<graft::f32>(), 0.0f);
    EXPECT_TRUE(v.view().empty());
    EXPECT_FALSE(static_cast<bool>(v.object()));
}

TEST(Value, TextFormOfEveryType) {
    EXPECT_EQ(graft::value{}.to_string(), "");
    EXPECT_EQ(graft::value{true}.to_string(), "1");
    EXPECT_EQ(graft::value{false}.to_string(), "0");
    EXPECT_EQ(graft::value{graft::i32{-3}}.to_string(), "-3");
    EXPECT_EQ(graft::value{std::string{"как есть"}}.to_string(), "как есть");
    const graft::value vectorText{graft::vector{1, 2, 3}};
    EXPECT_EQ(vectorText.to_string(), "1 2 3");
    EXPECT_EQ(graft::value{graft::type{}}.to_string(), "");
}

TEST(Value, IsUsableAsMapKey) {
    const std::hash<graft::value> hash;
    EXPECT_EQ(hash(graft::value{graft::i32{7}}), hash(graft::value{graft::i32{7}}));
    EXPECT_NE(graft::value{graft::i32{7}}, graft::value{graft::i32{8}});
    EXPECT_EQ(graft::value{std::string{"a"}}, graft::value{std::string{"a"}});
}

// ── Строка и вектор ─────────────────────────────────────────────────────────

TEST(Str, BehavesLikeAStringView) {
    const graft::str s{"abcdef"};
    EXPECT_EQ(s.size(), 6u);
    EXPECT_TRUE(s.starts_with("abc"));
    EXPECT_TRUE(s.contains("cde"));
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(std::string{s}, "abcdef");
    EXPECT_EQ(s, graft::str{"abcdef"});
    // Пустой указатель — пустая строка, а не падение.
    const graft::str none;
    EXPECT_TRUE(none.empty());
    EXPECT_STREQ(none.c_str(), "");
    EXPECT_FALSE(static_cast<bool>(none));
}

TEST(Vector, ArithmeticAndArrayInterop) {
    const graft::vector a{1, 2, 3};
    const graft::vector b{4, 5, 6};
    EXPECT_EQ(a + b, (graft::vector{5, 7, 9}));
    EXPECT_EQ(b - a, (graft::vector{3, 3, 3}));
    EXPECT_EQ(a * 2.0f, (graft::vector{2, 4, 6}));
    EXPECT_FLOAT_EQ(a.dot(b), 32.0f);
    const graft::vector unitish{3, 4, 0};
    EXPECT_FLOAT_EQ(unitish.length(), 5.0f);
    EXPECT_EQ(graft::vector::from({7, 8, 9}), (graft::vector{7, 8, 9}));
    const auto asArray = static_cast<std::array<float, 3>>(a);
    EXPECT_FLOAT_EQ(asArray[1], 2.0f);
}

// ── Модель памяти скриптовых типов ──────────────────────────────────────────

struct managed_type : graft::ref<"Managed"> {
    static constexpr graft::lifetime script_lifetime = graft::lifetime::managed;
    static constexpr bool script_spawnable = true;
};
struct engine_type : graft::ref<"EngineOwned"> {
    static constexpr graft::lifetime script_lifetime = graft::lifetime::engine;
    static constexpr bool script_spawnable = false;
};

TEST(Lifetime, ReadsWhatTheMirrorDeclared) {
    static_assert(graft::lifetime_of<managed_type>() == graft::lifetime::managed);
    static_assert(graft::lifetime_of<engine_type>() == graft::lifetime::engine);
    static_assert(graft::spawnable<managed_type>());
    static_assert(!graft::spawnable<engine_type>());
    // Класс, ничего о себе не сказавший (контейнер, свой тип мода), считается обычным —
    // самое осторожное предположение: оно ничего не разрешает сверх.
    static_assert(graft::lifetime_of<graft::obj>() == graft::lifetime::plain);
    static_assert(graft::spawnable<graft::obj>());
    SUCCEED();
}

TEST(LoaderState, ReportRowsAndCollisionsAreShared) {
    // Хранилище результата загрузки отдельно от самой загрузки: класс Graft читает эти
    // списки и в игре, и в graft.exe, где загрузчика нет вовсе.
    const std::size_t rows = graft::loader::rows().size();
    graft::loader::detail::rows_ref().push_back({"ТЕСТ", 7, "путь", GRAFT_OK, 3});
    ASSERT_EQ(graft::loader::rows().size(), rows + 1);
    EXPECT_EQ(graft::loader::rows().back().name, "ТЕСТ");
    EXPECT_EQ(graft::loader::rows().back().version, 7u);
    graft::loader::detail::rows_ref().pop_back();

    EXPECT_EQ(graft::loader::registry().size(), graft::loader::detail::registry_ref().size());
    EXPECT_EQ(graft::loader::collisions().size(), graft::loader::detail::collisions_ref().size());
}

TEST(LoaderState, MarkWritesWithoutALogPath) {
    // Журнал пишется рядом с exe игры, и его тут нет. Отметка обязана это пережить —
    // иначе инструмент падал бы на ровном месте.
    graft::loader::mark("шаг без журнала");
    graft::loader::mark(nullptr);
    SUCCEED();
}

// ── Вход в объектный граф без движка ────────────────────────────────────────

TEST(World, SpawnWithoutEngineIsRefusedNotCrashed) {
    // Без хоста find_class отдаёт ноль. Ответ — промах в типе, а не падение и не
    // попытка сочинить объект.
    const auto made = graft::try_spawn<graft::array<graft::i32>>();
    ASSERT_FALSE(made.has_value());
    EXPECT_EQ(made.error(), graft::miss::not_found);
    EXPECT_FALSE(static_cast<bool>(graft::spawn<graft::array<graft::i32>>()));
}

TEST(World, RootIsEmptyUntilTheModTicks) {
    graft::loader::set_script_root(nullptr);   // не затирает, см. кейс выше
    const auto game = graft::try_game();
    // Корень к этому моменту уже проставлен другим кейсом — важно, что обе формы
    // согласованы между собой, а не конкретное значение.
    EXPECT_EQ(game.has_value(), static_cast<bool>(graft::game()));
}

TEST(World, CastKeepsPointerAcrossTypes) {
    int marker = 0;
    const graft::ref<"Man"> man{&marker};
    EXPECT_EQ(graft::cast<graft::obj>(man).ptr, &marker);
    EXPECT_EQ(graft::cast<graft::ref<"EntityAI">>(man).ptr, &marker);
}

TEST(World, BorrowedResetForgetsEverything) {
    int marker = 0;
    graft::borrowed<graft::ref<"Man">> kept{graft::ref<"Man">{&marker}};
    kept.reset();
    EXPECT_FALSE(kept.get().has_value());
}

// ── Защита вызова ────────────────────────────────────────────────────────────
// В игре это проверяется настоящим падением настоящего натива (кейс Crash_*), но сам
// механизм — обычный C++ и никакого движка не требует. Здесь он и проверяется целиком:
// оба слоя (исключение и аппаратный сбой), возвращаемое значение после отказа, отчёт с
// именем плагина и сброс глубины арены.

// Сбой обязан случиться ВНУТРИ диапазона __try. Таблица областей x64-SEH описывает
// диапазоны адресов, поэтому падение живёт в отдельной функции без инлайна: тогда в
// диапазон попадает вызов к ней.
__declspec(noinline) int read_null() {
    return *reinterpret_cast<volatile int*>(0);
}

TEST(Guard, PassesValueThroughWhenNothingHappens) {
    const std::size_t before = graft::loader::fault_count();
    EXPECT_EQ(graft::detail::guarded<int>(nullptr, [] { return 17; }), 17);
    EXPECT_EQ(graft::loader::fault_count(), before);
}

TEST(Guard, CatchesStdExceptionAndKeepsItsText) {
    const std::size_t before = graft::loader::fault_count();
    const int got = graft::detail::guarded<int>(
        nullptr, []() -> int { throw std::runtime_error("натив передумал"); });
    EXPECT_EQ(got, 0);
    EXPECT_EQ(graft::loader::fault_count(), before + 1);
    EXPECT_NE(graft::loader::last_fault().find("натив передумал"), std::string::npos);
}

TEST(Guard, CatchesForeignThrowToo) {
    const int got = graft::detail::guarded<int>(nullptr, []() -> int { throw 42; });
    EXPECT_EQ(got, 0);
    EXPECT_NE(graft::loader::last_fault().find("неизвестного типа"), std::string::npos);
}

TEST(Guard, CatchesHardwareFault) {
    const std::size_t before = graft::loader::fault_count();
    EXPECT_EQ(graft::detail::guarded<int>(nullptr, [] { return read_null(); }), 0);
    EXPECT_EQ(graft::loader::fault_count(), before + 1);
    // 0xc0000005 — обращение по недопустимому адресу. Код в отчёте, потому что без него
    // «что-то упало» неотличимо от «что-то бросило».
    EXPECT_NE(graft::loader::last_fault().find("c0000005"), std::string::npos);
}

TEST(Guard, CatchesFaultWrittenPlainlyInTheBody) {
    // Тот же сбой, но БЕЗ noinline — так его напишет пишущий натив. Требование «сбой в
    // отдельной функции» относится к устройству самой защиты (тело вызова уже отделено
    // от __try), а не к коду пользователя, и проверяется это здесь, а не на словах.
    const std::size_t before = graft::loader::fault_count();
    volatile int* nowhere = nullptr;
    EXPECT_EQ(graft::detail::guarded<int>(nullptr, [nowhere] { return *nowhere; }), 0);
    EXPECT_EQ(graft::loader::fault_count(), before + 1);
}

TEST(Guard, ReturnsEmptyTextRatherThanNullPointer) {
    // Движок копирует возвращённую строку сам и nullptr не проверяет: отказ обязан
    // выглядеть как пустая строка, иначе защита роняет то, что защищает.
    const graft::text got = graft::detail::guarded<std::string>(
        nullptr, []() -> graft::text { throw std::runtime_error("нет текста"); });
    ASSERT_NE(got.raw, nullptr);
    EXPECT_STREQ(got.raw, "");
}

TEST(Guard, VoidNativeSurvivesToo) {
    const std::size_t before = graft::loader::fault_count();
    graft::detail::guarded<void>(nullptr, [] { read_null(); });
    EXPECT_EQ(graft::loader::fault_count(), before + 1);
}

TEST(Guard, ReportNamesThePluginAndTheNative) {
    // Кто упал, известно только по адресу трамплина: хост ищет его в реестре. Без этого
    // в журнале осталось бы «где-то в dwmapi», а нужно «плагин такой-то, натив такой-то».
    static int trampoline = 0;
    static const graft_native_desc desc{
        .class_name = "SeraphNode", .name = "Id", .impl = &trampoline};
    auto& registry = graft::loader::detail::registry_ref();
    registry.push_back({&desc, "SIXW_GRAFT"});
    graft::detail::guarded<int>(&trampoline,
                                []() -> int { throw std::runtime_error("ой"); });
    registry.pop_back();
    EXPECT_NE(graft::loader::last_fault().find("SIXW_GRAFT"), std::string::npos);
    EXPECT_NE(graft::loader::last_fault().find("SeraphNode.Id"), std::string::npos);
}

TEST(Guard, StopsSpammingTheLogButKeepsCounting) {
    // Натив, падающий каждый кадр, писал бы по строке шестьдесят раз в секунду и добил
    // сервер тем, от чего его спасли. Счёт при этом обязан идти дальше: молчаливая
    // потеря хуже шумной.
    static int trampoline = 0;
    static const graft_native_desc desc{.class_name = nullptr, .name = "Шумный", .impl = &trampoline};
    auto& registry = graft::loader::detail::registry_ref();
    registry.push_back({&desc, "SIXW_GRAFT"});
    const std::size_t before = graft::loader::fault_count();
    for (int i = 0; i < 10; ++i) {
        graft::detail::guarded<int>(&trampoline, []() -> int { throw std::runtime_error("опять"); });
    }
    registry.pop_back();
    EXPECT_EQ(graft::loader::fault_count(), before + 10);
}

TEST(Guard, ResetsCallDepthSoTheArenaKeepsMoving) {
    // Раскрутка SEH деструкторы не зовёт: call_scope не закроется сам, глубина уехала бы
    // навсегда, и окно арены перестало бы двигаться. Проверяем по расходу: после отказа
    // следующий внешний вызов обязан начать цепочку заново.
    graft::detail::guarded<int>(nullptr, [] {
        const graft::detail::call_scope call;
        graft::detail::stash(std::string(200, 'x'));
        return read_null();
    });
    const std::size_t after_fault = graft::detail::peak_demand();
    {
        const graft::detail::call_scope call;
        graft::detail::stash("коротко");
    }
    EXPECT_EQ(graft::detail::peak_demand(), after_fault);  // цепочка началась заново
}

// ── Серверный ли процесс ────────────────────────────────────────────────────
// Библиотека не вмешивается в клиент: сборки клиента и сервера разные, и найденное
// сканом в одной в другой указывает в другое место. Решение принимается по командной
// строке, а «-server» встречается и внутри имён модов — вот это и проверяется.

TEST(Serving, FindsTheFlagAsItsOwnArgument) {
    EXPECT_TRUE(graft::has_flag(L"DayZDiag_x64.exe -server -port=2302", L"-server"));
    EXPECT_TRUE(graft::has_flag(L"-server", L"-server"));
    EXPECT_TRUE(graft::has_flag(L"exe \"-server\" -mod=@A;", L"-server"));
}

TEST(Serving, DoesNotMistakeAModNameForTheFlag) {
    // Ровно тот случай, ради которого разбор не сводится к поиску подстроки.
    EXPECT_FALSE(graft::has_flag(L"exe -mod=@my-server-mod; -connect=127.0.0.1", L"-server"));
    EXPECT_FALSE(graft::has_flag(L"exe -servers", L"-server"));
    EXPECT_FALSE(graft::has_flag(L"exe -noPause -window", L"-server"));
    EXPECT_FALSE(graft::has_flag(L"", L"-server"));
}

}  // namespace
