// Пример ОБРАТНОГО направления: не «скрипт зовёт C++», а «C++ работает с игрой».
//
// Раз в пять секунд обходим игроков, читаем их steam id и позицию, кладём в свою
// структуру C++ — и отдаём наружу одним нативом. На стороне скрипта НЕТ НИЧЕГО: кадр
// библиотека берёт у движка сама, и корень объектного графа приходит вместе с ним.
//
// Что здесь показано:
//   GRAFT_ON_TICK          точка входа: C++ просыпается на скриптовом потоке
//   graft::game()          корень объектного графа (тот же GetGame())
//   graft::dayz::*         зеркало движкового API — настоящие методы с сигнатурами
//   graft::scratch<T>()    буфер под out-аргумент движка, один на процесс
//   graft::borrowed<T>     чужая ссылка: действительна до конца вызова, и это проверяется
//   obj["m_Field"_f]       поле объекта как lvalue
//   graft::print()         строка в script-лог игры прямо из C++
//
// Зеркало движкового API нужно ВСЕГДА: пример написан против настоящих классов игры, а не
// против ref<"CGame"> вручную. Генерится из скриптов игры один раз (и заново после патча):
//   graft apigen <PDrive>/scripts src/graft/dayz
#include <map>
#include <string>

#include "graft/dayz/3_Game.hpp"
#include "graft/engine.hpp"
#include "graft/native.hpp"

using namespace std;

namespace {

// Наши данные — обычный C++. Никаких скриптовых типов: имя и позиция это ЗНАЧЕНИЯ, и
// хранить надо именно их. Ссылку на игрока между тиками держать нельзя — движок волен
// удалить объект, а память переиспользовать.
struct player_row {
    string name;
    graft::vector position;
    int seen_ticks = 0;
};

// less<> — прозрачное сравнение: поиск по string_view идёт без временной строки и
// без аллокации. Игроков на сервере десятки, поэтому дерево тут не хуже хеша, а кода
// меньше: у unordered_map тот же трюк требует своих хешера и компаратора.
map<string, player_row, less<>> g_players;
float g_since_scan = 0;
float g_uptime = 0;   // сколько игрового времени C++ уже отработал своим тиком

// Корень объектного графа. Пусто до первого кадра — это не ошибка, а «ещё рано».
graft::dayz::CGame game() {
    return graft::cast<graft::dayz::CGame>(graft::game());
}

void scan_players() {
    const graft::dayz::CGame world = game();
    // Буфер под out-аргумент. scratch — ОДИН объект на тип на весь процесс: движок
    // считает ссылки сам, а мы ими не управляем, поэтому владения не изображаем.
    const auto players = graft::scratch<graft::array<graft::dayz::Man>>();
    if (!world || !players) {
        // Молчать тут нельзя: «известно игроков: 0» в логе мода ничем не отличается от
        // пустого сервера, и разбираться будет не с чем. Системный журнал для того и
        // есть — он про библиотеку, а не про мод.
        graft::log(world ? "[EXAMPLE_PLAYERS] нет array<Man> — сканировать нечем"
                         : "[EXAMPLE_PLAYERS] корень объектного графа ещё не пришёл");
        return;
    }
    players.clear();
    world.GetPlayers(players);

    int without_identity = 0;
    for (const graft::dayz::Man man : players) {
        const graft::dayz::PlayerIdentity id = man.GetIdentity();
        if (!id) {
            ++without_identity;
            continue;  // игрок ещё не представился
        }
        // GetPlainId объявлен как `proto`, а не `proto native` — библиотека сама выбирает
        // маршалируемый путь, снаружи разницы нет. Спрашиваем один раз: это вызов в
        // движок, а не чтение поля.
        const string steam = id.GetPlainId();
        player_row& row = g_players[steam];
        row.name = id.GetName();
        row.position = man.GetOrigin();
        ++row.seen_ticks;
        if (row.seen_ticks == 1) {
            // Строка уходит в SCRIPT-ЛОГ игры — это Print самой игры, только позванный
            // из C++. print(), а не log(), потому что читать её будет админ мода, а не
            // тот, кто разбирает библиотеку.
            graft::print(format("новый игрок: {} ({})", row.name, steam));
        }
    }

    // Что движок отдал и сколько из этого пригодилось — по этой строке видно, чей ноль:
    // движка (никого не вернул) или наш (вернул, а мы не разобрали). Пишем при смене
    // картины, а не каждые пять секунд.
    static int said = -1;
    const int seen = static_cast<int>(players.size());
    if (seen != said) {
        said = seen;
        graft::log(format("[EXAMPLE_PLAYERS] движок отдал {} игроков, без identity {}", seen,
                          without_identity));
    }
}

// Точка входа: своего потока у библиотеки нет и быть не должно — всё обязано идти по
// скриптовому. Библиотека будит нас раз в кадр сама, dt — движковый timeslice.
GRAFT_ON_TICK(dt) {
    g_uptime += dt;
    g_since_scan += dt;
    if (g_since_scan < 5.0f) {
        return;
    }
    g_since_scan = 0;
    scan_players();
}

// ── Что мод может спросить у нас ─────────────────────────────────────────────

// Тик виден снаружи даже на пустом сервере: секунды копит С++, а не скрипт.
float ExampleUptime() {
    return g_uptime;
}

int ExamplePlayersKnown() {
    return static_cast<int>(g_players.size());
}

string ExamplePlayerReport(string_view steam_id) {
    const auto found = g_players.find(steam_id);
    if (found == g_players.end()) {
        return "неизвестен";
    }
    const player_row& row = found->second;
    return format("{} @ {:.0f} {:.0f} {:.0f}, тиков {}", row.name, row.position.x,
                       row.position.y, row.position.z, row.seen_ticks);
}

// Поле движкового объекта — синтаксисом sol2, но имя живёт в типе, поэтому номер слота
// ищется один раз на класс, а не на обращение.
int ExampleDebugMonitor() {
    using namespace graft::literals;
    if (const graft::dayz::CGame world = game()) {
        return world["m_DebugMonitorEnabled"_f];
    }
    return -1;
}

GRAFT_BINDINGS("1_Core") {
    bind.global<&ExampleUptime>("ExampleUptime")
        .global<&ExamplePlayersKnown>("ExamplePlayersKnown")
        .global<&ExamplePlayerReport>("ExamplePlayerReport")
        .global<&ExampleDebugMonitor>("ExampleDebugMonitor");
}

}  // namespace
