// Hello world: одна функция C++, видимая из скриптов DayZ как обычный натив.
//
// Здесь ровно три вещи, и больше в плагине ничего обязательного нет:
//   GRAFT_PLUGIN   — паспорт: имя и версия, по одному разу на DLL;
//   обычная функция C++ — она же impl натива, никаких особых типов;
//   GRAFT_BINDINGS — привязка: имя в скрипте и модуль, в котором оно объявляется.
#include <format>
#include <string>
#include <string_view>

#include "graft/native.hpp"

GRAFT_PLUGIN("HELLO_GRAFT", 1);

namespace {

// string_view приезжает из скрипта без копии, возвращённый string движок копирует
// себе — владеть им и освобождать его не нужно.
std::string HelloGraft(std::string_view name) {
    return std::format("Hello from C++, {}!", name);
}

// "3_Game" — модуль Enforce, в котором появится объявление. Из него натив виден и в
// 4_World, и в 5_Mission, то есть почти везде, где пишут мод.
GRAFT_BINDINGS("3_Game") {
    bind.global<&HelloGraft>("HelloGraft");
}

}  // namespace
