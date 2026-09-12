// Тесты сканера точек регистрации: собираем синтетический "образ" из двух секций
// (строки + код) и проверяем, что discover() находит те же три функции, что руками
// найдены в exe. На настоящих бинарях тот же алгоритм сверяется через
// re/scripts/discover.py (сухой прогон в IDA).
#include <gtest/gtest.h>

#include <windows.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <vector>

#include "graft/scan.hpp"
#include "graft/types.hpp"

namespace {

constexpr std::uintptr_t kStrBase   = 0x10000000;
constexpr std::uintptr_t kCodeBase  = 0x20000000;
constexpr std::uintptr_t kRegGlobal = kCodeBase + 0x1000;
constexpr std::uintptr_t kRegMethod = kCodeBase + 0x2000;
constexpr std::uintptr_t kFindClass = kCodeBase + 0x3000;

struct fake_image {
    std::vector<std::uint8_t> strings = std::vector<std::uint8_t>(0x100, 0);
    std::vector<std::uint8_t> code    = std::vector<std::uint8_t>(0x4000, 0);

    std::uintptr_t put_str(std::size_t off, const char* text) {
        std::memcpy(strings.data() + off, text, std::strlen(text) + 1);
        return kStrBase + off;
    }

    // Инструкция пишется ПО ЕЁ ЖЕ СИГНАТУРЕ: сверяемые байты как есть, дырка — посчитанным
    // смещением от конца инструкции. Так фикстура не может разойтись с тем, что ищет сканер.
    template <std::size_t N>
    void put_insn(std::size_t off, const graft::scan::pattern<N>& insn, std::uintptr_t target) {
        std::memcpy(code.data() + off, insn.value.data(), N);
        const std::int32_t disp = static_cast<std::int32_t>(target - (kCodeBase + off + N));
        std::memcpy(code.data() + off + insn.hole, &disp, 4);
    }

    // Сырые байты: ими кладутся инструкции, которых кодировщик не знает намеренно
    // (векторные), — в сигнатуре они тоже записаны байтами.
    void put_bytes(std::size_t off, std::initializer_list<std::uint8_t> raw) {
        std::size_t i = 0;
        for (const std::uint8_t b : raw) {
            code[off + i++] = b;
        }
    }

    void put_call(std::size_t off, std::uintptr_t target) { put_rel32(off, 0xE8, target); }
    void put_jmp(std::size_t off, std::uintptr_t target) { put_rel32(off, 0xE9, target); }

    void put_rel32(std::size_t off, std::uint8_t opcode, std::uintptr_t target) {
        code[off]               = opcode;
        const std::int32_t disp = static_cast<std::int32_t>(target - (kCodeBase + off + 5));
        std::memcpy(code.data() + off + 1, &disp, 4);
    }

    // dword по смещению в секции данных — ею собираются структуры RTTI.
    void put_u32(std::size_t off, std::uint32_t v) { std::memcpy(strings.data() + off, &v, 4); }
    void put_ptr(std::size_t off, std::uintptr_t v) {
        std::memcpy(strings.data() + off, &v, sizeof v);
    }

    std::vector<graft::scan::view> sections() {
        return {{strings, kStrBase, false}, {code, kCodeBase, true}};
    }
};

// Раскладка как в движке: FindClass(ctx,"Math"); RegisterMethod(ctx,cls,"GetNumberOfSetBits",..);
// ... RegisterGlobal(ctx,"MemoryValidation",..)
fake_image make_image() {
    fake_image           img;
    const std::uintptr_t s_global = img.put_str(0x00, "MemoryValidation");
    const std::uintptr_t s_method = img.put_str(0x40, "GetNumberOfSetBits");

    img.put_call(0x00, kFindClass);
    img.put_insn(0x05, graft::scan::lea_r8, s_method);
    img.put_call(0x0C, kRegMethod);
    img.put_insn(0x11, graft::scan::lea_rdx, s_global);
    img.put_call(0x18, kRegGlobal);
    return img;
}

TEST(Scan, FindsCStringExactly) {
    fake_image img;
    img.put_str(0x00, "Memory");
    img.put_str(0x10, "MemoryValidation");
    const graft::scan::view v{img.strings, kStrBase, false};

    EXPECT_EQ(v.find_cstr("MemoryValidation"), kStrBase + 0x10);
    EXPECT_EQ(v.find_cstr("Validation"), 0u); // хвост чужой строки — не совпадение
    EXPECT_EQ(v.find_cstr("Nope"), 0u);
}

TEST(Scan, DiscoversAllThreeEntryPoints) {
    fake_image             img = make_image();
    const graft::scan::api api = graft::scan::discover(img.sections());

    ASSERT_TRUE(static_cast<bool>(api));
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(api.register_global), kRegGlobal);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(api.register_method), kRegMethod);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(api.find_class), kFindClass);
}

// Байт 0xE8 может оказаться и внутри чужого смещения: такой "вызов" ведёт мимо кода
// и не должен уводить поиск с настоящего call'а.
TEST(Scan, IgnoresFalseCallBytes) {
    fake_image img = make_image();
    img.put_call(0x18, 0x99999999); // цель вне всех секций — мусор
    img.put_call(0x1D, kRegGlobal); // настоящий вызов дальше по коду

    const graft::scan::api api = graft::scan::discover(img.sections());
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(api.register_global), kRegGlobal);
}

// ── Точка входа кадра ────────────────────────────────────────────────────────
// Раз в кадр движок зовёт скриптовый OnUpdate, и graft вешается на ПОДГОТОВКУ этого вызова
// (хук на саму функцию смертелен — она общая, и детур затирает xmm чужим вызовам).
//
// Место опознаётся четырьмя приметами подряд, и до этих кейсов вся функция проверялась
// только выездом на живую игру: строки-маяка у неё нет, поэтому промах выглядел как
// «кадровый тик просто не работает».
constexpr std::uintptr_t kFindIndex = kCodeBase + 0x1000;
constexpr std::uintptr_t kPrepare   = kCodeBase + 0x2000;
constexpr std::uintptr_t kIndexVar  = kStrBase + 0x80;

// Раскладка как в CGame::Update: имя -> поиск индекса -> float-аргумент -> кэш индекса ->
// сборка вызова. Маркер float кладётся, только если его просят.
fake_image make_frame_image(bool with_float) {
    fake_image           img;
    const std::uintptr_t name = img.put_str(0x00, "OnUpdate");
    constexpr std::size_t site = 0x100;
    img.put_insn(site, graft::scan::lea_rdx, name);              // lea rdx,"OnUpdate"
    img.put_call(site + 7, kFindIndex);                          // найти индекс по имени
    if (with_float) {
        img.put_bytes(site + 12, {0xF3, 0x0F, 0x5A, 0xC6});      // cvtss2sd xmm0,xmm6
    }
    img.put_insn(site + 16, graft::scan::sig<"89 05 [disp32]">,  // mov [rip+d],eax
                 kIndexVar);
    img.put_call(site + 22, kPrepare);                           // собрать вызов <- цель
    return img;
}

TEST(Scan, FindsFrameEntryByItsFourMarks) {
    fake_image img = make_frame_image(true);

    const graft::scan::frame_entry e = graft::scan::find_frame_entry(img.sections());
    ASSERT_TRUE(static_cast<bool>(e));
    // Перехватывается ВТОРОЙ вызов — сборка кадра, а не поиск индекса.
    EXPECT_EQ(e.site, kCodeBase + 0x100 + 22);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(e.prepare), kPrepare);
    // Кэш индекса берётся из дырки: адрес считается от КОНЦА инструкции.
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(e.index), kIndexVar);
}

// Строка "OnUpdate" в образе одна на всех, а ссылок на неё несколько: виджеты, техника,
// игра. Нужную отличает ровно float-аргумент — без него это не кадр, и ответ обязан быть
// отказом, а не ближайшим похожим местом.
TEST(Scan, FrameEntryNeedsTheFloatMark) {
    fake_image img = make_frame_image(false);
    EXPECT_FALSE(static_cast<bool>(graft::scan::find_frame_entry(img.sections())));
}

// Цель `call` обязана лежать в исполняемой секции: байт 0xE8 встречается и внутри чужого
// смещения, и такой «вызов» ведёт куда попало.
TEST(Scan, FrameEntryRefusesCallOutsideCode) {
    fake_image img = make_frame_image(true);
    img.put_call(0x100 + 22, 0x99999999);
    EXPECT_FALSE(static_cast<bool>(graft::scan::find_frame_entry(img.sections())));
}

TEST(Scan, EmptyImageIsHarmless) {
    const std::vector<graft::scan::view> none;
    EXPECT_FALSE(static_cast<bool>(graft::scan::discover(none)));
}

// ── Сигнатура с джокерами ────────────────────────────────────────────────────
// В прологе движковой функции сверять можно не всё: там, где запечено смещение поля или
// выбранный компилятором игры регистр, байт обязан считаться любым.
TEST(Scan, MatchesSignatureWithWildcards) {
    static const std::uint8_t body[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83};
    const auto ea = reinterpret_cast<std::uintptr_t>(body);

    EXPECT_TRUE(graft::scan::matches(ea, graft::scan::sig<"48 89 5C 24 ?? 57">));
    EXPECT_TRUE(graft::scan::matches(ea, graft::scan::sig<"48 89">));
    EXPECT_TRUE(graft::scan::matches(ea, graft::scan::sig<"?? ?? ??">));
    EXPECT_FALSE(graft::scan::matches(ea, graft::scan::sig<"48 89 5C 24 11 57">));
    EXPECT_FALSE(graft::scan::matches(ea, graft::scan::sig<"49">));
}

// Маскированный байт: совпасть обязаны только биты маски.
TEST(Scan, MasksCompareOnlyTheBitsThatMatter) {
    static const std::uint8_t rax[] = {0x48, 0x8D, 0x05, 0x11, 0x22, 0x33, 0x44};
    static const std::uint8_t rdx[] = {0x48, 0x8D, 0x15, 0x11, 0x22, 0x33, 0x44};
    static const std::uint8_t other[] = {0x48, 0x8D, 0x41, 0x08, 0x00, 0x00, 0x00};
    constexpr auto any_reg = graft::scan::sig<"48 8D 05&C7">;

    EXPECT_TRUE(graft::scan::matches(reinterpret_cast<std::uintptr_t>(rax), any_reg));
    EXPECT_TRUE(graft::scan::matches(reinterpret_cast<std::uintptr_t>(rdx), any_reg));
    // mod и rm в маске остались, поэтому другая форма адресации не совпадает — в отличие
    // от `??`, под который подошла бы любая.
    EXPECT_FALSE(graft::scan::matches(reinterpret_cast<std::uintptr_t>(other), any_reg));
    EXPECT_TRUE(graft::scan::matches(reinterpret_cast<std::uintptr_t>(other),
                                     graft::scan::sig<"48 8D ??">));
}

// Секция ищет по своим байтам: `base` у неё бывает и синтетическим (так собраны фикстуры
// сьюты), и спрашивать у системы, отображена ли такая страница, бессмысленно.
TEST(Scan, ViewFindsInsideItsOwnBytesWithSyntheticBase) {
    std::vector<std::uint8_t> body(0x100, 0);
    body[0x20] = 0x8B;
    body[0x21] = 0x41;
    body[0x22] = 0x38;
    const graft::scan::view v{body, kCodeBase, true};

    const auto at = v.find(kCodeBase, 0x100, graft::scan::sig<"8B 41 [disp8]">);
    ASSERT_TRUE(at.has_value());
    EXPECT_EQ(at->site, kCodeBase + 0x20);
    EXPECT_EQ(at->value, 0x38);
    // Окно обрезается концом секции, а не уезжает за неё.
    EXPECT_TRUE(v.find(kCodeBase, 0x10000, graft::scan::sig<"8B 41 [disp8]">).has_value());
    EXPECT_FALSE(v.find(kCodeBase, 0x20, graft::scan::sig<"8B 41 [disp8]">).has_value());
}

// Кандидат мог приехать из мусорного смещения — сверка обязана ответить «не совпало», а
// не уронить процесс на чтении по нулю.
TEST(Scan, MatchesRefusesUnreadableAddress) {
    EXPECT_FALSE(graft::scan::matches(0, graft::scan::sig<"48">));
    EXPECT_FALSE(graft::scan::matches(0xDEADBEEF, graft::scan::sig<"48">));
    EXPECT_FALSE(graft::scan::find(0, 0x20, graft::scan::sig<"48">).has_value());
}

// Адрес внутри функции ничем не выровнен: сверка сигнатуры со смещения 1 обязана
// работать так же, как с начала. Это отдельный кейс, потому что «похоже на указатель»
// требует выравнивания по 8, а «страница читается» — нет, и спутать их легко.
TEST(Scan, MatchesWorksAtUnalignedAddress) {
    static const std::uint8_t body[] = {0x00, 0x48, 0x89, 0x5C, 0x24, 0x10};
    const auto ea = reinterpret_cast<std::uintptr_t>(body) + 1;
    EXPECT_TRUE(graft::scan::matches(ea, graft::scan::sig<"48 89 5C">));
}

// ── Разбор сигнатуры на компиляции ───────────────────────────────────────────
// Строка копируется из отладчика как есть. Длину, вид байтов и ширину дырки проверяет
// компилятор, а не отладка на живом сервере.
TEST(Scan, SignatureLiteralIsParsedAtCompileTime) {
    static_assert(graft::scan::sig<"48 89 5C">.size() == 3);
    static_assert(!graft::scan::sig<"48 ?? 57">.checked(1), "?? — это любой байт");
    static_assert(graft::scan::sig<"4a">.value[0] == 0x4A, "регистр цифр значения не имеет");
    // Лишние пробелы — не ошибка: строку копируют из отладчика, а он выравнивает столбцы.
    static_assert(graft::scan::sig<"  48   89  ">.size() == 2);
    static_assert(!graft::scan::sig<"48 ? 57">.checked(1), "одиночный ? — тот же джокер");

    // Дырка занимает в сигнатуре свою ширину и сверке не мешает — внутри неё любые байты.
    static_assert(graft::scan::sig<"8B 41 [disp8]">.size() == 3);
    static_assert(graft::scan::sig<"48 8B 81 [disp32]">.size() == 7);
    static_assert(graft::scan::sig<"48 8B 81 [disp32]">.hole == 3);
    static_assert(graft::scan::sig<"48 8B 81 [disp32]">.hole_size == 4);
    static_assert(!graft::scan::sig<"48 8B 81 [disp32]">.checked(4));
    // Слово в скобках — для читателя; парсер берёт из него только число.
    static_assert(graft::scan::sig<"3D [imm32]">.hole_size == graft::scan::sig<"3D [32]">.hole_size);
    // Сигнатура без дырки так и говорит: ширина ноль.
    static_assert(graft::scan::sig<"48 89">.hole_size == 0);

    // Часть байта: сверяются только биты маски. Так пишется «поле reg в modrm любое, а
    // форма адресации вот такая» — целиком байт здесь сверять нельзя, а целиком отпускать
    // нельзя тем более.
    static_assert(graft::scan::sig<"48 8D 05&C7">.mask[2] == 0xC7);
    static_assert(graft::scan::sig<"48 8D 05&C7">.value[2] == 0x05);
    static_assert(graft::scan::sig<"48 8D 05&C7">.checked(2), "маска не ноль — байт сверяется");

    // Мусор в строке — ошибка КОМПИЛЯЦИИ (consteval не может бросить), поэтому кейсов на
    // «4» вместо «48», на `[disp]` без ширины и на две дырки здесь нет и быть не может:
    // они не собрались бы.
}

// ── Поиск в окне и смещение из дырки ─────────────────────────────────────────
// Так берутся смещения полей движка: инструкция ищется в теле функции, а ответ лежит
// внутри неё. Раньше ширину ответа держало ИМЯ функции (disp8_of против disp32_of), и
// промах мимо нужной читал четыре байта там, где лежит один.
TEST(Scan, FindReadsTheHoleAtItsOwnWidth) {
    // mov eax,[rcx+38h] ; mov rax,[rcx+2C8h] ; mov eax,[rcx+40h]
    static const std::uint8_t body[] = {0x8B, 0x41, 0x38,
                                        0x48, 0x8B, 0x81, 0xC8, 0x02, 0x00, 0x00,
                                        0x8B, 0x41, 0x40};
    const auto ea = reinterpret_cast<std::uintptr_t>(body);

    const auto first = graft::scan::find(ea, sizeof body, graft::scan::sig<"8B 41 [disp8]">);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->site, ea);
    EXPECT_EQ(first->value, 0x38);

    // nth: второе такое же чтение — другое поле, и брать молча первое нельзя.
    const auto second = graft::scan::find(ea, sizeof body, graft::scan::sig<"8B 41 [disp8]">, 2);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->site, ea + 10);
    EXPECT_EQ(second->value, 0x40);
    EXPECT_FALSE(graft::scan::find(ea, sizeof body, graft::scan::sig<"8B 41 [disp8]">, 3));

    // disp32 читается целиком, а не первым байтом.
    const auto wide = graft::scan::find(ea, sizeof body, graft::scan::sig<"48 8B 81 [disp32]">);
    ASSERT_TRUE(wide.has_value());
    EXPECT_EQ(wide->site, ea + 3);
    EXPECT_EQ(wide->value, 0x2C8);
}

// Смещение x86 знаковое, и это не формальность: disp8 0x80 — это -128. Раньше disp8_of
// расширял его нулями и молча превращал в +128.
TEST(Scan, HoleIsSignExtended) {
    static const std::uint8_t body[] = {0x8B, 0x41, 0x80};
    const auto ea = reinterpret_cast<std::uintptr_t>(body);
    const auto at = graft::scan::find(ea, sizeof body, graft::scan::sig<"8B 41 [disp8]">);
    ASSERT_TRUE(at.has_value());
    EXPECT_EQ(at->value, -128);
}

// Окно кончается там, где сказано: инструкция, начинающаяся за его краем, не считается,
// иначе разбор одной функции цепляет соседнюю.
TEST(Scan, FindStaysInsideTheWindow) {
    static const std::uint8_t body[] = {0x90, 0x90, 0x90, 0x8B, 0x41, 0x38};
    const auto ea = reinterpret_cast<std::uintptr_t>(body);
    EXPECT_TRUE(graft::scan::find(ea, 6, graft::scan::sig<"8B 41 [disp8]">).has_value());
    EXPECT_FALSE(graft::scan::find(ea, 5, graft::scan::sig<"8B 41 [disp8]">).has_value());
}

// Сигнатура без дырки ищется так же, просто отвечать нечем, кроме адреса.
TEST(Scan, FindWithoutAHoleReturnsOnlyTheSite) {
    static const std::uint8_t body[] = {0x90, 0x48, 0x8B, 0xC4};
    const auto ea = reinterpret_cast<std::uintptr_t>(body);
    const auto at = graft::scan::find(ea, sizeof body, graft::scan::sig<"48 8B C4">);
    ASSERT_TRUE(at.has_value());
    EXPECT_EQ(at->site, ea + 1);
    EXPECT_EQ(at->value, 0);
}

// ── Цели переходов в окне ────────────────────────────────────────────────────
TEST(Scan, CollectsRel32TargetsAndDropsGarbage) {
    fake_image img;
    img.put_call(0x100, kCodeBase + 0x200);
    img.put_jmp(0x110, kCodeBase + 0x300);
    img.put_call(0x120, 0x99999999);        // цель вне всех секций — мусор
    img.put_call(0x140, kCodeBase + 0x400); // за краем окна 0x40
    const std::vector<graft::scan::view> sections = img.sections();

    EXPECT_EQ(graft::scan::rel32_targets(sections, kCodeBase + 0x100, 0x40),
              (std::vector<std::uintptr_t>{kCodeBase + 0x200}));
    EXPECT_EQ(graft::scan::rel32_targets(sections, kCodeBase + 0x100, 0x40, 0xE9),
              (std::vector<std::uintptr_t>{kCodeBase + 0x300}));
    // Окно кончается там, где сказано: иначе разбор одной функции цеплял бы соседнюю.
    EXPECT_EQ(graft::scan::rel32_targets(sections, kCodeBase + 0x100, 0x80),
              (std::vector<std::uintptr_t>{kCodeBase + 0x200, kCodeBase + 0x400}));
    EXPECT_TRUE(graft::scan::rel32_targets(sections, 0x99999999, 0x40).empty());
}

// ── Длина C++ таблицы ────────────────────────────────────────────────────────
namespace vt_probe {
int one() { return 1; }
int two() { return 2; }
int data_slot = 0;
}  // namespace vt_probe

TEST(Scan, VtableSlotsCountsCodeUntilFirstNonCode) {
    void* table[] = {reinterpret_cast<void*>(&vt_probe::one),
                     reinterpret_cast<void*>(&vt_probe::two), &vt_probe::data_slot,
                     reinterpret_cast<void*>(&vt_probe::one)};
    EXPECT_EQ(graft::scan::vtable_slots(table), 2u);

    void* empty[] = {&vt_probe::data_slot};
    EXPECT_EQ(graft::scan::vtable_slots(empty), 0u);
    EXPECT_EQ(graft::scan::vtable_slots(nullptr), 0u);
}

TEST(Scan, IsCodeAsksThePageNotTheSection) {
    EXPECT_TRUE(graft::scan::is_code(reinterpret_cast<void*>(&vt_probe::one)));
    EXPECT_FALSE(graft::scan::is_code(&vt_probe::data_slot));
    EXPECT_FALSE(graft::scan::is_code(nullptr));
}

// ── RTTI: имя класса -> таблица ──────────────────────────────────────────────
// Раскладка MSVC: дескриптор типа {vfptr, spare, имя} — имя лежит на +16; объектный
// локатор носит RVA дескриптора на +12 и СВОЙ СОБСТВЕННЫЙ RVA на +20; а перед самой
// таблицей лежит указатель на локатор. Собираем это в секции данных и идём тем же путём,
// что и в живом образе.
struct rtti_image {
    fake_image img;

    static constexpr std::size_t kTypeDesc = 0x40;
    static constexpr std::size_t kName     = kTypeDesc + 16;
    static constexpr std::size_t kLocator  = 0x80;
    static constexpr std::size_t kColPtr   = 0xC0;

    explicit rtti_image(const char* mangled) {
        img.put_str(kName, mangled);
        img.put_u32(kLocator + 0, 1);                                       // сигнатура x64
        img.put_u32(kLocator + 12, static_cast<std::uint32_t>(kTypeDesc));  // RVA дескриптора
        img.put_u32(kLocator + 20, static_cast<std::uint32_t>(kLocator));   // pSelf
        img.put_ptr(kColPtr, kStrBase + kLocator);
    }
    std::uintptr_t vtable() const { return kStrBase + kColPtr + sizeof(void*); }
};

TEST(Scan, RttiVtableFindsClassByMangledName) {
    rtti_image r{".?AVFoo@@"};
    EXPECT_EQ(graft::scan::rtti_vtable(r.img.sections(), kStrBase, ".?AVFoo@@"), r.vtable());
}

TEST(Scan, RttiVtableIsZeroForUnknownClass) {
    rtti_image r{".?AVFoo@@"};
    EXPECT_EQ(graft::scan::rtti_vtable(r.img.sections(), kStrBase, ".?AVBar@@"), 0u);
}

// Имя есть, а локатора на него нет — ответ «не нашли», а не первая попавшаяся таблица.
TEST(Scan, RttiVtableIsZeroWithoutLocator) {
    fake_image img;
    img.put_str(rtti_image::kName, ".?AVFoo@@");
    EXPECT_EQ(graft::scan::rtti_vtable(img.sections(), kStrBase, ".?AVFoo@@"), 0u);
}

// Синтетический образ проверяет разбор, но не то, ТАК ЛИ его раскладывает компилятор.
// Поэтому тот же поиск гоняется по НАСТОЯЩЕМУ RTTI — этого самого бинаря: ответ обязан
// совпасть с указателем на таблицу у живого объекта.
}  // namespace

// class, а не struct: буква после `.?A` — это вид типа, `V` у class и `U` у struct.
// Оба вида здесь и стоят, чтобы разница была видна, а не всплыла в чужом моде.
namespace graft_rtti_probe {
class base {
public:
    virtual ~base() = default;
    virtual int id() const { return 1; }
};
class derived : public base {
public:
    int id() const override { return 2; }
};
struct plain_struct {
    virtual ~plain_struct() = default;
};
}  // namespace graft_rtti_probe

namespace {

std::uintptr_t table_of(const void* object) {
    std::uintptr_t vt = 0;
    std::memcpy(&vt, object, sizeof vt);
    return vt;
}

TEST(Scan, RttiVtableMatchesTheRealBinary) {
    void* self = GetModuleHandleW(nullptr);
    const graft_rtti_probe::base         b;
    const graft_rtti_probe::derived      d;
    const graft_rtti_probe::plain_struct s;

    EXPECT_EQ(graft::scan::rtti_vtable(self, ".?AVbase@graft_rtti_probe@@"), table_of(&b));
    EXPECT_EQ(graft::scan::rtti_vtable(self, ".?AVderived@graft_rtti_probe@@"), table_of(&d));
    EXPECT_EQ(graft::scan::rtti_vtable(self, ".?AUplain_struct@graft_rtti_probe@@"),
              table_of(&s));
    EXPECT_EQ(graft::scan::rtti_vtable(self, ".?AVnope@graft_rtti_probe@@"), 0u);
    // `U` у struct, `V` у class: спутать вид типа — самый частый способ ничего не найти.
    EXPECT_EQ(graft::scan::rtti_vtable(self, ".?AUderived@graft_rtti_probe@@"), 0u);
}

// Цена. Поиск линейный по секциям данных, и от их размера зависит, сколько он стоит на
// настоящем образе игры (у DayZDiag .rdata ~3.4 МБ, .data ~57 МБ). Замер печатает
// пропускную способность — по ней видно, во что обойдётся тот образ.
TEST(Scan, RttiVtableCostIsMeasured) {
    void* self = GetModuleHandleW(nullptr);
    std::size_t scanned = 0;
    for (const graft::scan::view& v : graft::scan::sections_of(self)) {
        if (!v.exec) {
            scanned += v.bytes.size();
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    const std::uintptr_t found =
        graft::scan::rtti_vtable(self, ".?AVderived@graft_rtti_probe@@");
    const double ms =
        std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - t0}.count();

    EXPECT_NE(found, 0u);
    const double mb = static_cast<double>(scanned) / (1024.0 * 1024.0);
    // У DayZDiag_x64 1.29 неисполняемых секций ~61 МБ (.rdata 3.4 + .data 57): по этому
    // числу и считается, во что обойдётся один поиск на настоящем образе.
    std::cout << "  [замер] rtti_vtable: " << mb << " МБ данных за " << ms << " мс ("
              << (mb / (ms / 1000.0)) << " МБ/с); образ игры (~61 МБ) -> ~"
              << (61.0 * ms / mb) << " мс на поиск" << std::endl;
}

// ── Вызов настоящего метода C++ движка ───────────────────────────────────────
// Тот самый разъезд, который стоил падения: у функции-ЧЛЕНА MSVC кладёт скрытый буфер
// под возврат больше 8 байт ВТОРЫМ, после this, а у свободной — первым. Тест держит это
// правило: сломается ABI компилятора — покраснеет здесь, а не в игре.
struct fake_entity {
    float x = 0, y = 0, z = 0;
    int   id = 0;

    __declspec(noinline) graft::vector origin() const { return {x, y, z}; }
    __declspec(noinline) int           ident() const { return id; }
    __declspec(noinline) graft::vector shifted(float by) const { return {x + by, y, z}; }
};

void* address_of(graft::vector (fake_entity::*mp)() const) {
    static_assert(sizeof mp == sizeof(void*), "указатель на метод обязан быть одним адресом");
    void* raw = nullptr;
    std::memcpy(&raw, &mp, sizeof raw);
    return raw;
}

TEST(MemberCall, TwelveByteReturnRidesInTheSecondArgument) {
    fake_entity e{1.0f, 2.0f, 3.0f, 7};
    const graft::vector got =
        graft::scan::member_call<graft::vector>(address_of(&fake_entity::origin), &e);
    EXPECT_EQ(got, (graft::vector{1.0f, 2.0f, 3.0f}));
    EXPECT_EQ(e.id, 7);  // объект не тронут: буфер уехал не поверх него
}

TEST(MemberCall, SmallReturnStaysInTheRegister) {
    fake_entity e{1.0f, 2.0f, 3.0f, 42};
    int (fake_entity::*mp)() const = &fake_entity::ident;
    void* raw = nullptr;
    std::memcpy(&raw, &mp, sizeof raw);
    EXPECT_EQ(graft::scan::member_call<int>(raw, &e), 42);
}

TEST(MemberCall, ArgumentsFollowTheHiddenBuffer) {
    fake_entity e{1.0f, 2.0f, 3.0f, 0};
    graft::vector (fake_entity::*mp)(float) const = &fake_entity::shifted;
    void* raw = nullptr;
    std::memcpy(&raw, &mp, sizeof raw);
    EXPECT_EQ(graft::scan::member_call<graft::vector>(raw, &e, 10.0f),
              (graft::vector{11.0f, 2.0f, 3.0f}));
}

} // namespace
