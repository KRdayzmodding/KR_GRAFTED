// Экспорт плагина с чужим хостом: настоящая DLL-фикстура, не пересказ. Отказывая, плагин
// обязан назвать себя и свои числа — иначе в журнале хоста останется «?» и гадание,
// что пересобирать. Фикстура лежит рядом с graft_tests (одна выходная папка).
#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "graft/abi.h"

namespace {

graft_plugin_entry_fn fixture_entry() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto dll = std::filesystem::path{exe}.parent_path() / L"SIXW_HASHMAP.grafted.dll";
    // Не выгружаем: плагины не выгружаются и в игре, строки описания живут в их образе.
    HMODULE mod = LoadLibraryW(dll.c_str());
    if (!mod) {
        return nullptr;
    }
    return reinterpret_cast<graft_plugin_entry_fn>(GetProcAddress(mod, GRAFT_PLUGIN_ENTRY_NAME));
}

// Хост «из другого времени»: плагин вправе прочесть только заголовок, дальше — ни шагу.
graft_host_api foreign_host(std::uint32_t abi, std::uint32_t layout) {
    graft_host_api host{};
    host.size   = sizeof(graft_host_api);
    host.abi    = abi;
    host.layout = layout;
    return host;
}

void expect_header_filled(const graft_plugin_info& info) {
    EXPECT_EQ(info.size, sizeof(graft_plugin_info));
    EXPECT_EQ(info.abi, GRAFT_ABI_VERSION);
    EXPECT_EQ(info.layout, GRAFT_LAYOUT_VERSION);
    ASSERT_NE(info.name, nullptr);
    EXPECT_EQ(std::string_view{info.name}, "SIXW_HASHMAP");
    EXPECT_EQ(info.version, 1u);
    // Отказ — не загрузка: нативов не отдаёт.
    EXPECT_EQ(info.count, 0u);
    EXPECT_EQ(info.natives, nullptr);
}

TEST(PluginEntry, RefusingForeignAbiReportsOwnHeader) {
    const auto entry = fixture_entry();
    ASSERT_NE(entry, nullptr) << "нет SIXW_HASHMAP.grafted.dll рядом с graft_tests";
    const auto        host = foreign_host(GRAFT_ABI_VERSION + 1, GRAFT_LAYOUT_VERSION);
    graft_plugin_info info{};
    EXPECT_EQ(entry(&host, &info), GRAFT_ERR_ABI);
    expect_header_filled(info);
}

TEST(PluginEntry, RefusingForeignLayoutReportsOwnHeader) {
    const auto entry = fixture_entry();
    ASSERT_NE(entry, nullptr);
    const auto        host = foreign_host(GRAFT_ABI_VERSION, GRAFT_LAYOUT_VERSION + 1);
    graft_plugin_info info{};
    EXPECT_EQ(entry(&host, &info), GRAFT_ERR_LAYOUT);
    expect_header_filled(info);
}

TEST(PluginEntry, RefusingTruncatedHostReportsOwnHeader) {
    const auto entry = fixture_entry();
    ASSERT_NE(entry, nullptr);
    auto host = foreign_host(GRAFT_ABI_VERSION, GRAFT_LAYOUT_VERSION);
    host.size = 12;
    graft_plugin_info info{};
    EXPECT_EQ(entry(&host, &info), GRAFT_ERR_ABI);
    expect_header_filled(info);
}

std::vector<std::string> g_host_log;

void capture_log(const char* line) {
    g_host_log.emplace_back(line);
}

// GRAFT_ON_LOAD зовут из graft_plugin_entry, когда хост уже представился: graft::log из
// обработчика доходит до журнала хоста. Статический хост — плагин помнит указатель на него.
TEST(PluginEntry, OnLoadRunsOnceTheHostHasIntroduced) {
    const auto entry = fixture_entry();
    ASSERT_NE(entry, nullptr);
    static auto host = foreign_host(GRAFT_ABI_VERSION, GRAFT_LAYOUT_VERSION);
    host.log         = &capture_log;
    graft_plugin_info info{};
    EXPECT_EQ(entry(&host, &info), GRAFT_OK);
    // Следом может быть строка «событий нет»: у этого хоста полей событий нет, а у фикстуры есть
    // объявленная подписка. Главное — on_load позван и позван первым.
    ASSERT_FALSE(g_host_log.empty());
    EXPECT_EQ(g_host_log.front(), "fixture: on_load");
    EXPECT_EQ(std::ranges::count(g_host_log, std::string{"fixture: on_load"}), 1);
}

// Хост 0.5 (ABI 7, без полей «С 0.6») плагин 0.6 принимает: события ему просто недоступны,
// а post не течёт — замыкание отпущено. Идёт после OnLoad: on_load зовут один раз.
TEST(PluginEntry, AcceptsHostWithoutEventServices) {
    const auto entry = fixture_entry();
    ASSERT_NE(entry, nullptr);
    static auto host = foreign_host(GRAFT_ABI_VERSION, GRAFT_LAYOUT_VERSION);
    host.size        = offsetof(graft_host_api, post);
    host.log         = &capture_log;
    graft_plugin_info info{};
    EXPECT_EQ(entry(&host, &info), GRAFT_OK);
    const auto probe = reinterpret_cast<int (*)()>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"SIXW_HASHMAP.grafted.dll"), "fixture_events_probe")));
    ASSERT_NE(probe, nullptr);
    EXPECT_EQ(probe(), 0);
}

// Подписка, объявленная вне функций (graft::on<E> в файле плагина), строится раньше хоста и
// уходит ему из точки входа — без GRAFT_ON_LOAD. Хосты прошлых тестов событий не имели: заявка ждала.
struct fake_events {
    std::vector<std::string> topics;
    void (*handler)(void*, const void*) = nullptr;
    void* ctx                           = nullptr;
};

fake_events g_events;

uint64_t fake_subscribe(const char* topic, uint32_t, void (*handler)(void*, const void*), void (*)(void*), void* ctx, const char*) {
    g_events.topics.emplace_back(topic);
    g_events.handler = handler;
    g_events.ctx     = ctx;
    return 42;
}

TEST(PluginEntry, DeclaredSubscriptionReachesHostAtEntry) {
    const auto entry = fixture_entry();
    ASSERT_NE(entry, nullptr);
    static auto host = foreign_host(GRAFT_ABI_VERSION, GRAFT_LAYOUT_VERSION);
    host.log         = &capture_log;
    host.post        = [](void (*)(void*), void (*drop)(void*), void* ctx, const char*) -> uint8_t {
        drop(ctx);
        return 1;
    };
    host.on_script_thread = []() -> uint8_t { return 1; };
    host.subscribe        = &fake_subscribe;
    host.unsubscribe      = [](uint64_t) {};
    host.publish          = [](const char*, uint32_t, const void*, uint8_t) -> uint8_t { return 1; };
    host.add_timer        = [](uint32_t, uint32_t, void (*)(void*), void (*drop)(void*), void* ctx, const char*) -> uint64_t {
        drop(ctx);
        return 0;
    };
    host.topic         = [](const char*, uint32_t, const char*) -> const graft_topic* { return nullptr; };
    host.publish_topic = [](const graft_topic*, const void*, uint8_t) -> uint8_t { return 1; };
    host.watch         = [](const char*, uint32_t, void (*)(void*, uint8_t), void (*drop)(void*), void* ctx, const char*) -> uint64_t {
        drop(ctx);
        return 0;
    };
    graft_plugin_info info{};
    ASSERT_EQ(entry(&host, &info), GRAFT_OK);
    ASSERT_EQ(g_events.topics, std::vector<std::string>{"fixture.ping"});

    struct {
        int n = 3;
    } ping;

    g_events.handler(g_events.ctx, &ping);
    const auto pinged = reinterpret_cast<int (*)()>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"SIXW_HASHMAP.grafted.dll"), "fixture_pinged")));
    ASSERT_NE(pinged, nullptr);
    EXPECT_EQ(pinged(), 3);
}

// Объявления, которые сборка напечатала рядом с фикстурами (graft.exe protogen, POST_BUILD).
std::string generated(const wchar_t* plugin, const wchar_t* module) {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto file = std::filesystem::path{exe}.parent_path() /
                      (std::wstring{plugin} + L".scripts") / module /
                      (L"grafted_natives_" + std::wstring{plugin} + L".c");
    std::ifstream in(file, std::ios::binary);
    EXPECT_TRUE(in) << "нет " << file.string();
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Сквозь инструмент: файл вне 1_Core обёрнут в дефайн ИМЕННО этого плагина.
TEST(PluginProtogen, GameModuleFileIsWrappedInPluginDefine) {
    const std::string text = generated(L"SIXW_GRAFT", L"3_Game");
    const std::size_t open = text.find("#ifdef GRAFTED_SIXW_GRAFT\n");
    ASSERT_NE(open, std::string::npos) << text;
    EXPECT_LT(open, text.find("\nproto")) << text;
    EXPECT_TRUE(text.ends_with("#endif\n")) << text;
}

// 1_Core компилируется без дефайнов (проверено на сервере) — там обёртки нет.
TEST(PluginProtogen, CoreModuleFileIsNotWrapped) {
    const std::string text = generated(L"SIXW_HASHMAP", L"1_Core");
    ASSERT_NE(text.find("class SeraphHashMap"), std::string::npos) << text;
    EXPECT_EQ(text.find("#if"), std::string::npos) << text;
}

} // namespace
