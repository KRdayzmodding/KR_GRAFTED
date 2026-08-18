// Скриптовое лицо менеджера. Реализация живёт в самом хосте, поэтому эти нативы
// участвуют в слиянии наравне с плагинскими — под именем "graft".
//
// Зачем это нужно. Мод, которому нужен натив, зависит от своего плагина ЖЁСТКО: без
// импла `proto native` скрипт не слинкуется, и мягкой деградации тут быть не может по
// природе вещей. А вот НЕОБЯЗАТЕЛЬНАЯ интеграция с ЧУЖИМ модом — случай настоящий:
// IsGrafted("OTHER") отвечает на него до первого вызова.
//
// Свободные функции, а не класс: классу пришлось бы где-то объявиться, а хост — не мод
// и своего PBO не имеет. Глобальные же имена движок принимает от кого угодно.
#include <algorithm>
#include <string>
#include <string_view>

#include <format>

#include <windows.h>

#include "graft/frame.hpp"
#include "graft/loader.hpp"
#include "graft/native.hpp"

namespace {

// Загруженный плагин с таким именем, либо nullptr. Отклонённые не считаются: для
// скрипта «есть» значит «его нативы работают».
const graft::loader::row* find_plugin(std::string_view name) {
    const auto& rows = graft::loader::rows();
    const auto it = std::ranges::find_if(
        rows, [&](const graft::loader::row& r) { return r.status == GRAFT_OK && r.name == name; });
    return it == rows.end() ? nullptr : &*it;
}

// Версия интерфейса хоста и версия раскладки движка: по ним видно, с чем имеешь дело.
int GraftVersion() {
    return static_cast<int>(GRAFT_ABI_VERSION);
}

int GraftLayoutVersion() {
    return static_cast<int>(GRAFT_LAYOUT_VERSION);
}

// Сколько модулей загружено, включая сам хост.
int GraftPluginCount() {
    return static_cast<int>(graft::loader::rows().size());
}

bool IsGrafted(std::string_view name) {
    return find_plugin(name) != nullptr;
}

// -1 — такого плагина нет (0 — законная версия, поэтому нулём отвечать нельзя).
int GraftPluginVersion(std::string_view name) {
    const graft::loader::row* r = find_plugin(name);
    return r ? static_cast<int>(r->version) : -1;
}

std::string GraftPluginAt(int index) {
    const auto& rows = graft::loader::rows();
    if (index < 0 || static_cast<std::size_t>(index) >= rows.size()) {
        return {};
    }
    const graft::loader::row& r = rows[static_cast<std::size_t>(index)];
    return std::format("{} {} {} {}", r.name, r.version, r.count,
                       r.status == GRAFT_OK ? "ok" : graft::plugins::explain(r.status));
}

// Сколько имён отклонено из-за спора двух плагинов. Ноль — здоровая установка.
int GraftCollisionCount() {
    return static_cast<int>(graft::loader::collisions().size());
}

std::string GraftCollisionAt(int index) {
    const auto& all = graft::loader::collisions();
    if (index < 0 || static_cast<std::size_t>(index) >= all.size()) {
        return {};
    }
    return graft::plugins::describe(all[static_cast<std::size_t>(index)]);
}

// ── Точка входа для C++ ──────────────────────────────────────────────────────
// Своего потока у библиотеки нет: строковый аллокатор движка без блокировок, кадр
// вызова у скриптового потока свой. Поэтому C++ просыпается там же, где живёт скрипт, —
// мод зовёт это из OnUpdate, а хост разводит веером по всем плагинам.
// game — корень объектного графа (GetGame()). Он приходит здесь, а не берётся движком
// сам, потому что взять его неоткуда: EnScript.GetClassVar отдавать глобальные
// переменные отказывается, и это проверено — из самого скрипта тоже (кейс
// Out_GlobalVariableIsRefusedByEngine). Мод и так зовёт GraftTick одной строкой, так что
// корень приезжает бесплатно.
void GraftTick(float dt, graft::obj game) {
    graft::loader::set_script_root(game.ptr);
    graft::loader::run_ticks(dt);
}

// Сколько раз натив упал и был перехвачен. Ноль на живом сервере — здоровая установка;
// не ноль — в журнале написано, чей именно плагин и где.
int GraftFaultCount() {
    return static_cast<int>(graft::loader::fault_count());
}

std::string GraftLastFault() {
    return graft::loader::last_fault();
}

// dt последнего кадра — тот самый timeslice, что движок вручил скрипту.
float GraftFrameDt() {
    return graft::frame::last_dt();
}

// Состояние привязки к кадру одной строкой.
std::string GraftLoopProbe() {
    return std::format(
        "кадров {} dt {} | поток {} | корень {} | отсев: чужой класс {}, индекс -1 {} | "
        "привязка: {}",
        graft::frame::frames(), graft::frame::last_dt(), GetCurrentThreadId(),
        graft::loader::script_root(), graft::frame::rejected_class(),
        graft::frame::rejected_unresolved(), graft::frame::state());
}

// Тот ли объект приезжает первым аргументом движковой точки входа кадра, что скрипт
// знает как GetGame(). Сравнение с тем, что мод передаёт в GraftTick, — единственная
// доступная проверка, и кейс сьюты обязан её сторожить.
bool GraftRootstockMatches(graft::obj game) {
    return game.ptr != nullptr && game.ptr == graft::loader::script_root();
}

// Сколько кадров движка библиотека уже поймала сама, без строки в скрипте.
int GraftFrames() {
    return static_cast<int>(graft::frame::frames());
}

// Сколько обработчиков подписалось. Ноль на живом сервере значит, что либо плагин их не
// ставил, либо мод не зовёт GraftTick, — и различить это надо до того, как «почему-то
// не работает».
int GraftTickCount() {
    return static_cast<int>(graft::loader::tick_count());
}

GRAFT_BINDINGS("1_Core") {
    bind.global<&GraftFaultCount>("GraftFaultCount")
        .global<&GraftLastFault>("GraftLastFault")
        .global<&GraftLoopProbe>("GraftLoopProbe")
        .global<&GraftRootstockMatches>("GraftRootstockMatches")
        .global<&GraftFrames>("GraftFrames")
        .global<&GraftFrameDt>("GraftFrameDt")
        .global<&GraftTick>("GraftTick")
        .global<&GraftTickCount>("GraftTickCount")
        .global<&GraftVersion>("GraftVersion")
        .global<&GraftLayoutVersion>("GraftLayoutVersion")
        .global<&GraftPluginCount>("GraftPluginCount")
        .global<&IsGrafted>("IsGrafted")
        .global<&GraftPluginVersion>("GraftPluginVersion")
        .global<&GraftPluginAt>("GraftPluginAt")
        .global<&GraftCollisionCount>("GraftCollisionCount")
        .global<&GraftCollisionAt>("GraftCollisionAt");
}

}  // namespace
