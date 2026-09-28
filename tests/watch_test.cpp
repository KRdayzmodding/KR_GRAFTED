// Аппаратная точка останова на запись — инструмент разбора (INCLUDE/graft/watch.hpp).
//
// Проверяется без игры: железу всё равно, чья память. Кейс взводит точку на свою ячейку,
// пишет в неё из ОТДЕЛЬНОЙ функции (чтобы обратный адрес был настоящим кадром) и смотрит,
// что в кольце лежит новое значение и кадр записи.
#include <gtest/gtest.h>

#include <cstdint>

#include "graft/watch.hpp"

namespace {

std::uint64_t g_cell = 0; // статическая и выровненная: точка на 8 байт требует кратности

// noinline: врезанный в кейс `mov` дал бы кадром сам кейс, и проверка обратного адреса
// перестала бы что-либо проверять.
__declspec(noinline) void poke(std::uint64_t v) {
    g_cell = v;
}

} // namespace

TEST(Watch, CatchesWriteAndNamesTheWriter) {
    graft::watch::head.store(0);
    graft::watch::tail     = 0;
    graft::watch::chains_n = 0;

    ASSERT_TRUE(graft::watch::arm({{&g_cell, "cell", 8}}));
    {
        const graft::watch::own mark; // своя запись — так она и должна быть помечена
        poke(0x1234);
    }
    graft::watch::disarm();

    ASSERT_GE(graft::watch::head.load(), 1u) << "точка не сработала";
    const graft::watch::hit& h = graft::watch::ring[0];
    EXPECT_EQ(h.which, 0);
    EXPECT_TRUE(h.ours);
    // Правило 3: ловушка поднимается ПОСЛЕ записи, поэтому значение уже новое.
    EXPECT_EQ(h.value, 0x1234u);
    EXPECT_NE(h.frames[0], 0u);
    // Кадр записи — в этом же модуле, значит where() найдёт для него начало функции.
    EXPECT_NE(graft::watch::where(h.frames[0]).find("/f0x"), std::string::npos);

    graft::watch::drain("кейс");
    EXPECT_EQ(graft::watch::chains_n, 1) << "срабатывания не свернулись в одну цепочку";
    graft::watch::report();
    EXPECT_EQ(graft::watch::chains_n, 0);
}

// Невыровненный адрес железо молча игнорирует — отказывать надо до записи в регистры.
TEST(Watch, RefusesUnalignedSpot) {
    auto* const misaligned = reinterpret_cast<std::uint8_t*>(&g_cell) + 1;
    EXPECT_FALSE(graft::watch::arm({{misaligned, "кривой", 8}}));
    EXPECT_FALSE(graft::watch::arm({{&g_cell, "длина", 3}}));
    EXPECT_FALSE(graft::watch::arm({{nullptr, "ноль", 8}}));
    EXPECT_EQ(graft::watch::watched, 0u) << "отказ всё же тронул регистры";
}
