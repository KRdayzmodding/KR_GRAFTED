// Тесты сканера точек регистрации: собираем синтетический "образ" из двух секций
// (строки + код) и проверяем, что discover() находит те же три функции, что руками
// найдены в exe. На настоящих бинарях тот же алгоритм сверяется через
// re/scripts/discover.py (сухой прогон в IDA).
#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <expected>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "graft/scan.hpp"
#include "graft/types.hpp"

namespace {

// Причина отказа — или пусто, если отказа не было. `.error()` у ответа без ошибки читать
// нельзя (UB), а кейс «нашлось, хотя не должно» обязан краснеть, а не падать.
template <class T>
std::optional<graft::miss> why(const std::expected<T, graft::miss>& r) {
    return r ? std::nullopt : std::optional{r.error()};
}

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
    // Хвост чужой строки — не совпадение; и отказ называет причину, а не молчит нулём.
    EXPECT_EQ(why(v.find_cstr("Validation")), graft::miss::not_found);
    EXPECT_EQ(why(v.find_cstr("Nope")), graft::miss::not_found);
}

// Ссылка через rip находится по цели и по форме инструкции; следующую ищут с `from`.
TEST(Scan, ViewFindRipFindsTheReferenceAndSaysWhyNot) {
    fake_image           img;
    const std::uintptr_t text = img.put_str(0x00, "anchor");
    img.put_insn(0x10, graft::scan::lea_rdx, text);
    const graft::scan::view code{img.code, kCodeBase, true};

    EXPECT_EQ(code.find_rip(graft::scan::lea_rdx, text), kCodeBase + 0x10);
    EXPECT_EQ(why(code.find_rip(graft::scan::lea_rdx, text, kCodeBase + 0x11)),
              graft::miss::not_found); // дальше ссылок нет
    EXPECT_EQ(why(code.find_rip(graft::scan::lea_r8, text)), graft::miss::not_found);
}

TEST(Scan, DiscoversAllThreeEntryPoints) {
    fake_image img = make_image();
    const auto api = graft::scan::discover(img.sections());

    ASSERT_TRUE(api.has_value());
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(api->register_global), kRegGlobal);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(api->register_method), kRegMethod);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(api->find_class), kFindClass);
}

// Байт 0xE8 может оказаться и внутри чужого смещения: такой "вызов" ведёт мимо кода
// и не должен уводить поиск с настоящего call'а.
TEST(Scan, IgnoresFalseCallBytes) {
    fake_image img = make_image();
    img.put_call(0x18, 0x99999999); // цель вне всех секций — мусор
    img.put_call(0x1D, kRegGlobal); // настоящий вызов дальше по коду

    const auto api = graft::scan::discover(img.sections());
    ASSERT_TRUE(api.has_value());
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(api->register_global), kRegGlobal);
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
    fake_image            img;
    const std::uintptr_t  name = img.put_str(0x00, "OnUpdate");
    constexpr std::size_t site = 0x100;
    img.put_insn(site, graft::scan::lea_rdx, name); // lea rdx,"OnUpdate"
    img.put_call(site + 7, kFindIndex);             // найти индекс по имени
    if (with_float) {
        img.put_bytes(site + 12, {0xF3, 0x0F, 0x5A, 0xC6}); // cvtss2sd xmm0,xmm6
    }
    img.put_insn(site + 16, graft::scan::sig<"89 05 [disp32]">, // mov [rip+d],eax
                 kIndexVar);
    img.put_call(site + 22, kPrepare); // собрать вызов <- цель
    return img;
}

TEST(Scan, FindsFrameEntryByItsFourMarks) {
    fake_image img = make_frame_image(true);

    const auto e = graft::scan::find_frame_entry(img.sections());
    ASSERT_TRUE(e.has_value());
    // Перехватывается ВТОРОЙ вызов — сборка кадра, а не поиск индекса.
    EXPECT_EQ(e->site, kCodeBase + 0x100 + 22);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(e->prepare), kPrepare);
    // Кэш индекса берётся из дырки: адрес считается от КОНЦА инструкции.
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(e->index), kIndexVar);
}

// Строка "OnUpdate" в образе одна на всех, а ссылок на неё несколько: виджеты, техника,
// игра. Нужную отличает ровно float-аргумент — без него это не кадр, и ответ обязан быть
// отказом, а не ближайшим похожим местом.
TEST(Scan, FrameEntryNeedsTheFloatMark) {
    fake_image img = make_frame_image(false);
    EXPECT_EQ(why(graft::scan::find_frame_entry(img.sections())), graft::miss::not_found);
}

// Цель `call` обязана лежать в исполняемой секции: байт 0xE8 встречается и внутри чужого
// смещения, и такой «вызов» ведёт куда попало.
TEST(Scan, FrameEntryRefusesCallOutsideCode) {
    fake_image img = make_frame_image(true);
    img.put_call(0x100 + 22, 0x99999999);
    EXPECT_EQ(why(graft::scan::find_frame_entry(img.sections())), graft::miss::not_found);
}

TEST(Scan, EmptyImageIsHarmless) {
    const std::vector<graft::scan::view> none;
    EXPECT_EQ(why(graft::scan::discover(none)), graft::miss::not_found);
}

// Причина идёт в журнал словами, и у каждой они свои: иначе «маяк пропал» и «маяк раздвоился»,
// которые чинятся по-разному, в журнале не отличить.
TEST(Scan, EveryMissReasonHasItsOwnWords) {
    using graft::miss;
    std::set<std::string> seen;
    for (const miss m : {miss::null_object, miss::not_found, miss::not_native, miss::wrong_arity, miss::too_many_args, miss::unsafe_arg, miss::wrong_type, miss::no_template, miss::ambiguous, miss::unreadable}) {
        const std::string words = to_string(m);
        EXPECT_NE(words, "?");
        EXPECT_TRUE(seen.insert(words).second) << "повтор: " << words;
    }
}

// ── Первый вызов в функции ───────────────────────────────────────────────────
// Окно задаёт вызывающий, и оно — часть ответа: вызов за его краем не считается, иначе
// «первый вызов функции» превращался бы в «какой-нибудь вызов подальше».
TEST(Scan, FirstCallIsTheFirstRealCallInsideTheWindow) {
    fake_image img;
    img.put_call(0x100 + 0x08, 0x99999999);        // цель вне кода — мусор, отсеивается
    img.put_call(0x100 + 0x10, kCodeBase + 0x300); // первый настоящий
    img.put_call(0x100 + 0x20, kCodeBase + 0x400);
    const std::vector<graft::scan::view> sections = img.sections();

    EXPECT_EQ(graft::scan::first_call(sections, kCodeBase + 0x100, 0x40), kCodeBase + 0x300);
    // Окно кончается до настоящего вызова — отказ, а не ближайший подходящий.
    EXPECT_EQ(why(graft::scan::first_call(sections, kCodeBase + 0x100, 0x10)),
              graft::miss::not_found);
    EXPECT_EQ(why(graft::scan::first_call(sections, 0x12345678, 0x40)), graft::miss::not_found);
}

// ── Сигнатура с джокерами ────────────────────────────────────────────────────
// В прологе движковой функции сверять можно не всё: там, где запечено смещение поля или
// выбранный компилятором игры регистр, байт обязан считаться любым.
TEST(Scan, MatchesSignatureWithWildcards) {
    static const std::uint8_t body[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83};
    const auto                ea     = reinterpret_cast<std::uintptr_t>(body);

    EXPECT_TRUE(graft::scan::matches(ea, graft::scan::sig<"48 89 5C 24 ?? 57">));
    EXPECT_TRUE(graft::scan::matches(ea, graft::scan::sig<"48 89">));
    EXPECT_TRUE(graft::scan::matches(ea, graft::scan::sig<"?? ?? ??">));
    EXPECT_FALSE(graft::scan::matches(ea, graft::scan::sig<"48 89 5C 24 11 57">));
    EXPECT_FALSE(graft::scan::matches(ea, graft::scan::sig<"49">));
}

// Маскированный байт: совпасть обязаны только биты маски.
TEST(Scan, MasksCompareOnlyTheBitsThatMatter) {
    static const std::uint8_t rax[]   = {0x48, 0x8D, 0x05, 0x11, 0x22, 0x33, 0x44};
    static const std::uint8_t rdx[]   = {0x48, 0x8D, 0x15, 0x11, 0x22, 0x33, 0x44};
    static const std::uint8_t other[] = {0x48, 0x8D, 0x41, 0x08, 0x00, 0x00, 0x00};
    constexpr auto            any_reg = graft::scan::sig<"48 8D 05&C7">;

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
    EXPECT_EQ(why(v.find(kCodeBase, 0x20, graft::scan::sig<"8B 41 [disp8]">)),
              graft::miss::not_found);
    // Адрес не из этой секции — не «нет вхождения», а «читать здесь нечего»: причины разные.
    EXPECT_EQ(why(v.find(kCodeBase - 1, 0x10, graft::scan::sig<"8B 41 [disp8]">)),
              graft::miss::unreadable);
}

// Кандидат мог приехать из мусорного смещения — сверка обязана ответить «не совпало», а
// не уронить процесс на чтении по нулю. Причина при этом своя: память не отображена — это
// не то же самое, что сигнатуры в ней нет.
TEST(Scan, MatchesRefusesUnreadableAddress) {
    EXPECT_FALSE(graft::scan::matches(0, graft::scan::sig<"48">));
    EXPECT_FALSE(graft::scan::matches(0xDEADBEEF, graft::scan::sig<"48">));
    EXPECT_EQ(why(graft::scan::find(0, 0x20, graft::scan::sig<"48">)), graft::miss::unreadable);
    EXPECT_EQ(why(graft::scan::find(0xDEADBEEF, 0x20, graft::scan::sig<"48">)),
              graft::miss::unreadable);
}

// Адрес внутри функции ничем не выровнен: сверка сигнатуры со смещения 1 обязана
// работать так же, как с начала. Это отдельный кейс, потому что «похоже на указатель»
// требует выравнивания по 8, а «страница читается» — нет, и спутать их легко.
TEST(Scan, MatchesWorksAtUnalignedAddress) {
    static const std::uint8_t body[] = {0x00, 0x48, 0x89, 0x5C, 0x24, 0x10};
    const auto                ea     = reinterpret_cast<std::uintptr_t>(body) + 1;
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
    // clang-format off
    static const std::uint8_t body[] = {0x8B, 0x41, 0x38,
                                        0x48, 0x8B, 0x81, 0xC8, 0x02, 0x00, 0x00,
                                        0x8B, 0x41, 0x40};
    // clang-format on
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
    const auto                ea     = reinterpret_cast<std::uintptr_t>(body);
    const auto                at     = graft::scan::find(ea, sizeof body, graft::scan::sig<"8B 41 [disp8]">);
    ASSERT_TRUE(at.has_value());
    EXPECT_EQ(at->value, -128);
}

// Окно кончается там, где сказано: инструкция, начинающаяся за его краем, не считается,
// иначе разбор одной функции цепляет соседнюю.
TEST(Scan, FindStaysInsideTheWindow) {
    static const std::uint8_t body[] = {0x90, 0x90, 0x90, 0x8B, 0x41, 0x38};
    const auto                ea     = reinterpret_cast<std::uintptr_t>(body);
    EXPECT_TRUE(graft::scan::find(ea, 6, graft::scan::sig<"8B 41 [disp8]">).has_value());
    EXPECT_EQ(why(graft::scan::find(ea, 5, graft::scan::sig<"8B 41 [disp8]">)),
              graft::miss::not_found);
}

// Сигнатура без дырки ищется так же, просто отвечать нечем, кроме адреса.
TEST(Scan, FindWithoutAHoleReturnsOnlyTheSite) {
    static const std::uint8_t body[] = {0x90, 0x48, 0x8B, 0xC4};
    const auto                ea     = reinterpret_cast<std::uintptr_t>(body);
    const auto                at     = graft::scan::find(ea, sizeof body, graft::scan::sig<"48 8B C4">);
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
} // namespace vt_probe

TEST(Scan, VtableSlotsCountsCodeUntilFirstNonCode) {
    void* table[] = {reinterpret_cast<void*>(&vt_probe::one),
                     reinterpret_cast<void*>(&vt_probe::two),
                     &vt_probe::data_slot,
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
        img.put_u32(kLocator + 0, 1);                                      // сигнатура x64
        img.put_u32(kLocator + 12, static_cast<std::uint32_t>(kTypeDesc)); // RVA дескриптора
        img.put_u32(kLocator + 20, static_cast<std::uint32_t>(kLocator));  // pSelf
        img.put_ptr(kColPtr, kStrBase + kLocator);
    }

    std::uintptr_t vtable() const { return kStrBase + kColPtr + sizeof(void*); }
};

TEST(Scan, RttiVtableFindsClassByMangledName) {
    rtti_image r{".?AVFoo@@"};
    EXPECT_EQ(graft::scan::rtti_vtable(r.img.sections(), kStrBase, ".?AVFoo@@"), r.vtable());
}

TEST(Scan, RttiVtableIsNotFoundForUnknownClass) {
    rtti_image r{".?AVFoo@@"};
    EXPECT_EQ(why(graft::scan::rtti_vtable(r.img.sections(), kStrBase, ".?AVBar@@")),
              graft::miss::not_found);
}

// Имя есть, а локатора на него нет — ответ «не нашли», а не первая попавшаяся таблица.
TEST(Scan, RttiVtableIsNotFoundWithoutLocator) {
    fake_image img;
    img.put_str(rtti_image::kName, ".?AVFoo@@");
    EXPECT_EQ(why(graft::scan::rtti_vtable(img.sections(), kStrBase, ".?AVFoo@@")),
              graft::miss::not_found);
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
} // namespace graft_rtti_probe

namespace {

std::uintptr_t table_of(const void* object) {
    std::uintptr_t vt = 0;
    std::memcpy(&vt, object, sizeof vt);
    return vt;
}

TEST(Scan, RttiVtableMatchesTheRealBinary) {
    void*                                self = GetModuleHandleW(nullptr);
    const graft_rtti_probe::base         b;
    const graft_rtti_probe::derived      d;
    const graft_rtti_probe::plain_struct s;

    EXPECT_EQ(graft::scan::rtti_vtable(self, ".?AVbase@graft_rtti_probe@@"), table_of(&b));
    EXPECT_EQ(graft::scan::rtti_vtable(self, ".?AVderived@graft_rtti_probe@@"), table_of(&d));
    EXPECT_EQ(graft::scan::rtti_vtable(self, ".?AUplain_struct@graft_rtti_probe@@"),
              table_of(&s));
    EXPECT_EQ(why(graft::scan::rtti_vtable(self, ".?AVnope@graft_rtti_probe@@")),
              graft::miss::not_found);
    // `U` у struct, `V` у class: спутать вид типа — самый частый способ ничего не найти.
    EXPECT_EQ(why(graft::scan::rtti_vtable(self, ".?AUderived@graft_rtti_probe@@")),
              graft::miss::not_found);
}

// Модуль плагину брать не нужно: образ игры — главный модуль, и ответ тот же.
TEST(Scan, RttiVtableWithoutModuleLooksInTheGameImage) {
    const graft_rtti_probe::derived d;

    EXPECT_EQ(graft::scan::rtti_vtable(".?AVderived@graft_rtti_probe@@"), table_of(&d));
    EXPECT_EQ(why(graft::scan::rtti_vtable(".?AVnope@graft_rtti_probe@@")),
              graft::miss::not_found);
}

// ── От строки-маяка к функции ───────────────────────────────────────────────
// У движковых функций без натива (проверка линковки модуля) другой зацепки нет: строка
// есть, имени нет. Проверяется на образе самого теста — там настоящие .pdata.
//
// Фикстура собрана так, чтобы компилятор не выбросил то, что ищем:
//   - строка уходит через volatile-указатель — иначе её свернут в константу даже сквозь
//     noinline, и `lea [rip+...]` в коде не останется;
//   - вызов не хвостовой — лист без кадра (`lea; jmp rax`) записи раскрутки не имеет.
std::size_t graft_anchor_sink(const char* text, std::size_t salt) {
    return std::strlen(text) + salt;
}

std::size_t (*volatile g_anchor_sink)(const char*, std::size_t) = &graft_anchor_sink;

__declspec(noinline) std::size_t graft_anchor_probe(std::size_t salt) {
    const std::size_t n = g_anchor_sink("graft-scan-probe: function referencing this line", salt);
    return n * 2 + g_anchor_sink("", salt);
}

// Двойники: одна и та же строка в двух РАЗНЫХ функциях. Множители разные не случайно — иначе
// линкер склеил бы тела в одно (`/OPT:ICF`), и функция осталась бы одна.
__declspec(noinline) std::size_t graft_twin_left(std::size_t salt) {
    return g_anchor_sink("graft-scan-probe: two functions say this line", salt) * 3;
}

__declspec(noinline) std::size_t graft_twin_right(std::size_t salt) {
    return g_anchor_sink("graft-scan-probe: two functions say this line", salt) * 5;
}

// Искомое собирается в рантайме: литерал целиком в теле кейса сам стал бы «функцией,
// которая ссылается на строку».
std::string probe_text(const char* tail) {
    return std::string{"graft-scan-probe:"} + tail;
}

TEST(Scan, FunctionStartFromAnyAddressInside) {
    ASSERT_GT(graft_anchor_probe(1), 0u);
    const auto start = reinterpret_cast<std::uintptr_t>(&graft_anchor_probe);
    EXPECT_EQ(graft::scan::function_start(start), start);
    EXPECT_EQ(graft::scan::function_start(start + 4), start);
    EXPECT_EQ(why(graft::scan::function_start(0)), graft::miss::not_found); // не код — не функция
}

TEST(Scan, FunctionReferencingFindsItsStart) {
    ASSERT_GT(graft_anchor_probe(1), 0u);
    const auto sections = graft::scan::sections_of(GetModuleHandleW(nullptr));
    EXPECT_EQ(graft::scan::function_referencing(
                  sections, probe_text(" function referencing this line").c_str()),
              reinterpret_cast<std::uintptr_t>(&graft_anchor_probe));
    EXPECT_EQ(why(graft::scan::function_referencing(sections,
                                                    probe_text(" nobody says this").c_str())),
              graft::miss::not_found);
}

// Строка, на которую ссылаются две разные функции, — не маяк: выбирать из двух нельзя. И
// отказ называет именно это, а не «нет такой»: когда после патча игры маяк раздвоился,
// искать его надо иначе, чем когда он пропал.
TEST(Scan, FunctionReferencingRefusesWhenTwoFunctionsShareTheString) {
    ASSERT_GT(graft_twin_left(1) + graft_twin_right(1), 0u);
    EXPECT_NE(reinterpret_cast<std::uintptr_t>(&graft_twin_left),
              reinterpret_cast<std::uintptr_t>(&graft_twin_right));
    EXPECT_EQ(why(graft::scan::function_referencing(
                  probe_text(" two functions say this line").c_str())),
              graft::miss::ambiguous);
}

// ── Образ игры без Windows API ───────────────────────────────────────────────
// Плагину, чтобы искать в движке, не нужен ни модуль от системы, ни кэш секций в своей
// статике: образ игры — главный модуль процесса, и отдаёт его сама библиотека. Здесь это
// образ самого теста — тот же путь, что и в игре.
TEST(Scan, ImageIsTheSectionsOfTheMainModule) {
    const auto  own = graft::scan::sections_of(GetModuleHandleW(nullptr));
    const auto& img = graft::scan::image();

    ASSERT_FALSE(img.empty());
    ASSERT_EQ(img.size(), own.size());
    for (std::size_t i = 0; i < img.size(); ++i) {
        EXPECT_EQ(img[i].base, own[i].base);
        EXPECT_EQ(img[i].bytes.size(), own[i].bytes.size());
        EXPECT_EQ(img[i].exec, own[i].exec);
    }
    // Одна на процесс: считается при первом обращении, а ссылку можно держать сколько угодно.
    EXPECT_EQ(&graft::scan::image(), &img);

    const auto code = reinterpret_cast<std::uintptr_t>(&graft_anchor_probe);
    EXPECT_TRUE(std::ranges::any_of(
        img, [&](const graft::scan::view& v) { return v.exec && v.contains(code); }));
}

TEST(Scan, FunctionReferencingWithoutSectionsLooksInTheGameImage) {
    ASSERT_GT(graft_anchor_probe(1), 0u);
    EXPECT_EQ(graft::scan::function_referencing(
                  probe_text(" function referencing this line").c_str()),
              reinterpret_cast<std::uintptr_t>(&graft_anchor_probe));
    EXPECT_EQ(why(graft::scan::function_referencing(probe_text(" nobody says this").c_str())),
              graft::miss::not_found);
}

// ── Тело функции: конец считает таблица раскрутки, а не число в вызове ───────────
// Искать листинг «в первых N байтах» значит гадать N: мало — не дотянешься, много — окно
// перелезет в соседнюю функцию и подхватит чужое. Конец функции известен точно, он лежит в
// .pdata, поэтому окно берётся оттуда и в вызове числа нет.
//
// Функции собираются прямо в памяти, а их записи регистрируются руками
// (RtlAddFunctionTable): так граница между соседями известна до байта, и видно, что поиск
// её не переходит.
//
//   +0x00 ┌ A ┐ +0x40   `8B 41 11` на +0x08; `8B 41 12` на +0x3D — последнее место, где
//                        сигнатура ещё влезает в функцию
//   +0x40 └ B ┘ +0x80   `8B 41 22` с самого начала: сразу за концом A
//   +0x80 ┌ C ┐ +0xC0   холодный хвост A, вынесенный компилятором: своя запись, сцепленная с
//                        основной; `8B 41 33` в начале, `8B 41 44` — уже за его концом
class jit_functions {
public:
    jit_functions() {
        base_ = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!base_) {
            return;
        }
        std::memset(base_, 0x90, 0x200); // nop: случайных сигнатур в теле нет
        put(0x08, {0x8B, 0x41, 0x11});
        put(0x3D, {0x8B, 0x41, 0x12});
        put(0x40, {0x8B, 0x41, 0x22});
        put(0x80, {0x8B, 0x41, 0x33});
        put(0xC0, {0x8B, 0x41, 0x44});

        // UNWIND_INFO без кодов: версия 1 — этого достаточно, чтобы запись признали записью.
        put(0x400, {0x01, 0x00, 0x00, 0x00});
        // Сцепленная запись: флаг CHAININFO, а за заголовком — запись основной функции целиком.
        put(0x410, {static_cast<std::uint8_t>(1 | (UNW_FLAG_CHAININFO << 3)), 0x00, 0x00, 0x00});
        const RUNTIME_FUNCTION main_fn{0x00, 0x40, 0x400};
        std::memcpy(base_ + 0x414, &main_fn, sizeof main_fn);

        table_[0]   = {0x00, 0x40, 0x400}; // A
        table_[1]   = {0x40, 0x80, 0x400}; // B
        table_[2]   = {0x80, 0xC0, 0x410}; // C: сцеплена с A
        registered_ = RtlAddFunctionTable(table_, 3, reinterpret_cast<DWORD64>(base_)) != FALSE;
    }

    ~jit_functions() {
        if (registered_) {
            RtlDeleteFunctionTable(table_);
        }
        if (base_) {
            VirtualFree(base_, 0, MEM_RELEASE);
        }
    }

    jit_functions(const jit_functions&)            = delete;
    jit_functions& operator=(const jit_functions&) = delete;

    bool ok() const { return base_ != nullptr && registered_; }

    std::uintptr_t at(std::size_t off) const { return reinterpret_cast<std::uintptr_t>(base_) + off; }

    std::uintptr_t a() const { return at(0x00); }

    std::uintptr_t b() const { return at(0x40); }

    std::uintptr_t cold() const { return at(0x80); }

private:
    void put(std::size_t off, std::initializer_list<std::uint8_t> bytes) {
        std::copy(bytes.begin(), bytes.end(), base_ + off);
    }

    std::uint8_t*    base_ = nullptr;
    RUNTIME_FUNCTION table_[3]{};
    bool             registered_ = false;
};

TEST(Scan, FunctionStartFollowsAColdPartBackToItsMainFunction) {
    jit_functions fn;
    ASSERT_TRUE(fn.ok());

    EXPECT_EQ(graft::scan::function_start(fn.a() + 4), fn.a());
    EXPECT_EQ(graft::scan::function_start(fn.b()), fn.b());        // соседняя — своя, а не A
    EXPECT_EQ(graft::scan::function_start(fn.cold() + 4), fn.a()); // по цепочке — к основной
    EXPECT_EQ(why(graft::scan::function_start(fn.at(0x200))), graft::miss::not_found);
}

TEST(Scan, FindInFunctionStopsAtTheEndOfTheFunction) {
    jit_functions fn;
    ASSERT_TRUE(fn.ok());
    constexpr auto field = graft::scan::sig<"8B 41 [disp8]">;

    const auto first = graft::scan::find_in_function(fn.a(), field);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->site, fn.a() + 0x08);
    EXPECT_EQ(first->value, 0x11);

    // Последнее место, где сигнатура ещё влезает, — внутри функции, и находится...
    const auto last = graft::scan::find_in_function(fn.a(), field, 2);
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->value, 0x12);

    // ...а за концом начинается соседняя: её вхождение в окно не попадает.
    EXPECT_EQ(why(graft::scan::find_in_function(fn.a(), field, 3)), graft::miss::not_found);

    // Своя функция находит своё и тоже не заходит к соседу за ней.
    const auto own = graft::scan::find_in_function(fn.b(), field);
    ASSERT_TRUE(own.has_value());
    EXPECT_EQ(own->value, 0x22);
    EXPECT_EQ(why(graft::scan::find_in_function(fn.b(), field, 2)), graft::miss::not_found);
}

// Адрес не обязан быть началом: ищется от него и до конца функции, в которой он лежит. Так
// пропускают пролог, не считая его байты.
TEST(Scan, FindInFunctionSearchesFromTheGivenAddressToTheEnd) {
    jit_functions fn;
    ASSERT_TRUE(fn.ok());

    const auto at = graft::scan::find_in_function(fn.a() + 0x10, graft::scan::sig<"8B 41 [disp8]">);
    ASSERT_TRUE(at.has_value());
    EXPECT_EQ(at->value, 0x12); // вхождение на +0x08 осталось позади
}

// Холодный хвост — отдельный кусок со своей записью: окно кончается на его краю, а не на
// краю основной функции и не там, где кончается память.
TEST(Scan, FindInFunctionKeepsToTheColdPartItWasGivenAddressIn) {
    jit_functions fn;
    ASSERT_TRUE(fn.ok());
    constexpr auto field = graft::scan::sig<"8B 41 [disp8]">;

    const auto in_cold = graft::scan::find_in_function(fn.cold(), field);
    ASSERT_TRUE(in_cold.has_value());
    EXPECT_EQ(in_cold->value, 0x33);
    EXPECT_EQ(why(graft::scan::find_in_function(fn.cold(), field, 2)), graft::miss::not_found);
}

// Нет записи раскрутки — нет и конца: «до конца функции» тут не определено, и это отказ, а
// не окно, взятое с потолка.
TEST(Scan, FindInFunctionRefusesWhereNoFunctionIsKnown) {
    jit_functions fn;
    ASSERT_TRUE(fn.ok());
    constexpr auto field = graft::scan::sig<"8B 41 [disp8]">;

    EXPECT_EQ(why(graft::scan::find_in_function(fn.at(0x200), field)), graft::miss::not_found);
    EXPECT_EQ(why(graft::scan::find_in_function(0, field)), graft::miss::not_found);
}

// То же на настоящем .pdata этого бинаря: функцию нашли по строке и ищут в ней целиком —
// без числа в вызове.
TEST(Scan, FindInFunctionWorksOnTheRealBinary) {
    ASSERT_GT(graft_anchor_probe(1), 0u);
    const auto fn = graft::scan::function_referencing(
        probe_text(" function referencing this line").c_str());
    ASSERT_TRUE(fn.has_value());

    // «Любой байт» совпадает с первого же места: сверять нечем, важно, что окно вообще есть.
    const auto at = graft::scan::find_in_function(*fn, graft::scan::sig<"[disp8]">);
    ASSERT_TRUE(at.has_value());
    EXPECT_EQ(at->site, *fn);
}

// Цена. Поиск линейный по секциям данных, и от их размера зависит, сколько он стоит на
// настоящем образе игры (у DayZDiag .rdata ~3.4 МБ, .data ~57 МБ). Замер печатает
// пропускную способность — по ней видно, во что обойдётся тот образ.
TEST(Scan, RttiVtableCostIsMeasured) {
    void*       self    = GetModuleHandleW(nullptr);
    std::size_t scanned = 0;
    for (const graft::scan::view& v : graft::scan::sections_of(self)) {
        if (!v.exec) {
            scanned += v.bytes.size();
        }
    }
    const auto   t0 = std::chrono::steady_clock::now();
    const auto   vt = graft::scan::rtti_vtable(self, ".?AVderived@graft_rtti_probe@@");
    const double ms =
        std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - t0}.count();

    EXPECT_TRUE(vt.has_value());
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

    __declspec(noinline) int ident() const { return id; }

    __declspec(noinline) graft::vector shifted(float by) const { return {x + by, y, z}; }
};

void* address_of(graft::vector (fake_entity::*mp)() const) {
    static_assert(sizeof mp == sizeof(void*), "указатель на метод обязан быть одним адресом");
    void* raw = nullptr;
    std::memcpy(&raw, &mp, sizeof raw);
    return raw;
}

TEST(MemberCall, TwelveByteReturnRidesInTheSecondArgument) {
    fake_entity         e{1.0f, 2.0f, 3.0f, 7};
    const graft::vector got =
        graft::scan::member_call<graft::vector>(address_of(&fake_entity::origin), &e);
    EXPECT_EQ(got, (graft::vector{1.0f, 2.0f, 3.0f}));
    EXPECT_EQ(e.id, 7); // объект не тронут: буфер уехал не поверх него
}

TEST(MemberCall, SmallReturnStaysInTheRegister) {
    fake_entity e{1.0f, 2.0f, 3.0f, 42};
    int (fake_entity::*mp)() const = &fake_entity::ident;
    void* raw                      = nullptr;
    std::memcpy(&raw, &mp, sizeof raw);
    EXPECT_EQ(graft::scan::member_call<int>(raw, &e), 42);
}

TEST(MemberCall, ArgumentsFollowTheHiddenBuffer) {
    fake_entity e{1.0f, 2.0f, 3.0f, 0};
    graft::vector (fake_entity::*mp)(float) const = &fake_entity::shifted;
    void* raw                                     = nullptr;
    std::memcpy(&raw, &mp, sizeof raw);
    EXPECT_EQ(graft::scan::member_call<graft::vector>(raw, &e, 10.0f),
              (graft::vector{11.0f, 2.0f, 3.0f}));
}

} // namespace
