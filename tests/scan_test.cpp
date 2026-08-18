// Тесты сканера точек регистрации: собираем синтетический "образ" из двух секций
// (строки + код) и проверяем, что discover() находит те же три функции, что руками
// найдены в exe. На настоящих бинарях тот же алгоритм сверяется через
// re/scripts/discover.py (сухой прогон в IDA).
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "graft/scan.hpp"

namespace {

constexpr std::uintptr_t kStrBase = 0x10000000;
constexpr std::uintptr_t kCodeBase = 0x20000000;
constexpr std::uintptr_t kRegGlobal = kCodeBase + 0x1000;
constexpr std::uintptr_t kRegMethod = kCodeBase + 0x2000;
constexpr std::uintptr_t kFindClass = kCodeBase + 0x3000;

struct fake_image {
    std::vector<std::uint8_t> strings = std::vector<std::uint8_t>(0x100, 0);
    std::vector<std::uint8_t> code = std::vector<std::uint8_t>(0x4000, 0);

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
    void put_call(std::size_t off, std::uintptr_t target) {
        code[off] = 0xE8;
        const std::int32_t disp = static_cast<std::int32_t>(target - (kCodeBase + off + 5));
        std::memcpy(code.data() + off + 1, &disp, 4);
    }
    std::vector<graft::scan::view> sections() {
        return {{strings.data(), strings.size(), kStrBase, false},
                {code.data(), code.size(), kCodeBase, true}};
    }
};

// Раскладка как в движке: FindClass(ctx,"Math"); RegisterMethod(ctx,cls,"GetNumberOfSetBits",..);
// ... RegisterGlobal(ctx,"MemoryValidation",..)
fake_image make_image() {
    fake_image img;
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
    const graft::scan::view v{img.strings.data(), img.strings.size(), kStrBase, false};

    EXPECT_EQ(v.find_cstr("MemoryValidation"), kStrBase + 0x10);
    EXPECT_EQ(v.find_cstr("Validation"), 0u);  // хвост чужой строки — не совпадение
    EXPECT_EQ(v.find_cstr("Nope"), 0u);
}

TEST(Scan, DiscoversAllThreeEntryPoints) {
    fake_image img = make_image();
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
    img.put_call(0x18, 0x99999999);   // цель вне всех секций — мусор
    img.put_call(0x1D, kRegGlobal);   // настоящий вызов дальше по коду

    const graft::scan::api api = graft::scan::discover(img.sections());
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(api.register_global), kRegGlobal);
}

TEST(Scan, EmptyImageIsHarmless) {
    const std::vector<graft::scan::view> none;
    EXPECT_FALSE(static_cast<bool>(graft::scan::discover(none)));
}

}  // namespace
