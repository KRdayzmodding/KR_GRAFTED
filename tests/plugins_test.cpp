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

// ── Каталог профиля ──────────────────────────────────────────────────────────
// В него ложатся оба журнала — рядом со script- и crash-логами сервера.

TEST(ProfileDir, TakesTheSwitchValue) {
    EXPECT_EQ(graft::plugins::profile_dir("game.exe -profiles=DEBUG/profiles -port=1"),
              "DEBUG/profiles");
}

TEST(ProfileDir, HandlesQuotedPathWithSpaces) {
    EXPECT_EQ(graft::plugins::profile_dir(R"(game.exe -profiles="C:\My Server\prof" -mod=@A;)"),
              R"(C:\My Server\prof)");
}

// Хвостовой разделитель снимаем: к пути дописывается имя файла.
TEST(ProfileDir, DropsTrailingSeparator) {
    EXPECT_EQ(graft::plugins::profile_dir(R"(game.exe -profiles=prof\)"), "prof");
}

TEST(ProfileDir, EmptyWhenNoSwitch) {
    EXPECT_TRUE(graft::plugins::profile_dir("game.exe -server -mod=@A;").empty());
}

// Тот же разбор границы, что и у -mod=: ключ внутри чужого значения не ключ.
TEST(ProfileDir, RequiresSwitchToStartAtBoundary) {
    EXPECT_EQ(graft::plugins::profile_dir("game.exe -mod=@A-profiles=NOPE; -profiles=YES"), "YES");
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

// ── Причина отказа ───────────────────────────────────────────────────────────
// Строка уходит в журнал человеку, у которого мод не завёлся. Из неё обязано быть видно,
// какие числа разошлись, у кого они старее и что именно пересобирать или обновлять.

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string num(std::uint32_t v) {
    return std::to_string(v);
}

TEST(Reason, OkSaysOk) {
    EXPECT_EQ(graft::plugins::reason(make_info(GRAFT_ABI_VERSION, GRAFT_LAYOUT_VERSION), GRAFT_OK),
              "ok");
}

// Плагин старее хоста: чинится пересборкой плагина, хост трогать незачем.
TEST(Reason, OlderPluginAbiNamesBothNumbersAndAsksToRebuildPlugin) {
    const auto text = graft::plugins::reason(
        make_info(GRAFT_ABI_VERSION - 1, GRAFT_LAYOUT_VERSION), GRAFT_ERR_ABI);
    EXPECT_TRUE(has(text, "ABI " + num(GRAFT_ABI_VERSION - 1))) << text;
    EXPECT_TRUE(has(text, "ABI " + num(GRAFT_ABI_VERSION))) << text;
    EXPECT_TRUE(has(text, "пересобрать плагин")) << text;
    EXPECT_FALSE(has(text, "обновить хост")) << text;
}

// Плагин новее хоста: пересборка плагина не поможет, отстал хост.
TEST(Reason, NewerPluginAbiAsksToUpdateHost) {
    const auto text = graft::plugins::reason(
        make_info(GRAFT_ABI_VERSION + 1, GRAFT_LAYOUT_VERSION), GRAFT_ERR_ABI);
    EXPECT_TRUE(has(text, "ABI " + num(GRAFT_ABI_VERSION + 1))) << text;
    EXPECT_TRUE(has(text, "обновить хост")) << text;
    EXPECT_FALSE(has(text, "пересобрать плагин")) << text;
}

TEST(Reason, OlderPluginLayoutNamesBothNumbers) {
    const auto text = graft::plugins::reason(
        make_info(GRAFT_ABI_VERSION, GRAFT_LAYOUT_VERSION - 1), GRAFT_ERR_LAYOUT);
    EXPECT_TRUE(has(text, "LAYOUT " + num(GRAFT_LAYOUT_VERSION - 1))) << text;
    EXPECT_TRUE(has(text, "LAYOUT " + num(GRAFT_LAYOUT_VERSION))) << text;
    EXPECT_TRUE(has(text, "пересобрать плагин")) << text;
}

TEST(Reason, NewerPluginLayoutAsksToUpdateHost) {
    const auto text = graft::plugins::reason(
        make_info(GRAFT_ABI_VERSION, GRAFT_LAYOUT_VERSION + 1), GRAFT_ERR_LAYOUT);
    EXPECT_TRUE(has(text, "обновить хост")) << text;
}

// Разошлось оба числа — показать оба, а не только первое попавшееся.
TEST(Reason, BothMismatchesAreReported) {
    const auto text = graft::plugins::reason(
        make_info(GRAFT_ABI_VERSION - 1, GRAFT_LAYOUT_VERSION + 1), GRAFT_ERR_ABI);
    EXPECT_TRUE(has(text, "ABI " + num(GRAFT_ABI_VERSION - 1))) << text;
    EXPECT_TRUE(has(text, "LAYOUT " + num(GRAFT_LAYOUT_VERSION + 1))) << text;
}

// Плагин отказал сам и своих чисел не сообщил (собран до того, как отказ стал их
// сообщать). Гадать нельзя — но версии хоста и что делать назвать обязаны.
TEST(Reason, SilentRefusalStillNamesHostNumbers) {
    graft_plugin_info info{};
    const auto text = graft::plugins::reason(info, GRAFT_ERR_ABI);
    EXPECT_TRUE(has(text, "ABI " + num(GRAFT_ABI_VERSION))) << text;
    EXPECT_TRUE(has(text, "LAYOUT " + num(GRAFT_LAYOUT_VERSION))) << text;
    EXPECT_TRUE(has(text, "не сообщил")) << text;
    EXPECT_TRUE(has(text, "пересобрать плагин")) << text;
}

TEST(Reason, TruncatedStructNamesSizes) {
    auto info = make_info(GRAFT_ABI_VERSION, GRAFT_LAYOUT_VERSION);
    info.size = 8;
    const auto text = graft::plugins::reason(info, GRAFT_ERR_ABI);
    EXPECT_TRUE(has(text, "8")) << text;
    EXPECT_TRUE(has(text, num(sizeof(graft_plugin_info)))) << text;
}

TEST(Reason, UnknownCodeIsShown) {
    const auto text = graft::plugins::reason(make_info(GRAFT_ABI_VERSION, GRAFT_LAYOUT_VERSION), 42);
    EXPECT_TRUE(has(text, "42")) << text;
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

TEST(Describe, MentionsBothPluginsAndTheName) {
    const collision c{"Alpha", "Count", "one", "two"};
    const std::string text = graft::plugins::describe(c);
    EXPECT_NE(text.find("Alpha.Count"), std::string::npos);
    EXPECT_NE(text.find("one"), std::string::npos);
    EXPECT_NE(text.find("two"), std::string::npos);
}

}  // namespace
