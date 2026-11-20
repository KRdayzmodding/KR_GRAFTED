// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <intrin.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Сколько кадра движка съедает graft — ТОЛЬКО ХОСТ. Ключ `-graftProfile=<сек>`: раз в столько секунд в журнал
// graft — доля времени, средний и худший кадр, самые дорогие вызовы (обработчики, таймеры, задания) по DLL и
// «файл:строка». Без ключа — одно чтение числа на кадр и на вызов: счётчики тактов не читаются вовсе.
namespace graft::profile {

// Интервал отчёта, секунд; 0 — выключен. Из командной строки, один раз — при загрузке хоста.
extern const std::uint32_t every_s;

inline bool on() {
    return every_s != 0;
}

inline std::uint64_t now() {
    return __rdtsc();
}

// Накопитель окна: graft в кадре и по местам вызова. Отдельно от часов — проверяется без игры.
class meter {
public:
    void frame(std::uint64_t cycles);
    void call(void* code, std::string_view where, std::uint64_t cycles);

    // Текст отчёта за окно: ns_per_cycle — из часов окна, seconds — его длина. top — сколько мест назвать.
    std::string report(double ns_per_cycle, double seconds, std::size_t top = 8) const;
    void        clear();

    std::uint64_t frames() const { return frames_; }

private:
    // Место вызова — по адресу строки «файл:строка» у записи хаба: хэш строки в каждом кадре стоил
    // бы дороже самого вызова и врал бы в «сам graft». Совпал адрес — сверяется и текст: запись
    // могла умереть, а адрес достаться другой.
    struct stat {
        const char*   site = nullptr;
        std::string   where;
        void*         code   = nullptr;
        std::uint64_t calls  = 0;
        std::uint64_t cycles = 0;
        std::uint64_t worst  = 0;
    };

    std::uint64_t     frames_ = 0;
    std::uint64_t     cycles_ = 0;
    std::uint64_t     worst_  = 0;
    std::uint64_t     called_ = 0; // такты всех вызовов: «сам graft» — кадр без них
    std::vector<stat> calls_;      // shortcut: перебор, мест — десятки; станут сотни — карта по site
};

// Кадр хоста: graft за кадр, отчёт — когда окно прошло. Только поток скрипта.
void frame(std::uint64_t cycles);
// Вызов чужого кода хабом (обработчик, таймер, задание). Только поток скрипта.
void call(void* code, std::string_view where, std::uint64_t cycles);

} // namespace graft::profile
