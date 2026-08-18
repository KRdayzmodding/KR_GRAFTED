// Логика загрузчика плагинов, отделённая от WinAPI: разбор командной строки, сверка
// версий и слияние реестров с обнаружением коллизий. Всё это чистые функции — их можно
// прогнать без игры, и именно они решают, заведётся мод у человека или нет.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "graft/plugins.hpp"

namespace {

using graft::plugins::collision;
using graft::plugins::entry;

graft_native_desc make(const char* class_name, const char* name) {
    graft_native_desc d{};
    d.class_name = class_name;
    d.name = name;
    d.ret = "int";
    d.module = "1_Core";
    d.generate = 1;
    return d;
}

graft_plugin_info make_info(std::uint32_t abi, std::uint32_t layout) {
    graft_plugin_info info{};
    info.size = sizeof(graft_plugin_info);
    info.abi = abi;
    info.layout = layout;
    info.name = "test";
    return info;
}

// ── Разбор командной строки ──────────────────────────────────────────────────

TEST(ModDirs, PicksBothModAndServerMod) {
    const auto got = graft::plugins::mod_dirs(
        R"(DayZDiag_x64.exe -server -mod=@MAP;@CLIENT; -serverMod=@SERVER; -port=2402)");
    EXPECT_EQ(got, (std::vector<std::string>{"@MAP", "@CLIENT", "@SERVER"}));
}

TEST(ModDirs, IgnoresCaseOfTheSwitch) {
    const auto got = graft::plugins::mod_dirs("game.exe -MOD=@A; -SERVERMOD=@B;");
    EXPECT_EQ(got, (std::vector<std::string>{"@A", "@B"}));
}

TEST(ModDirs, HandlesQuotedListWithSpaces) {
    const auto got = graft::plugins::mod_dirs(R"(game.exe -mod="@My Mod;@B" -port=1)");
    EXPECT_EQ(got, (std::vector<std::string>{"@My Mod", "@B"}));
}

TEST(ModDirs, SkipsEmptyEntriesAndDuplicates) {
    const auto got = graft::plugins::mod_dirs("game.exe -mod=@A;;@B;@A; -serverMod=@B;");
    EXPECT_EQ(got, (std::vector<std::string>{"@A", "@B"}));
}

TEST(ModDirs, EmptyWhenNoSwitches) {
    EXPECT_TRUE(graft::plugins::mod_dirs("game.exe -server -port=2302").empty());
}

// Подстрока "-mod=" внутри другого ключа не должна считаться ключом.
TEST(ModDirs, RequiresSwitchToStartAtBoundary) {
    const auto got = graft::plugins::mod_dirs("game.exe -profiles=x-mod=@NOPE; -mod=@YES;");
    EXPECT_EQ(got, (std::vector<std::string>{"@YES"}));
}

// ── Сверка версий ────────────────────────────────────────────────────────────

TEST(Check, AcceptsMatchingVersions) {
    EXPECT_EQ(graft::plugins::check(make_info(GRAFT_ABI_VERSION, GRAFT_LAYOUT_VERSION)),
              GRAFT_OK);
}

TEST(Check, RejectsForeignAbi) {
    EXPECT_EQ(graft::plugins::check(make_info(GRAFT_ABI_VERSION + 1, GRAFT_LAYOUT_VERSION)),
              GRAFT_ERR_ABI);
}

// Раскладка ломается отдельно от интерфейса: смещения запечены в код плагина.
TEST(Check, RejectsForeignLayoutEvenWhenAbiMatches) {
    EXPECT_EQ(graft::plugins::check(make_info(GRAFT_ABI_VERSION, GRAFT_LAYOUT_VERSION + 1)),
              GRAFT_ERR_LAYOUT);
}

TEST(Check, RejectsTruncatedStruct) {
    auto info = make_info(GRAFT_ABI_VERSION, GRAFT_LAYOUT_VERSION);
    info.size = 8;
    EXPECT_EQ(graft::plugins::check(info), GRAFT_ERR_ABI);
}

// ── Слияние реестров ─────────────────────────────────────────────────────────

TEST(Merge, KeepsEverythingWhenNamesDiffer) {
    const auto a = make(nullptr, "Alpha");
    const auto b = make(nullptr, "Beta");
    std::vector<collision> bad;
    const auto kept = graft::plugins::merge({{&a, "one"}, {&b, "two"}}, bad);
    EXPECT_EQ(kept.size(), 2u);
    EXPECT_TRUE(bad.empty());
}

TEST(Merge, RejectsSecondClaimOfTheSameGlobal) {
    const auto a = make(nullptr, "Same");
    const auto b = make(nullptr, "Same");
    std::vector<collision> bad;
    const auto kept = graft::plugins::merge({{&a, "one"}, {&b, "two"}}, bad);
    ASSERT_EQ(kept.size(), 1u);
    EXPECT_STREQ(kept[0].owner, "one");   // побеждает первый по порядку загрузки
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_EQ(bad[0].first, "one");
    EXPECT_EQ(bad[0].second, "two");
    EXPECT_EQ(bad[0].name, "Same");
}

// Метод — это пара <класс, имя>: одноимённые методы разных классов не конфликтуют.
TEST(Merge, SameMethodNameOnDifferentClassesIsFine) {
    const auto a = make("Alpha", "Count");
    const auto b = make("Beta", "Count");
    std::vector<collision> bad;
    const auto kept = graft::plugins::merge({{&a, "one"}, {&b, "two"}}, bad);
    EXPECT_EQ(kept.size(), 2u);
    EXPECT_TRUE(bad.empty());
}

TEST(Merge, SameMethodOnTheSameClassCollides) {
    const auto a = make("Alpha", "Count");
    const auto b = make("Alpha", "Count");
    std::vector<collision> bad;
    const auto kept = graft::plugins::merge({{&a, "one"}, {&b, "two"}}, bad);
    EXPECT_EQ(kept.size(), 1u);
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_EQ(bad[0].class_name, "Alpha");
}

// Глобальная функция и метод с одним именем — разные сущности.
TEST(Merge, GlobalDoesNotCollideWithMethod) {
    const auto a = make(nullptr, "Count");
    const auto b = make("Alpha", "Count");
    std::vector<collision> bad;
    const auto kept = graft::plugins::merge({{&a, "one"}, {&b, "two"}}, bad);
    EXPECT_EQ(kept.size(), 2u);
    EXPECT_TRUE(bad.empty());
}

// Два плагина, вешающие состояние на один скриптовый класс, оба хотят NativeDispose и
// оба печатают деструктор — этого Enforce не примет. Ловим на слиянии.
TEST(Merge, ReportsDisposeCollisionOnSharedClass) {
    const auto a = make("Shared", "NativeDispose_one");
    const auto b = make("Shared", "NativeDispose_two");
    std::vector<collision> bad;
    graft::plugins::merge({{&a, "one"}, {&b, "two"}}, bad);
    EXPECT_TRUE(graft::plugins::disposes_clash({{&a, "one"}, {&b, "two"}}));
}

TEST(Merge, SingleDisposeOnAClassIsFine) {
    const auto a = make("Shared", "NativeDispose_one");
    const auto b = make("Shared", "Count");
    EXPECT_FALSE(graft::plugins::disposes_clash({{&a, "one"}, {&b, "two"}}));
}

TEST(Describe, MentionsBothPluginsAndTheName) {
    const collision c{"Alpha", "Count", "one", "two"};
    const std::string text = graft::plugins::describe(c);
    EXPECT_NE(text.find("Alpha.Count"), std::string::npos);
    EXPECT_NE(text.find("one"), std::string::npos);
    EXPECT_NE(text.find("two"), std::string::npos);
}

}  // namespace
