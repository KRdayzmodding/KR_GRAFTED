// Наблюдение за BEClient_x64.dll: сопоставление имени и — главное — что уведомление
// загрузчика действительно приходит. Проверяется настоящей загрузкой настоящей DLL,
// переименованной в BEClient_x64.dll: BattlEye для этого не нужен, а механизм тот же,
// которым движок втянет его в процесс (LoadLibraryA).
#include <gtest/gtest.h>

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <string>

#include "graft/battleye.hpp"

namespace {

namespace be = graft::battleye;

// ── Имя ──────────────────────────────────────────────────────────────────────

TEST(BattlEyeName, MatchesTheFileNameAndAFullPath) {
    EXPECT_TRUE(be::is_client_module(L"BEClient_x64.dll"));
    EXPECT_TRUE(be::is_client_module(L"F:\\DayZ\\BattlEye\\BEClient_x64.dll"));
    EXPECT_TRUE(be::is_client_module(L"F:/DayZ/BattlEye/BEClient_x64.dll"));
}

TEST(BattlEyeName, IgnoresCase) {
    EXPECT_TRUE(be::is_client_module(L"beclient_x64.DLL"));
    EXPECT_TRUE(be::is_client_module(L"BECLIENT_X64.DLL"));
}

TEST(BattlEyeName, RejectsNeighbours) {
    EXPECT_FALSE(be::is_client_module(L""));
    EXPECT_FALSE(be::is_client_module(L"BEClient_x86.dll"));
    EXPECT_FALSE(be::is_client_module(L"xBEClient_x64.dll"));
    EXPECT_FALSE(be::is_client_module(L"BEClient_x64.dll.bak"));
    EXPECT_FALSE(be::is_client_module(L"BEServer_x64.dll"));
    // каталог с таким именем — не модуль
    EXPECT_FALSE(be::is_client_module(L"C:\\BEClient_x64.dll\\other.dll"));
}

// ── Наблюдение ───────────────────────────────────────────────────────────────

std::atomic<int> g_hits{0};

void count() {
    ++g_hits;
}

// Настоящая DLL под именем BEClient_x64.dll: копия version.dll, которая есть на любой
// Windows и грузится без побочных эффектов.
std::wstring decoy() {
    static const std::wstring path = [] {
        wchar_t                     sys[MAX_PATH];
        const UINT                  n   = GetSystemDirectoryW(sys, MAX_PATH);
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "graft_be_test";
        std::filesystem::create_directories(dir);
        const std::filesystem::path out = dir / L"BEClient_x64.dll";
        std::filesystem::copy_file(std::filesystem::path{std::wstring(sys, n)} / L"version.dll", out, std::filesystem::copy_options::overwrite_existing);
        return out.wstring();
    }();
    return path;
}

// Каждый кейс снимает наблюдение сам, иначе подписка пережила бы его и сработала в чужом.
struct Watching : ::testing::Test {
    void SetUp() override { g_hits = 0; }

    void TearDown() override { be::unwatch(); }
};

TEST_F(Watching, FiresWhenTheModuleArrivesLater) {
    ASSERT_TRUE(be::watch(&count));
    EXPECT_EQ(g_hits, 0); // в тестовом процессе BattlEye нет
    HMODULE m = LoadLibraryW(decoy().c_str());
    ASSERT_NE(m, nullptr);
    EXPECT_GE(g_hits, 1);
    FreeLibrary(m);
}

TEST_F(Watching, FiresAtOnceWhenTheModuleIsAlreadyThere) {
    // Щель между «проверили» и «подписались» закрыта порядком: подписка, потом проверка.
    HMODULE m = LoadLibraryW(decoy().c_str());
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(be::watch(&count));
    EXPECT_EQ(g_hits, 1);
    FreeLibrary(m);
}

TEST_F(Watching, StaysQuietForOtherModules) {
    ASSERT_TRUE(be::watch(&count));
    HMODULE m = LoadLibraryW(L"winmm.dll");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(g_hits, 0);
    FreeLibrary(m);
}

TEST_F(Watching, SecondWatchIsRefused) {
    ASSERT_TRUE(be::watch(&count));
    EXPECT_FALSE(be::watch(&count));
}

TEST_F(Watching, NoCallbackNoWatch) {
    EXPECT_FALSE(be::watch(nullptr));
}

TEST_F(Watching, UnwatchStopsTheNotifications) {
    ASSERT_TRUE(be::watch(&count));
    be::unwatch();
    HMODULE m = LoadLibraryW(decoy().c_str());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(g_hits, 0);
    FreeLibrary(m);
}

} // namespace
