// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include "graft/callout.hpp"

// Вход в объектный граф игры и время жизни того, что оттуда достаётся.
//
// Тут всего три вопроса, и они важнее любого удобства:
//
//  1. ОТКУДА НАЧАТЬ. Корень игры — глобальная скриптовая переменная (`DayZGame g_Game`),
//     а не натив. Движок отдаёт такие через EnScript.GetClassVar с inst == null.
//
//  2. КТО ВЛАДЕЕТ. Объект, пришедший ОТ движка (игрок, сущность, идентити), — ЧУЖОЙ.
//     Мы его не считаем, движок может его удалить, и указатель на него живёт ровно до
//     конца текущего вызова. Держать такое между тиками нельзя; правильный приём —
//     каждый тик спрашивать заново, а запоминать ЗНАЧЕНИЕ (steam id, позицию), а не
//     ссылку. borrowed<T> ниже это проверяет, а не просит верить на слово.
//
//  3. ЧТО ДЕЛАТЬ СО СВОИМИ. Объект, созданный нами (буфер под out-массив), надо где-то
//     держать. Счётчиком ссылок движка мы не управляем, поэтому владения не изображаем:
//     scratch<T>() даёт ОДИН объект на тип на весь процесс. Это осознанная утечка с
//     известным потолком — по объекту на тип, — и она честнее, чем поддельный RAII.
namespace graft {

// ── Модель памяти скриптовых типов ───────────────────────────────────────────
// В игре её не одна, и разница между видами — не деталь, а то, можно ли вообще держать
// ссылку. Зеркало проставляет это каждому классу само (см. `graft apigen`), а мы
// пользуемся тем, что запреты стали ошибками КОМПИЛЯЦИИ, а не падениями в проде.
enum class lifetime {
    managed,  // наследник Managed: счётчик ссылок, ссылка продлевает жизнь
    plain,    // обычный класс: счётчик тоже есть, но с оговорками — держать осторожно
    entity,   // сущность мира: живёт до удаления из мира, ссылка жизнь НЕ продлевает
    engine,   // приватный деструктор: скриптом не удерживается вовсе, только заимствуется
};

// Класс мог и не сказать о себе ничего (свой тип мода, контейнер) — тогда считаем его
// обычным: это самое осторожное из предположений, оно ничего не разрешает сверх.
template <class T>
consteval lifetime lifetime_of() {
    if constexpr (requires { T::script_lifetime; }) {
        return T::script_lifetime;
    } else {
        return lifetime::plain;
    }
}

// Приватный конструктор в скрипте значит «этот тип создаёт только движок».
template <class T>
consteval bool spawnable() {
    if constexpr (requires { T::script_spawnable; }) {
        return T::script_spawnable;
    } else {
        return true;
    }
}

// Та же ссылка, но другим скриптовым классом. Внутри и там и там указатель, поэтому
// стоит это ничего; проверить родство можно через Class.IsInherited.
template <class To, class From>
To cast(From from) {
    To out{};
    out.ptr = from.ptr;
    return out;
}

namespace detail {
template <class T>
T make_ref(void* p) {
    T out{};
    out.ptr = p;
    return out;
}
}  // namespace detail

// ── Создание скриптовых объектов ─────────────────────────────────────────────
// Это движковый typename.Spawn — тот самый `new`, что и в скрипте. Работает для чего
// угодно, у чего есть скриптовый класс: array<Man>, map<string,int>, свой класс мода.
//
//   graft::array<graft::ref<"Man">> players = graft::spawn<...>();
template <class T>
std::expected<T, miss> try_spawn() {
    static_assert(spawnable<T>(),
                  "у этого скриптового класса приватный конструктор: его создаёт только "
                  "движок (CreateObject и подобные), а не typename.Spawn");
    void* class_desc = script::find_class(enf_type<T>::name);
    if (!class_desc) {
        return std::unexpected(miss::not_found);
    }
    // typename — это и есть дескриптор класса: Spawn зовётся НА НЁМ, как в скрипте
    // зовётся на значении типа. Приёмник у типа-значения едет первым в блоке.
    const std::expected<obj, miss> made = type{class_desc}.try_proto<obj, "Spawn">();
    if (!made) {
        return std::unexpected(made.error());
    }
    return detail::make_ref<T>(made->ptr);
}

template <class T>
T spawn() {
    const auto made = try_spawn<T>();
    return made ? *made : T{};
}

// Один объект на тип на весь процесс: буфер под out-аргументы движка.
//
// ponytail: намеренная утечка с известным потолком. Освобождать нечем — счётчик ссылок
// движка не наш, — а заводить объект на каждый вызов значит копить мусор без верхней
// границы. Путь наверх, если однажды понадобится: найти движковый Release и завести
// настоящее владение.
template <class T>
T scratch() {
    static_assert(lifetime_of<T>() != lifetime::entity,
                  "сущность мира нельзя держать весь процесс: она живёт до удаления из "
                  "мира, и ссылка этого не меняет. Спрашивай её заново каждый тик");
    // Первое обращение может случиться раньше, чем движок отдал контекст, — тогда
    // пробуем снова. Ноль здесь не кэшируем по той же причине, что и промах поиска.
    static T kept{};
    if (!kept) {
        kept = spawn<T>();
    }
    return kept;
}

// ── Переменная по имени, руками движка ───────────────────────────────────────
// EnScript.GetClassVar(inst, name, index, out result). Читает поле объекта, а при
// inst == null — глобальную переменную.
//
// Два факта, снятых на живом сервере и стоящих того, чтобы их записать:
//
//  1. КОД ВОЗВРАТА ВРЁТ. Документация обещает «returns true when success», а функция
//     отдаёт 0 и на успешном чтении — при этом значение в out-аргумент кладёт верное.
//     Судить об успехе можно только по записанному значению.
//  2. Глобальные переменные (`g_Game`) она не отдаёт: возвращает null. Это не наша
//     беда — из самого скрипта тот же вызов даёт тот же null. Поэтому корень игры
//     берётся другим путём, см. game().
//
// Полям объекта у нас есть путь короче — прямое чтение по раскладке (ref::field). Этот
// нужен там, где раскладка не подходит: чужая переменная неизвестного типа.
template <class T>
std::expected<T, miss> try_script_var(obj self, const char* name) {
    const script::method fn = detail::engine_method<"EnScript", "GetClassVar">();
    const value args[] = {value{self}, value{std::string{name ? name : ""}}, value{i32{0}},
                          value{T{}}};
    // Статический метод: объекта нет, а значит нет и rcx — только блок и возврат.
    const auto r = call_proto(fn, nullptr, args, script::tag_int);
    if (!r) {
        return std::unexpected(r.error());
    }
    const value out = r->arg(3);
    if (out.empty()) {
        return std::unexpected(miss::not_found);
    }
    if constexpr (script_class<T>) {
        if (!out.as<obj>().ptr) {
            return std::unexpected(miss::not_found);
        }
        return detail::make_ref<T>(out.as<obj>().ptr);
    } else {
        return out.as<T>();
    }
}

// Корень игры — то же, что GetGame() в скрипте.
//
// Приходит он вместе с тиком: `GraftTick(t, GetGame())`. Сам GetGame — обычная
// СКРИПТОВАЯ функция (`DayZGame GetGame() { return g_Game; }`), то есть байткод, а
// глобальную переменную движок отдавать отказывается (см. try_script_var выше). Строка
// в моде и так нужна ради тика, поэтому корень достаётся бесплатно.
//
// Пусто до первого тика — это не ошибка, а «мод ещё не проснулся».
inline ref<"DayZGame"> game() {
    return detail::make_ref<ref<"DayZGame">>(script::root());
}

inline std::expected<ref<"DayZGame">, miss> try_game() {
    const ref<"DayZGame"> found = game();
    if (!found) {
        return std::unexpected(miss::not_found);
    }
    return found;
}

// ── Чужой объект ─────────────────────────────────────────────────────────────
// Ссылка, полученная ОТ движка, действительна до конца текущего вызова. Между тиками
// движок волен её удалить, а память — переиспользовать под что угодно.
//
// Полной защиты тут быть не может: узнать, жив ли чужой объект, нельзя в принципе.
// Но самый частый исход переиспользования — другой класс по тому же адресу, и вот его
// поймать МОЖНО: запоминаем дескриптор класса и сверяем перед обращением.
//
//   graft::borrowed<graft::ref<"Man">> kept{player};
//   ...через тик...
//   if (auto p = kept.get()) { /* класс тот же — по крайней мере не чужой объект */ }
template <class T>
class borrowed {
public:
    borrowed() = default;
    explicit borrowed(T value) : ptr_(value.ptr), class_desc_(script::class_of(value.ptr)) {}

    // Пусто, если объекта не было или по этому адресу теперь объект другого класса.
    std::optional<T> get() const {
        if (!ptr_ || script::class_of(ptr_) != class_desc_) {
            return std::nullopt;
        }
        return detail::make_ref<T>(ptr_);
    }
    explicit operator bool() const { return get().has_value(); }
    void reset() { ptr_ = nullptr; class_desc_ = nullptr; }

private:
    void* ptr_ = nullptr;
    void* class_desc_ = nullptr;
};

}  // namespace graft
