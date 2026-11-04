// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include <algorithm>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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

// База с полями: new_class<"Имя", База> (bindings.hpp). Наследник и база — ОДИН экземпляр
// на объект: методы базы, позванные на объекте наследника, видят его поля как свои. База
// без полей (движковый класс, голая обёртка) экземпляра не держит и в счёт не идёт.
template <class C>
struct base_of {
    using type = void;
};

template <class C>
    requires requires { typename C::graft_base; } && (!std::is_void_v<typename C::graft_base>) &&
             has_fields<typename C::graft_base>
struct base_of<C> {
    using type = typename C::graft_base;
};

// Верх иерархии: одна таблица экземпляров на всю иерархию, ключ — скриптовый объект.
template <class C>
struct root_of_t {
    using type = C;
};

template <class C>
    requires(!std::is_void_v<typename base_of<C>::type>)
struct root_of_t<C> {
    using type = typename root_of_t<typename base_of<C>::type>::type;
};
template <class C>
using root_of = typename root_of_t<C>::type;

// Тип экземпляра в рантайме. Экземпляр хранится указателем на корень иерархии: к любому
// классу цепочки от него ведёт static_cast.
struct kind {
    const char* script; // скриптовый класс; nullptr — тип о скрипте не знает
    const kind* base;   // ближайшая база с полями
    void* (*make)();    // новый экземпляр, указателем на корень
    void (*destroy)(void*);
    void (*forget)(void* self); // сбросить память о self у каждого класса цепочки
};

inline bool derives(const kind* k, const kind* from) {
    for (; k; k = k->base) {
        if (k == from) {
            return true;
        }
    }
    return false;
}

inline int depth(const kind* k) {
    int d = 0;
    while ((k = k->base)) {
        ++d;
    }
    return d;
}

// Последний найденный экземпляр. Поиск по таблице — хеш плюс сравнение ключа на каждый
// нативный вызов, а скриптовый цикл почти всегда дёргает один и тот же объект: тогда
// вместо поиска остаётся сравнение указателей. Сбрасывает его forget_instance.
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

template <class C>
void forget_memo(void* self) {
    if (last_instance<C>().self == self) {
        last_instance<C>() = {};
    }
    if constexpr (!std::is_void_v<typename base_of<C>::type>) {
        forget_memo<typename base_of<C>::type>(self);
    }
}

template <class C>
const kind& kind_of() {
    using B    = typename base_of<C>::type;
    using Root = root_of<C>;
    static const kind k{
        [] -> const char* {
            if constexpr (requires { C::script_class; }) {
                return C::script_class.value;
            } else {
                return nullptr;
            }
        }(),
        [] -> const kind* {
            if constexpr (std::is_void_v<B>) {
                return nullptr;
            } else {
                return &kind_of<B>();
            }
        }(),
        [] -> void* { return static_cast<Root*>(new C()); },
        [](void* p) { delete static_cast<C*>(static_cast<Root*>(p)); },
        &forget_memo<C>,
    };
    return k;
}

// Владеет экземпляром: стирание из таблицы разрушает его деструктором настоящего типа.
struct held {
    void*       object;
    const kind* type;

    held(void* o, const kind* t) : object(o), type(t) {}

    held(held&& other) noexcept : object(std::exchange(other.object, nullptr)), type(other.type) {}

    held& operator=(held&&) = delete;

    ~held() {
        if (object) {
            type->destroy(object);
        }
    }
};

// Адрес объекта, свёрнутый с собственными старшими битами. std::hash<void*> у MSVC — FNV
// по восьми байтам, а корзину таблица берёт по МЛАДШИМ битам: голый адрес при шаге
// объектов 128/256 байт сваливает их в одни корзины. Замер на 1000–100000 объектах (шаг
// 64/128/256, malloc, случайные размеры): поиск в 1.3–4.7 раза быстрее std::hash.
struct object_hash {
    std::size_t operator()(void* p) const noexcept {
        const auto x = reinterpret_cast<std::uintptr_t>(p);
        return (x >> 4) ^ (x >> 9) ^ (x >> 17);
    }
};

using instance_map = std::unordered_map<void*, held, object_hash>;

// Таблица намеренно не разрушается: движок может позвать натив (в том числе Dispose из
// деструктора скриптового объекта) уже после того, как побежали деструкторы статиков
// при выгрузке DLL. Работать с трупом контейнера хуже, чем не освободить его на выходе.
template <class Root>
instance_map& instance_table() {
    static auto* all = new instance_map;
    return *all;
}

template <class C>
instance_map& instances() {
    return instance_table<root_of<C>>();
}

// Привязанные классы иерархии — из них выбирается тип нового экземпляра. Заполняет
// bind.class_<C>().
template <class Root>
std::vector<const kind*>& kinds() {
    static auto* all = new std::vector<const kind*>;
    return *all;
}

template <class C>
void know_kind() {
    if constexpr (has_fields<C>) {
        auto& all = kinds<root_of<C>>();
        if (std::ranges::find(all, &kind_of<C>()) == all.end()) {
            all.push_back(&kind_of<C>());
        }
    }
}

// Родство по движку: скрипт может унаследовать привязанный класс сам
// (`class MyPuppy : MyDog`), и такого имени среди привязанных нет. Имя метода — на
// компиляции: поиск по строке на каждый вопрос стоил бы ~30 пустых вызовов (lookupRT).
inline bool script_inherits(void* self, const char* script) {
    void* const desc = script ? script::find_class(script) : nullptr;
    return desc && obj{self}.call<bool, "IsInherited">(type{desc});
}

// Каким типом заводить экземпляр объекта, впервые пришедшего как C: самым глубоким из
// привязанных классов, которым объект является. Пока наследников C не привязано, объект
// не читается вовсе; класс объекта среди привязанных — ответ по имени, без движка.
template <class C>
const kind* pick_kind(void* self) {
    const kind* const asked  = &kind_of<C>();
    const auto&       all    = kinds<root_of<C>>();
    const auto        deeper = [&](const kind* k) { return k != asked && derives(k, asked); };
    if (std::ranges::none_of(all, deeper)) {
        return asked;
    }
    const char* const name  = script::class_name(script::class_of(self));
    const auto        named = [&](const kind* k) { return k->script && name && std::strcmp(k->script, name) == 0; };
    if (named(asked)) {
        return asked;
    }
    for (const kind* k : all) {
        if (deeper(k) && named(k)) {
            return k;
        }
    }
    const kind* best = asked;
    for (const kind* k : all) {
        if (deeper(k) && depth(k) > depth(best) && script_inherits(self, k->script)) {
            best = k;
        }
    }
    return best;
}

// Скриптовый объект умер — умирает и его экземпляр C++ (с деструкторами полей). Зовёт
// это движок: библиотека подписывается на разрушение объекта в instance_of.
// ВНИМАНИЕ: сигнатура `void(void*)` — её адрес уходит в хост как callback.
template <class C>
void forget_instance(void* self) {
    if constexpr (has_fields<C>) {
        auto&      table = instances<C>();
        const auto it    = table.find(self);
        if (it == table.end()) {
            return;
        }
        it->second.type->forget(self); // память об этом объекте больше не годится
        table.erase(it);
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
            auto& table = instances<C>();
            auto  it    = table.find(self);
            if (it == table.end()) {
                const kind* type = pick_kind<C>(self);
                it               = table.emplace(self, held{type->make(), type}).first;
                // Завели поля на этот объект — просим сказать, когда он умрёт. Скрипт об
                // этом не знает и знать не должен: движок разрушает объект своим же
                // виртуальным слотом, туда мы и встраиваемся (script::watch_object).
                script::watch_object(self, &forget_instance<root_of<C>>);
            } else if (!derives(it->second.type, &kind_of<C>())) {
                // Иерархия C++ разошлась со скриптовой: объект заведён другой ветвью.
                const char* have = it->second.type->script;
                const char* want = kind_of<C>().script;
                throw std::logic_error(std::string{"экземпляр объекта заведён как "} + (have ? have : "?") +
                                       ", а не " + (want ? want : "?"));
            }
            m.object = static_cast<C*>(static_cast<root_of<C>*>(it->second.object));
            m.self   = self;
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

// Объект класса С ПОЛЯМИ аргументом натива: в скрипте это сам объект (`int LegsOf(MyAnimal
// animal)`), в C++ — его экземпляр, тот же, что видят его собственные методы:
//
//   int LegsOf(const Animal& animal);
//
// Ссылка только константная: неконстантная у значений означает out. NULL вместо объекта —
// исключение: guard пишет его в журнал, натив не исполняется, возврат по умолчанию.
template <class C>
    requires script_class<C> && has_fields<C>
struct arg_of<const C&> {
    using abi = ref<C::script_class>;

    static const C& in(abi object) {
        if (!object) {
            throw std::invalid_argument(std::string{"NULL вместо объекта "} + C::script_class.value);
        }
        return instance_of<C>(object.ptr);
    }
};

// Трамплин метода: движок кладёт объект в первый аргумент (это и есть this у ванильных
// нативов), а мы находим по нему экземпляр класса C++ и зовём обычную функцию. В
// объявление скрипта объект не идёт. Привязать можно что угодно: метод, константный
// метод или свободную функцию, берущую объект первым параметром (последнее — чтобы
// цеплять методами чужие классы, в которые метод не добавить). Все три зовутся одним
// std::invoke, разница — только в сигнатуре.
template <class... T>
struct type_list {
    static constexpr std::size_t size = sizeof...(T);
};

template <auto F>
struct method_sig;

template <class Own, class R, class... A, R (Own::*F)(A...)>
struct method_sig<F> {
    using ret  = R;
    using args = type_list<A...>;
};

template <class Own, class R, class... A, R (Own::*F)(A...) const>
struct method_sig<F> {
    using ret  = R;
    using args = type_list<A...>;
};

template <class R, class Self, class... A, R (*F)(Self, A...)>
struct method_sig<F> {
    using ret  = R;
    using args = type_list<A...>;
};

// Вход трамплина — тот, каким его зовёт движок. Флаги у наших членов те же, что у
// Object.GetPosition (0xa28, без external), и движок зовёт их как функцию-член C++.
// Пока возврат влезает в регистр, это не отличить от свободной функции: объект в rcx.
// Возврат больше 8 байт (vector) едет через буфер, и тогда он ВТОРЫМ, после объекта, —
// примешь его первым, и движок запишет результат поверх шапки скриптового объекта
// (так падал кейс Fields_VectorThroughPointer). Адрес буфера возвращается в rax, как у
// функции-члена.
template <class Thunk, class R, class... A>
struct member_entry_in_register : abi_check<R, A...> {
    static ret_abi<R> __fastcall call(void* self, arg_abi<A>... args) {
        return guarded<R>(reinterpret_cast<void*>(&call),
                          [&] { return Thunk::body(self, args...); });
    }
};

template <class Thunk, class R, class... A>
struct member_entry_in_buffer : abi_check<R, A...> {
    static ret_abi<R>* __fastcall call(void* self, ret_abi<R>* out, arg_abi<A>... args) {
        *out = guarded<R>(reinterpret_cast<void*>(&call), [&] { return Thunk::body(self, args...); });
        return out;
    }
};

template <class Thunk, class R, class... A>
using member_entry = std::conditional_t<script::returned_in_register<ret_abi<R>>,
                                        member_entry_in_register<Thunk, R, A...>,
                                        member_entry_in_buffer<Thunk, R, A...>>;

template <class C, auto F, class R, class Args>
struct method_body;

template <class C, auto F, class R, class... A>
struct method_body<C, F, R, type_list<A...>> : member_entry<method_body<C, F, R, type_list<A...>>, R, A...> {
    static ret_abi<R> body(void* self, arg_abi<A>... args) {
        [[maybe_unused]] arena_scope<R> alive;
        C& object = instance_of<C>(self);
        if constexpr (std::is_void_v<R>) {
            std::invoke(F, object, arg_of<A>::in(args)...);
        } else {
            return ret_of<R>::out(std::invoke(F, object, arg_of<A>::in(args)...));
        }
    }
};

template <class C, auto F>
struct method_thunk : method_body<C, F, typename method_sig<F>::ret, typename method_sig<F>::args> {};

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
