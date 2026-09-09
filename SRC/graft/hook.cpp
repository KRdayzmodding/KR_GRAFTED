// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include <safetyhook/inline_hook.hpp>

#include <cstddef>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "graft/engine.hpp"

// Единственная копия механики врезки в процессе. Своя TU, а не кусок engine.cpp, по двум
// причинам: её линкует не только hid.dll, но и сьюта (иначе graft::hook проверялся бы
// только выездом на стенд), и в ней нет ни одного обращения к состоянию библиотеки —
// только к системе.
//
// Плагин зовёт эти же функции, но через таблицу сервисов: у него своей копии нет и быть
// не должно, см. комментарий у graft::hook в engine.hpp.
//
// Под капотом safetyhook как есть, без единой правки: разбор пролога у него на Zydis,
// знающем набор команд целиком, включая VEX и EVEX. Своего здесь ровно одно — реестр
// занятых целей, которого у safetyhook нет.
//
// Как он патчит и почему это важно знать за пределами этого файла: safetyhook снимает
// права со страницы, пишет переход и своим векторным обработчиком ловит тех, кто по
// странице исполняется, отвечая им «продолжай». То есть врезка ШТАТНО порождает в
// процессе поток обращений к недоступной памяти. Что из этого следует для обработки
// сбоев — записано в guard.hpp, и это не теория: обработчик, считавший такие сбои
// аномалией, вешал сьюту наглухо.
namespace graft {
namespace {

// Реестр целей. У safetyhook его нет вовсе: вторую врезку в ту же функцию он примет и
// построит цепочку, а снятие в неверном порядке порвёт её молча — первый вернёт на место
// исходные байты поверх чужого перехода, и трамплин второго поведёт в никуда. Обещание
// «одна механика на процесс, повторная врезка — честный отказ» держит именно этот
// словарь, а не библиотека под ним.
std::mutex g_lock;
std::map<void*, safetyhook::InlineHook> g_hooks;

// Создать выключенным и сразу отдать адрес трамплина.
//
// Выключенным — потому что пачка обязана либо встать целиком, либо не встать вовсе, а
// создание, в отличие от включения, чужой пролог не трогает: откатывать после него
// нечего. Адрес сразу — потому что оригинал обязан быть достижим ДО того, как детур
// станет доступен чужому потоку, иначе первый же вызов уйдёт в ноль.
bool prepare(const hook_request& want, std::vector<safetyhook::InlineHook>& into) {
    auto made = safetyhook::InlineHook::create(want.target, want.detour,
                                               safetyhook::InlineHook::StartDisabled);
    if (!made) {
        return false;
    }
    *want.original = made->original<void*>();
    into.push_back(std::move(*made));
    return true;
}

}  // namespace

bool hook(void* target, void* detour, void** original) {
    if (target == nullptr || detour == nullptr || original == nullptr) {
        return false;
    }
    const std::scoped_lock held{g_lock};
    if (g_hooks.contains(target)) {
        return false;
    }
    std::vector<safetyhook::InlineHook> made;
    if (!prepare({target, detour, original}, made)) {
        return false;
    }
    // Врезка и включение — одно действие: созданный, но выключенный хук наружу выглядит
    // как успех, которого не случилось.
    if (!made.front().enable()) {
        *original = nullptr;
        return false;
    }
    g_hooks.emplace(target, std::move(made.front()));
    return true;
}

bool unhook(void* target) {
    if (target == nullptr) {
        return false;
    }
    const std::scoped_lock held{g_lock};
    const auto found = g_hooks.find(target);
    if (found == g_hooks.end()) {
        return false;
    }
    // Разрушение возвращает пролог на место и освобождает трамплин: иначе пролог остался
    // бы пропатченным до выгрузки процесса.
    g_hooks.erase(found);
    return true;
}

bool hook_all(std::span<const hook_request> all) {
    if (all.empty()) {
        return true;  // ничего не просили — ничего и не сломалось
    }
    const std::scoped_lock held{g_lock};
    for (std::size_t i = 0; i < all.size(); ++i) {
        const hook_request& want = all[i];
        if (want.target == nullptr || want.detour == nullptr || want.original == nullptr ||
            g_hooks.contains(want.target)) {
            return false;
        }
        // Повтор ВНУТРИ пачки словарь не поймает — цели в нём ещё нет. Перебором: заявок
        // в пачке единицы, и это дешевле любого множества.
        for (std::size_t j = 0; j < i; ++j) {
            if (all[j].target == want.target) {
                return false;
            }
        }
    }

    std::vector<safetyhook::InlineHook> made;
    made.reserve(all.size());
    const auto forget_originals = [&] {
        // Адреса трамплинов уже розданы, а трамплины сейчас исчезнут вместе с made.
        // Оставить их у вызывающего значит подложить ему указатель в освобождённую
        // память; ноль он хотя бы заметит на первом же вызове.
        for (const hook_request& want : all) {
            *want.original = nullptr;
        }
    };
    for (const hook_request& want : all) {
        if (!prepare(want, made)) {
            forget_originals();
            return false;  // не включено ещё ничего — откатывать нечего
        }
    }
    for (std::size_t i = 0; i < made.size(); ++i) {
        if (!made[i].enable()) {
            for (std::size_t j = 0; j < i; ++j) {
                (void)made[j].disable();
            }
            forget_originals();
            return false;
        }
    }

    for (std::size_t i = 0; i < all.size(); ++i) {
        g_hooks.emplace(all[i].target, std::move(made[i]));
    }
    return true;
}

}  // namespace graft
