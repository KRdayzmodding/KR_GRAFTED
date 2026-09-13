// Экспорт плагина с чужим хостом: настоящая DLL-фикстура, не пересказ. Отказывая, плагин
// обязан назвать себя и свои числа — иначе в журнале хоста останется «?» и гадание,
// что пересобирать. Фикстура лежит рядом с graft_tests (одна выходная папка).
#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <string_view>

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
    host.size = sizeof(graft_host_api);
    host.abi = abi;
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
    const auto host = foreign_host(GRAFT_ABI_VERSION + 1, GRAFT_LAYOUT_VERSION);
    graft_plugin_info info{};
    EXPECT_EQ(entry(&host, &info), GRAFT_ERR_ABI);
    expect_header_filled(info);
}

TEST(PluginEntry, RefusingForeignLayoutReportsOwnHeader) {
    const auto entry = fixture_entry();
    ASSERT_NE(entry, nullptr);
    const auto host = foreign_host(GRAFT_ABI_VERSION, GRAFT_LAYOUT_VERSION + 1);
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

}  // namespace
