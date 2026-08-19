// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include "graft/stages.hpp"

#include <format>
#include <string>
#include <vector>

#include "graft/callout.hpp"
#include "graft/engine.hpp"
#include "graft/script.hpp"
#include "graft/types.hpp"

// Лестница этапов и слои. Здесь нет ни одной фичи: только состояние движка и те, кто его
// ждёт. Всё, что делает библиотека по ходу запуска, висит на подписках — см. install().
namespace graft::stage {
namespace {

step g_at = step::none;

struct waiter {
    step at;
    void (*fn)();
};

// Списки короткие (единицы), живут до конца процесса; порядок вызова — порядок подписки.
std::vector<waiter>& waiters() {
    static std::vector<waiter> all;
    return all;
}

std::vector<void (*)(const layer&)>& on_begin() {
    static std::vector<void (*)(const layer&)> all;
    return all;
}

std::vector<void (*)(const layer&)>& on_end() {
    static std::vector<void (*)(const layer&)> all;
    return all;
}

const char* name_of(step at) {
    switch (at) {
        case step::armed:
            return "врезка встала, плагины загружены";
        case step::linked:
            return "движок зовётся: классы, шаблоны переменных и глобали на месте";
        case step::running:
            return "пошли кадры, корень объектного графа есть";
        default:
            return "?";
    }
}

// ── Слои ─────────────────────────────────────────────────────────────────────
void* g_context = nullptr;  // контекст текущего слоя
layer g_layer;
bool g_open = false;

void close_layer() {
    if (!g_open) {
        return;
    }
    g_open = false;
    log(std::format("слой {}: конец ({} -> {})", g_layer.index,
                    g_layer.first_class ? g_layer.first_class : "?",
                    g_layer.last_class ? g_layer.last_class : "?"));
    for (void (*fn)(const layer&) : on_end()) {
        fn(g_layer);
    }
}

void open_layer(void* context, const char* class_name) {
    g_context = context;
    g_layer = layer{g_layer.index + 1, class_name, class_name};
    g_open = true;
    log(std::format("слой {}: начало ({})", g_layer.index, class_name ? class_name : "?"));
    for (void (*fn)(const layer&) : on_begin()) {
        fn(g_layer);
    }
}

// Условие этапа linked — «движок зовётся», и это три разные вещи, каждая нужна:
//
//   класс отвечает          find_method("string","Length")  — обход дескрипторов верен
//   есть шаблон переменной  donor_template(строка)          — иначе не собрать кадр
//                                                             вызова маршалируемого `proto`
//   глобали зарегистрированы find_global("Print")           — у них нет класса, и узнать
//                                                             их можно только с регистрации
//
// Проверяются ванильными именами, как и всё остальное в graft: они переживают патч, а
// смещения — нет. Порядок у движка свой (методы классов идут раньше глобалей), поэтому
// ждать надо всех троих, а не любого из них.
bool engine_answers() {
    // Ищем НАПРЯМУЮ, без памяти на пару <класс, имя>: у той памяти есть потолок попыток
    // (восемь промахов — и она сдаётся навсегда, см. detail::engine_method), а опрос
    // начинается с первой же регистрации, когда движок заведомо ещё молчит. Потолок
    // сгорел бы вхолостую, и этап не наступил бы никогда — проверено: шапка уезжала в
    // конец запуска. Здесь дорогой поиск оправдан: он идёт только ДО этапа.
    return static_cast<bool>(script::find_method("string", "Length")) &&
           ::graft::detail::donor_template(script::tag_string) != nullptr &&
           script::find_global("Print") != nullptr;
}

}  // namespace

bool reached(step at) {
    return static_cast<std::uint8_t>(g_at) >= static_cast<std::uint8_t>(at);
}

step current() {
    return g_at;
}

void on(step at, void (*fn)()) {
    if (!fn) {
        return;
    }
    if (reached(at)) {
        fn();  // опоздал — значит уже пора
        return;
    }
    waiters().push_back({at, fn});
}

void on_layer_begin(void (*fn)(const layer&)) {
    if (fn) {
        on_begin().push_back(fn);
    }
}

void on_layer_end(void (*fn)(const layer&)) {
    if (fn) {
        on_end().push_back(fn);
    }
}

namespace detail {

void reach(step at) {
    if (reached(at)) {
        return;
    }
    // Лестница монотонная: перескакивать ступени можно, возвращаться — нет.
    g_at = at;
    log(std::format("этап: {}", name_of(at)));
    // Копия списка: подписчик вправе подписаться на следующий этап прямо отсюда.
    const std::vector<waiter> now = waiters();
    for (const waiter& w : now) {
        if (static_cast<std::uint8_t>(w.at) <= static_cast<std::uint8_t>(at)) {
            w.fn();
        }
    }
    std::erase_if(waiters(), [at](const waiter& w) {
        return static_cast<std::uint8_t>(w.at) <= static_cast<std::uint8_t>(at);
    });
}

}  // namespace detail

void note_registration(void* context, const char* class_name) {
    if (!context) {
        return;
    }
    if (context != g_context) {
        close_layer();
        open_layer(context, class_name);
    } else if (class_name) {
        g_layer.last_class = class_name;
    }
}

void note_frame() {
    // Первый кадр закрывает последний слой: регистрации больше не будет, а ждущим её
    // подписчикам надо дать последний шанс до того, как пойдёт игра.
    close_layer();
    detail::reach(step::running);
}

void pump() {
    if (!reached(step::linked) && engine_answers()) {
        detail::reach(step::linked);
    }
}

}  // namespace graft::stage
