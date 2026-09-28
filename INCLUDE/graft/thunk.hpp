// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>

#include "graft/guard.hpp"
#include "graft/script.hpp"

// Трамплины немаршалируемого пути (`proto native`): движок зовёт их напрямую, обычным
// x64 fastcall. Здесь же — экземпляр класса C++ на скриптовый объект: поля живут ровно
// столько, сколько живёт объект в скрипте.
namespace graft::detail {

// Трамплин свободной функции: тот же перевод, что и у методов.
template <auto F>
struct free_thunk;
template <class R, class... A, R (*F)(A...)>
struct free_thunk<F> : abi_check<R, A...> {
    static ret_abi<R> __fastcall call(arg_abi<A>... args) {
        // Тело — отдельной функцией: у x64-SEH таблица областей описывает диапазоны
        // адресов, и защита работает, только если сбой случился ВНУТРИ вызова.
        return guarded<R>(reinterpret_cast<void*>(&call), [&] { return body(args...); });
    }
    static ret_abi<R> body(arg_abi<A>... args) {
        [[maybe_unused]] arena_scope<R> alive;
        if constexpr (std::is_void_v<R>) {
            F(arg_of<A>::in(args)...);
        } else {
            return ret_of<R>::out(F(arg_of<A>::in(args)...));
        }
    }
};

// Имена, собранные на лету (имя шаблонного класса без параметров), обязаны пережить
// блок привязки: указатель на них уходит в реестр.
inline const char* intern(std::string text) {
    static std::deque<std::string> kept;
    return kept.emplace_back(std::move(text)).c_str();
}

// Хранилище для записей блока привязки: адреса должны быть стабильны, поэтому deque.
inline std::deque<native>& storage() {
    static std::deque<native> all;
    return all;
}
inline void add(native desc) {
    storage().push_back(desc);
    native& stored = storage().back();
    stored.next = head();
    head() = &stored;
}

// ── Экземпляр класса C++ на скриптовый объект ────────────────────────────────
// Класс без своих полей — просто обёртка вокруг указателя: его дешевле собрать на стеке
// в каждом вызове. Класс с полями обязан пережить вызов, поэтому библиотека держит по
// одному экземпляру на скриптовый объект и отдаёт трамплину именно его. Для пишущего
// код это незаметно: поля ведут себя как поля.
//
// Наследоваться от чего-либо привязываемому классу не нужно: он может ничего не знать
// про скрипты. Наследник ref<"Имя"> дополнительно получает указатель на скриптовый
// объект, а с ним field()/call().
template <class C>
inline constexpr bool has_fields =
    !std::is_empty_v<C> && (sizeof(C) != sizeof(void*) || !std::is_trivially_copyable_v<C> ||
                            !std::is_trivially_destructible_v<C>);

// Таблица намеренно не разрушается: движок может позвать натив (в том числе Dispose из
// деструктора скриптового объекта) уже после того, как побежали деструкторы статиков
// при выгрузке DLL. Работать с трупом контейнера хуже, чем не освободить его на выходе.
template <class C>
std::unordered_map<void*, C>& instances() {
    static auto* all = new std::unordered_map<void*, C>;
    return *all;
}

// Последний найденный экземпляр. Поиск по таблице — хеш плюс сравнение ключа на каждый
// нативный вызов, а скриптовый цикл почти всегда дёргает один и тот же объект: тогда
// вместо поиска остаётся сравнение указателей. Ссылки на элементы unordered_map при
// вставках не портятся, поэтому держать указатель можно; сбрасывает его forget_instance.
// ponytail: как и сама таблица, без синхронизации — нативы зовутся из скриптового потока.
template <class C>
struct memo {
    void* self = nullptr;
    C* object = nullptr;
};

template <class C>
memo<C>& last_instance() {
    static memo<C> m;
    return m;
}


// Скриптовый объект умер — умирает и его экземпляр C++ (с деструкторами полей). Зовёт
// это движок: библиотека подписывается на разрушение объекта в instance_of.
// ВНИМАНИЕ: сигнатура `void(void*)` — её адрес уходит в хост как callback.
template <class C>
void forget_instance(void* self) {
    if constexpr (has_fields<C>) {
        last_instance<C>() = {};  // память об этом объекте больше не годится
        instances<C>().erase(self);
    }
}

template <class C>
C& instance_of(void* self) {
    const auto bind_self = [self](C& object) {
        if constexpr (requires { object.ptr = self; }) {
            object.ptr = self;  // наследник ref<...>: пусть знает свой скриптовый объект
        }
    };
    if constexpr (has_fields<C>) {
        memo<C>& m = last_instance<C>();
        if (m.self != self || !m.object) {
            const bool fresh = !instances<C>().contains(self);
            m.object = &instances<C>()[self];
            m.self = self;
            if (fresh) {
                // Завели поля на этот объект — просим сказать, когда он умрёт. Скрипт об
                // этом не знает и знать не должен: движок разрушает объект своим же
                // виртуальным слотом, туда мы и встраиваемся (script::watch_object).
                script::watch_object(self, &forget_instance<C>);
            }
        }
        bind_self(*m.object);
        return *m.object;
    } else {
        // Обёртка без полей: живёт ровно на время вызова, таблица не нужна.
        thread_local C object;
        bind_self(object);
        return object;
    }
}

// Трамплин метода: движок кладёт объект в первый аргумент (это и есть this у ванильных
// нативов), а мы находим по нему экземпляр класса C++ и зовём обычную функцию. В
// объявление скрипта объект не идёт. Привязать можно что угодно: метод, константный
// метод или свободную функцию, берущую объект первым параметром (последнее — чтобы
// цеплять методами чужие классы, в которые метод не добавить).
template <class C, auto F>
struct method_thunk;

template <class C, class Own, class R, class... A, R (Own::*F)(A...)>
struct method_thunk<C, F> : abi_check<R, A...> {
    static ret_abi<R> __fastcall call(void* self, arg_abi<A>... args) {
        return guarded<R>(reinterpret_cast<void*>(&call), [&] { return body(self, args...); });
    }
    static ret_abi<R> body(void* self, arg_abi<A>... args) {
        [[maybe_unused]] arena_scope<R> alive;
        C& object = instance_of<C>(self);
        if constexpr (std::is_void_v<R>) {
            (object.*F)(arg_of<A>::in(args)...);
        } else {
            return ret_of<R>::out((object.*F)(arg_of<A>::in(args)...));
        }
    }
};

template <class C, class Own, class R, class... A, R (Own::*F)(A...) const>
struct method_thunk<C, F> : abi_check<R, A...> {
    static ret_abi<R> __fastcall call(void* self, arg_abi<A>... args) {
        return guarded<R>(reinterpret_cast<void*>(&call), [&] { return body(self, args...); });
    }
    static ret_abi<R> body(void* self, arg_abi<A>... args) {
        [[maybe_unused]] arena_scope<R> alive;
        const C& object = instance_of<C>(self);
        if constexpr (std::is_void_v<R>) {
            (object.*F)(arg_of<A>::in(args)...);
        } else {
            return ret_of<R>::out((object.*F)(arg_of<A>::in(args)...));
        }
    }
};

template <class C, class R, class Self, class... A, R (*F)(Self, A...)>
struct method_thunk<C, F> : abi_check<R, A...> {
    static ret_abi<R> __fastcall call(void* self, arg_abi<A>... args) {
        return guarded<R>(reinterpret_cast<void*>(&call), [&] { return body(self, args...); });
    }
    static ret_abi<R> body(void* self, arg_abi<A>... args) {
        [[maybe_unused]] arena_scope<R> alive;
        C& object = instance_of<C>(self);
        if constexpr (std::is_void_v<R>) {
            F(object, arg_of<A>::in(args)...);
        } else {
            return ret_of<R>::out(F(object, arg_of<A>::in(args)...));
        }
    }
};

// ── Детур под защитой ────────────────────────────────────────────────────────
// Натив плагина обёрнут защитой всегда — трамплины выше это и делают. С детуром так не
// было: graft::hook принимает голый указатель, и детур исполняется внутри движкового кода
// без всякой обёртки. Асимметрия неочевидная и дорогая — код тот же самый, плагинный, а
// последствия у падения разные: в нативе вызов отменяется, в детуре умирает сервер.
//
// Детур приходит ШАБЛОННЫМ параметром, а не значением: обёртка обязана быть отдельной
// функцией с ровно той же сигнатурой, а сгенерировать такую можно, только зная детур на
// этапе компиляции.
//
// В отчёте такой сбой будет без имени: реестр ищет владельца по адресу трамплина натива,
// а детур в нём не значится. Зато он посчитан, записан адресом и не унёс сервер.
template <auto Detour>
struct guarded_detour;

template <class R, class... A, R (*Detour)(A...)>
struct guarded_detour<Detour> {
    static_assert(std::is_same_v<ret_abi<R>, R>,
                  "детур обязан возвращать ровно то, что движок ждёт в rax");

    static R call(A... args) {
        return guarded<R>(reinterpret_cast<void*>(Detour), [&] { return Detour(args...); });
    }
};

}  // namespace graft::detail

namespace graft {

// Врезка с защищённым детуром: падение внутри него отменяет ВЫЗОВ, а не игру.
//
//   graft::hook<&my_foo>(&Engine_Foo, &g_orig);      // детур защищён
//   graft::hook(&Engine_Foo, &my_foo, &g_orig);      // как было, под свою ответственность
//
// Голая форма оставлена намеренно: детур, который обязан досылать управление оригиналу
// при любом исходе, раскрутку пережить не может, и решать это вправе только его автор.
template <auto Detour, class F>
    requires std::is_pointer_v<F>
bool hook(F target, F* original) {
    static_assert(std::is_same_v<F, decltype(&detail::guarded_detour<Detour>::call)>,
                  "типы цели и детура обязаны совпадать — иначе кадр вызова разъедется");
    return hook(reinterpret_cast<void*>(target),
                reinterpret_cast<void*>(&detail::guarded_detour<Detour>::call),
                reinterpret_cast<void**>(original));
}

// Она же для пачки.
template <auto Detour, class F>
    requires std::is_pointer_v<F>
hook_request hooked(F target, F* original) {
    static_assert(std::is_same_v<F, decltype(&detail::guarded_detour<Detour>::call)>,
                  "типы цели и детура обязаны совпадать — иначе кадр вызова разъедется");
    return {reinterpret_cast<void*>(target),
            reinterpret_cast<void*>(&detail::guarded_detour<Detour>::call),
            reinterpret_cast<void**>(original)};
}

} // namespace graft
