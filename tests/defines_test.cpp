// Дефайны загруженных плагинов: поиск обеих движковых точек по форме тела и формат
// строки, в котором движок хранит имя дефайна. Образ синтетический — ровно та же
// раскладка, что найдена в exe (SRC/graft/defines.cpp), но без игры.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "graft/defines.hpp"

namespace {

constexpr std::uintptr_t kStrBase   = 0x10000000;
constexpr std::uintptr_t kCodeBase  = 0x20000000;
constexpr std::uintptr_t kAddPath   = kCodeBase + 0x1000;
constexpr std::uintptr_t kAddDefine = kCodeBase + 0x2000;
constexpr std::uintptr_t kParser    = kCodeBase + 0x3000; // разбор CfgMods: тут якорь

constexpr const char* kAnchor =
    "[CfgMods::Defines] :: [Warning] :: Skipping malformed define entry at index %i.";

struct fake_image {
    std::vector<std::uint8_t> strings = std::vector<std::uint8_t>(0x200, 0);
    std::vector<std::uint8_t> code    = std::vector<std::uint8_t>(0x4000, 0);

    std::uintptr_t put_str(std::size_t off, const char* text) {
        std::memcpy(strings.data() + off, text, std::strlen(text) + 1);
        return kStrBase + off;
    }

    void put(std::size_t off, std::initializer_list<std::uint8_t> bytes) {
        std::size_t i = 0;
        for (std::uint8_t b : bytes) {
            code[off + i++] = b;
        }
    }

    // lea rcx,[rip+d] -> target
    void put_lea_rcx(std::size_t off, std::uintptr_t target) {
        put(off, {0x48, 0x8D, 0x0D});
        const std::int32_t disp = static_cast<std::int32_t>(target - (kCodeBase + off + 7));
        std::memcpy(code.data() + off + 3, &disp, 4);
    }

    void put_call(std::size_t off, std::uintptr_t target) {
        code[off]               = 0xE8;
        const std::int32_t disp = static_cast<std::int32_t>(target - (kCodeBase + off + 5));
        std::memcpy(code.data() + off + 1, &disp, 4);
    }

    // Тело «дописать в массив у rcx»: счётчик, сам массив и (для дефайнов) шаг в 16 байт.
    void put_appender(std::size_t off, std::uint8_t array_off, bool wide) {
        put(off, {0x8B, 0x41, static_cast<std::uint8_t>(array_off + 12)}); // mov eax,[rcx+n]
        put(off + 3, {0x48, 0x8D, 0x79, array_off});                       // lea rdi,[rcx+n]
        if (wide) {
            put(off + 7, {0x48, 0xC1, 0xE0, 0x04}); // shl rax,4
        }
    }

    std::vector<graft::scan::view> sections() {
        return {{strings, kStrBase, false}, {code, kCodeBase, true}};
    }
};

// Как в движке: обе точки — методы аддона, обе зовутся из разбора CfgMods, и рядом с
// вызовом дефайнов лежит ссылка на строку предупреждения.
fake_image make_image() {
    fake_image           img;
    const std::uintptr_t anchor = img.put_str(0x10, kAnchor);
    img.put_appender(0x1000, 0x18, false); // CAddon::AddScriptModulePath
    img.put_appender(0x2000, 0x28, true);  // CAddon::AddDefine
    const std::size_t parser = kParser - kCodeBase;
    img.put_call(parser - 0x100, kAddPath); // пути добавляются до разбора defines[]
    img.put_lea_rcx(parser, anchor);
    img.put_call(parser + 0x40, kAddDefine);
    return img;
}

TEST(Defines, FindsBothAddonEntryPoints) {
    fake_image img = make_image();
    const auto api = graft::defines::find(img.sections());

    ASSERT_TRUE(api.has_value());
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(api->add_path), kAddPath);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(api->add_define), kAddDefine);
}

TEST(Defines, NoAnchorNoSearch) {
    fake_image img = make_image();
    img.strings.assign(img.strings.size(), 0); // строки-якоря в образе нет

    const auto api = graft::defines::find(img.sections());
    ASSERT_FALSE(api.has_value());
    EXPECT_EQ(api.error(), graft::miss::not_found);
}

// Массив со счётчиком не на своём месте — не наша функция. Иначе под врезку попало бы
// первое попавшееся обращение к полю объекта.
TEST(Defines, IgnoresForeignArrayShape) {
    fake_image img = make_image();
    img.put(0x2000, {0x8B, 0x41, 0x40}); // счётчик не на array+12

    const auto api = graft::defines::find(img.sections());
    ASSERT_FALSE(api.has_value());
    EXPECT_EQ(api.error(), graft::miss::not_found);
}

// Две разные функции одной формы в окне — отказ целиком, а не выбор наугад. И отказ
// называет именно это: «нашлось две» и «нет вовсе» чинятся по-разному.
TEST(Defines, AmbiguityRefuses) {
    fake_image img = make_image();
    img.put_appender(0x2800, 0x28, true);
    img.put_call(kParser - kCodeBase + 0x60, kCodeBase + 0x2800);

    const auto api = graft::defines::find(img.sections());
    ASSERT_FALSE(api.has_value());
    EXPECT_EQ(api.error(), graft::miss::ambiguous);
}

// Шапка строки движка: [-6] ёмкость, [-4] длина, [-2] счётчик ссылок, дальше символы с
// нулём. Потребители читают длину именно оттуда, а не strlen.
TEST(Defines, EngineStringCarriesHeader) {
    const char* s = graft::defines::engine_string("GRAFTED_MY_MOD");

    std::uint16_t cap = 0;
    std::uint16_t len = 0;
    std::uint16_t ref = 0;
    std::memcpy(&cap, s - 6, sizeof cap);
    std::memcpy(&len, s - 4, sizeof len);
    std::memcpy(&ref, s - 2, sizeof ref);

    EXPECT_STREQ(s, "GRAFTED_MY_MOD");
    EXPECT_EQ(len, 14);
    EXPECT_EQ(cap, 15); // вместе с нулём
    EXPECT_GT(ref, 1);  // до единицы счётчик не дойдёт: память не движковая
}

TEST(Defines, DefineNameKeepsIdentifiersOnly) {
    EXPECT_EQ(graft::defines::define_name("MY_MOD"), "GRAFTED_MY_MOD");
    EXPECT_EQ(graft::defines::define_name("my-plugin 2"), "GRAFTED_my_plugin_2");
}

} // namespace
