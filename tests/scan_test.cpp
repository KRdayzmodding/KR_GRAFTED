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

    // lea reg,[rip+d] -> target
    void put_lea(std::size_t off, const std::uint8_t (&opcode)[3], std::uintptr_t target) {
        std::memcpy(code.data() + off, opcode, 3);
        const std::int32_t disp = static_cast<std::int32_t>(target - (kCodeBase + off + 7));
        std::memcpy(code.data() + off + 3, &disp, 4);
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
    img.put_lea(0x05, graft::scan::lea_r8, s_method);
    img.put_call(0x0C, kRegMethod);
    img.put_lea(0x11, graft::scan::lea_rdx, s_global);
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

    EXPECT_TRUE(graft::scan::matches(ea, {0x48, 0x89, 0x5C, 0x24, -1, 0x57}));
    EXPECT_TRUE(graft::scan::matches(ea, {0x48, 0x89}));
    EXPECT_TRUE(graft::scan::matches(ea, {-1, -1, -1}));
    EXPECT_FALSE(graft::scan::matches(ea, {0x48, 0x89, 0x5C, 0x24, 0x11, 0x57}));
    EXPECT_FALSE(graft::scan::matches(ea, {0x49}));
}

// Кандидат мог приехать из мусорного смещения — сверка обязана ответить «не совпало», а
// не уронить процесс на чтении по нулю.
TEST(Scan, MatchesRefusesUnreadableAddress) {
    EXPECT_FALSE(graft::scan::matches(0, {0x48}));
    EXPECT_FALSE(graft::scan::matches(0xDEADBEEF, {0x48}));
}

// Адрес внутри функции ничем не выровнен: сверка сигнатуры со смещения 1 обязана
// работать так же, как с начала. Это отдельный кейс, потому что «похоже на указатель»
// требует выравнивания по 8, а «страница читается» — нет, и спутать их легко.
TEST(Scan, MatchesWorksAtUnalignedAddress) {
    static const std::uint8_t body[] = {0x00, 0x48, 0x89, 0x5C, 0x24, 0x10};
    const auto ea = reinterpret_cast<std::uintptr_t>(body) + 1;
    EXPECT_TRUE(graft::scan::matches(ea, {0x48, 0x89, 0x5C}));
}

// ── Сигнатура компайл-тайм строкой ───────────────────────────────────────────
// Та же сигнатура, но копируется из отладчика как есть. Длину и вид байтов проверяет
// компилятор, а не отладка на живом сервере.
TEST(Scan, SignatureFromLiteralIsTheSameAsTheByteList) {
    static const std::uint8_t body[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57};
    const auto ea = reinterpret_cast<std::uintptr_t>(body);

    EXPECT_TRUE(graft::scan::matches(ea, graft::scan::sig<"48 89 5C 24 ?? 57">));
    EXPECT_FALSE(graft::scan::matches(ea, graft::scan::sig<"48 89 5C 24 11 57">));
    EXPECT_TRUE(graft::scan::matches(ea, graft::scan::sig<"48 89">));
}

TEST(Scan, SignatureLiteralIsParsedAtCompileTime) {
    static_assert(graft::scan::sig<"48 89 5C">.size() == 3);
    static_assert(graft::scan::sig<"48 ?? 57">[1] < 0, "?? — это любой байт");
    static_assert(graft::scan::sig<"4a">[0] == 0x4A, "регистр цифр значения не имеет");
    // Лишние пробелы — не ошибка: строку копируют из отладчика, а он выравнивает столбцы.
    static_assert(graft::scan::sig<"  48   89  ">.size() == 2);
    static_assert(graft::scan::sig<"48 ? 57">[1] < 0, "одиночный ? — тот же джокер");
    // Мусор в строке — ошибка КОМПИЛЯЦИИ (consteval не может бросить), поэтому кейса на
    // него здесь нет и быть не может: он не собрался бы.
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
