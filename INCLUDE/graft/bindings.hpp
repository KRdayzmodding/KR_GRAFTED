// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "graft/abi.h"
#include "graft/dispatch.hpp"

// Привязка: чем перечисляются нативы модуля, и макросы GRAFT_BINDINGS / GRAFT_PLUGIN.
namespace graft {

inline const native* natives() {
    return detail::head();
}

// Сколько экземпляров класса C (с наследниками) сейчас живёт на стороне C++ — по одному на
// скриптовый объект, который хоть раз звал натив. Диагностика: если после смерти
// скриптовых объектов число не падает, значит до нас не дошло их разрушение.
template <class C>
std::size_t live_instances() {
    return static_cast<std::size_t>(std::ranges::count_if(detail::instances<C>(), [](const auto& entry) {
        return detail::derives(entry.second.type, &detail::kind_of<C>());
    }));
}

// ── Привязка ─────────────────────────────────────────────────────────────────
// Скриптовый класс отражается классом C++, а все нативы модуля перечисляются одним
// блоком — вместо макроса после каждой функции:
//
//   struct Table : graft::script_object<"CppHashMap"> {
//       std::unordered_map<std::string, std::string> data;   // обычное поле
//
//       void Set(graft::str k, graft::str v) { data.insert_or_assign(...); }
//       graft::i32 Count() const { return static_cast<graft::i32>(data.size()); }
//   };
//
//   GRAFT_BINDINGS("1_Core") {
//       bind.global<&Ping>();
//       bind.class_<Table>().method<&Table::Set>().method<&Table::Count>();
//   }
//
// Имя скриптового класса берётся из самого типа (script_object<"...">), имя метода — из
// имени функции C++, объект в методы попадает сам, а поля живут столько же, сколько
// скриптовый объект. Класс script_object уже есть в движке или в скрипте мода: генератор
// дописывает к нему методы через `modded class`.
template <name_t N>
using script_object = ref<N>;

// Новый класс — его вводит сам мод, в движке его нет. Объявление печатается целиком:
// `class Имя` или `class Имя : База`.
//
//   struct Animal : graft::new_class<"MyAnimal"> {
//       int legs = 4;
//       int Legs() const { return legs; }
//   };
//   struct Dog : graft::new_class<"MyDog", Animal> {   // class MyDog : MyAnimal
//       int barks = 0;
//       int Bark() { return ++barks; }
//   };
//
// Наследование настоящее: у Dog свои поля поверх полей Animal, а объект — один. Метод
// Animal, позванный на объекте MyDog (или скриптового наследника MyDog), видит тот же
// экземпляр Dog. База — другой new_class или движковый класс (graft::ref<"Managed">): у
// такой полей нет, она только печатается в заголовке.
template <name_t N, class Base = void>
struct new_class : Base {
    static_assert(requires { Base::script_class; }, "база new_class — скриптовый класс: new_class или ref<\"Имя\">");
    static constexpr name_t script_class = N;
    using graft_base                     = Base;
};

template <name_t N>
struct new_class<N, void> : ref<N> {
    using graft_base = void;
};

// Описание метода для генератора: имя в скрипте (по умолчанию — имя функции C++), имена
// аргументов через запятую и описание. Имена аргументов из C++ не достать — язык их не
// отдаёт, поэтому они строкой.
//
//   .method<&Animal::Feed>({.args = "grams", .doc = "Покормить животное."})
//   .method<&Dog::Bark>({.name = "Woof"})
struct decl {
    const char* name = nullptr;
    const char* args = nullptr;
    const char* doc  = nullptr;
};

namespace detail {

// Имя функции C++ без класса и параметров шаблона: &Dog::Bark -> "Bark",
// &Dog::Fetch<int> -> "Fetch". Из сигнатуры самого шаблона — другого способа язык не даёт.
template <auto F>
consteval std::string_view signature() {
#if defined(__clang__) || defined(__GNUC__)
    return __PRETTY_FUNCTION__; // "... signature() [F = &Dog::Bark]"
#else
    return __FUNCSIG__; // "... signature<&Dog::Bark>(void)"
#endif
}

consteval std::string_view short_name(std::string_view s) {
#if defined(__clang__) || defined(__GNUC__)
    s = s.substr(s.rfind("F = ") + 4);
    s = s.substr(0, s.rfind(']'));
#else
    s = s.substr(s.find("signature<") + 10);
    s = s.substr(0, s.rfind(">(void)"));
#endif
    if (s.ends_with('>')) { // параметры шаблона функции
        int depth = 0;
        for (std::size_t i = s.size(); i-- > 0;) {
            if (s[i] == '>') {
                ++depth;
            } else if (s[i] == '<') {
                --depth;
            }
            if (depth == 0) {
                s = s.substr(0, i);
                break;
            }
        }
    }
    const std::size_t colon = s.rfind("::");
    s                       = colon == std::string_view::npos ? s : s.substr(colon + 2);
    return s.starts_with('&') ? s.substr(1) : s;
}

template <auto F>
inline constexpr auto function_name = [] {
    constexpr std::string_view        name = short_name(signature<F>());
    std::array<char, name.size() + 1> out{};
    std::ranges::copy(name, out.begin());
    return out;
}();

// Заголовок нового класса: "Имя" или "Имя : База".
template <class C>
inline constexpr auto header = [] {
    if constexpr (std::is_void_v<typename C::graft_base>) {
        return C::script_class;
    } else {
        return C::script_class + name_t{" : "} + C::graft_base::script_class;
    }
}();

} // namespace detail

// Объявление уже написано в скрипте руками — генератор такие записи не печатает. Нужно
// там, где он не выразит синтаксис: шаблонный класс мода, свои модификаторы.
inline constexpr struct declared_t {
} declared{};

// Тег для класса C++, который о скриптах не знает: класса в движке НЕТ, его вводит сам
// мод, и объявление печатается целиком (`class Имя`), а не правкой существующего
// (`modded class Имя`). Своему типу удобнее new_class — там же задаётся и база.
inline constexpr struct fresh_t {
} fresh{};

class bindings {
public:
    explicit bindings(const char* script_module) : module_(script_module) {}

    // Методы одного скриптового класса.
    template <class C>
    class class_scope {
    public:
        // Имя может нести параметры шаблона: "CppHashMap<Class K, Class V>". Регистрация
        // идёт по имени до '<' (сам шаблонный класс), а объявление печатается целиком.
        // declare — заголовок нового класса (`class ...`); nullptr — класс движковый.
        class_scope(const char* class_name, const char* module, bool generate, const char* declare = nullptr)
            : class_name_(class_name), module_(module), generate_(generate), declare_(declare) {
            if (const char* angle = std::strchr(class_name, '<')) {
                declare_    = class_name;
                class_name_ = detail::intern(std::string(class_name, static_cast<std::size_t>(angle - class_name)));
            }
            detail::know_kind<C>();
        }

        // F — метод класса, константный метод или свободная функция, берущая объект
        // первым параметром. Объект в объявление скрипта не попадает.
        //
        // Если в сигнатуре есть graft::value или out у значения (`f32& ratio`), метод
        // зовётся маршалируемым путём (`proto`) — библиотека переключается на него сама.
        // graft::value — это ШАБЛОННЫЙ скриптовый класс: одна привязка на общий класс
        // работает для всех инстанциаций, а типы приезжают в рантайме.
        template <auto F>
        class_scope& method(decl d = {}) {
            constexpr bool marshalled = detail::marshalled_member<F>;
            using T                   = std::conditional_t<marshalled, detail::marshal_thunk<C, F>, detail::method_thunk<C, F>>;
            return put<F, T>(d, false, marshalled);
        }

        template <auto F>
        class_scope& method(const char* name, const char* params = nullptr, const char* doc = nullptr) {
            return method<F>({name, params, doc});
        }

        // Статический метод: объекта нет, подойдёт любая свободная функция.
        // graft::value в сигнатуре переключает на маршалируемый путь — у статического
        // вызова приёмника нет, форма та же, что у глобального.
        template <auto F>
        class_scope& static_method(decl d = {}) {
            constexpr bool marshalled = detail::marshalled_free<F>;
            using T                   = std::conditional_t<marshalled, detail::marshal_free_thunk<F>, detail::free_thunk<F>>;
            return put<F, T>(d, true, marshalled);
        }

        template <auto F>
        class_scope& static_method(const char* name, const char* params = nullptr, const char* doc = nullptr) {
            return static_method<F>({name, params, doc});
        }

    private:
        template <auto F, class T>
        class_scope& put(const decl& d, bool is_static, bool marshalled) {
            const char* name = d.name ? d.name : detail::function_name<F>.data();
            detail::add({class_name_, name, reinterpret_cast<void*>(&T::call), is_static, T::ret, T::args, nullptr, d.args, d.doc, marshalled, module_, generate_, declare_});
            return *this;
        }

        const char* class_name_;
        const char* module_;
        bool        generate_;
        const char* declare_ = nullptr;
    };

    // Имя в скрипте — имя функции C++, если не задано иначе; args — имена аргументов через
    // запятую, doc — описание. Всё это попадает в сгенерированное объявление: пользователь
    // читает не типы, а имена и назначение.
    //
    //   bind.global<&KRT_Attach>({.args = "player, player_uid, char_uid",
    //                             .doc  = "Подключить игрока: слот актора и конверт."});
    template <auto Fn>
    bindings& global(decl d = {}) {
        return put_global<Fn>(d, true);
    }

    template <auto Fn>
    bindings& global(const char* name, const char* params = nullptr, const char* doc = nullptr) {
        return put_global<Fn>({name, params, doc}, true);
    }

    template <auto Fn>
    bindings& global(const char* name, declared_t) {
        return put_global<Fn>({name}, false);
    }

    // Настоящий шаблон C++: методы пишутся обычными типами, а библиотека разворачивает
    // класс по всем поддерживаемым типам и выбирает нужную инстанциацию по имени
    // скриптового класса объекта.
    //
    //   bind.template_class<Table>("CppHashMap<Class K, Class V>")
    //       .method<GRAFT_M(Set)>("Set");
    template <template <class...> class C>
    class template_scope {
    public:
        // Имя несёт объявление целиком: "CppHashMap<Class K, Class V>". Отсюда и имя
        // для регистрации (до '<'), и имена параметров для объявлений методов.
        template_scope(const char* class_name, const char* module) : class_name_(class_name), module_(module) {
            if (const char* angle = std::strchr(class_name, '<')) {
                declare_    = class_name;
                class_name_ = detail::intern(std::string(class_name, static_cast<std::size_t>(angle - class_name)));
                detail::split_params(class_name, params_, kMaxParams, param_count_);
                for (std::size_t i = 0; i < param_count_; ++i) {
                    // "Class K" -> "K"
                    const auto space = params_[i].rfind(' ');
                    if (space != params_[i].npos) {
                        params_[i] = params_[i].substr(space + 1);
                    }
                }
            }
        }

        template <class Pick>
        template_scope& method(Pick, const char* name) {
            using D   = detail::dispatch2<C, Pick, detail::script_param_types, detail::script_param_types>;
            using MA  = decltype(Pick{}.template operator()<C<i32, f32>>());
            using MB  = decltype(Pick{}.template operator()<C<f32, i32>>());
            using Sig = detail::template_sig<MA, MB>;
            // Порядок полей — как в graft::native (registry.hpp): next, имена аргументов,
            // описание, marshalled, модуль, печатать ли, заголовок класса. У шаблонного
            // метода имён и описания нет: генератор печатает их по параметрам класса.
            detail::add({class_name_, name, reinterpret_cast<void*>(&D::call), false, Sig::ret(params_, param_count_), Sig::args(params_, param_count_), nullptr, nullptr, nullptr, true, module_, true, declare_});
            return *this;
        }

    private:
        static constexpr std::size_t kMaxParams = 4;
        const char*                  class_name_;
        const char*                  module_;
        const char*                  declare_ = nullptr;
        std::string_view             params_[kMaxParams]{};
        std::size_t                  param_count_ = 0;
    };

    template <template <class...> class C>
    template_scope<C> template_class(const char* name) {
        return {name, module_};
    }

    // Имя скриптового класса — своё, если класс C++ ничего не знает про скрипты...
    template <class C>
    class_scope<C> class_(const char* name) {
        return {name, module_, true};
    }

    template <class C>
    class_scope<C> class_(const char* name, declared_t) {
        return {name, module_, false};
    }

    // Класс, которого в движке нет: объявляется целиком, а не правкой существующего.
    template <class C>
    class_scope<C> class_(const char* name, fresh_t) {
        return {name, module_, true, name};
    }

    // ...или из самого типа: script_object<"Имя"> — дописать движковый класс,
    // new_class<"Имя", База> — объявить новый целиком.
    template <class C>
    class_scope<C> class_() {
        if constexpr (requires { typename C::graft_base; }) {
            return {C::script_class.value, module_, true, detail::header<C>.value};
        } else {
            return {C::script_class.value, module_, true};
        }
    }

    template <class C>
    class_scope<C> class_(declared_t) {
        return {C::script_class.value, module_, false};
    }

private:
    // graft::value в сигнатуре глобального натива делает его маршалируемым: в
    // объявлении такой аргумент печатается как `void` — «любой», ровно как у
    // ванильных proto void Print(void var) и Serializer.Write(void).
    template <auto Fn>
    bindings& put_global(const decl& d, bool generate) {
        constexpr bool marshalled = detail::marshalled_free<Fn>;
        using T                   = std::conditional_t<marshalled, detail::marshal_free_thunk<Fn>, detail::free_thunk<Fn>>;
        const char* name          = d.name ? d.name : detail::function_name<Fn>.data();
        detail::add({nullptr, name, reinterpret_cast<void*>(&T::call), false, T::ret, T::args, nullptr, d.args, d.doc, marshalled, module_, generate});
        return *this;
    }

    const char* module_;
};

namespace detail {
struct binder {
    binder(const char* script_module, void (*fn)(bindings&)) {
        bindings block{script_module};
        fn(block);
    }
};
} // namespace detail

// Текст объявления для скрипта: одна строка / целый файл модуля.
// Формы с явным списком дескрипторов нужны генератору: он достаёт их из чужой DLL
// плагина, где своего реестра ему не видно.
std::string proto_decl(const native& n);
std::string proto_decl(const graft_native_desc& n);
std::string proto_file(const char* module = "1_Core");
// plugin — имя плагина: файл целиком оборачивается в `#ifdef GRAFTED_<ИМЯ>` (у хоста,
// "graft", — в `#ifdef GRAFTED`). Пусто или module == "1_Core" — без обёртки: этот модуль
// движок компилирует без дефайнов.
std::string proto_file(const std::vector<const graft_native_desc*>& source, const char* module, std::string_view plugin = {});
// Модули, в которых есть хоть одно объявление (для генератора).
std::vector<std::string> proto_modules();
std::vector<std::string> proto_modules(const std::vector<const graft_native_desc*>& source);

} // namespace graft

#define GRAFT_CAT_(a, b) a##b
#define GRAFT_CAT(a, b)  GRAFT_CAT_(a, b)

// Блок привязки: все нативы одного модуля описываются в одном месте. Модуль — тот, в
// который генератор напечатает объявления: игровые типы (Object, EntityAI) в 1_Core ещё
// не существуют, для них нужен "3_Game". Блоков в файле может быть несколько.
//
//   GRAFT_BINDINGS("1_Core") {
//       bind.global<&Ping>("SeraphGraftPing");
//       bind.class_<Node>().method<&Node::Id>("Id");
//   }
// Выбор метода шаблонного класса: имя метода одно и то же у всех инстанциаций, но
// указатель на член у каждой свой — поэтому передаём «как его взять», а не сам указатель.
//   bind.template_class<Table>("CppHashMap<Class K, Class V>").method(GRAFT_METHOD(Set));
#define GRAFT_M(name)      ([]<class T>() { return &T::name; })
#define GRAFT_METHOD(name) GRAFT_M(name), #name

// Паспорт плагина: имя, под которым он представляется хосту, и своя версия. Ровно один
// раз на плагин, в любом его файле. Без него не слинкуется — и это лучше, чем безымянный
// плагин в журнале, когда разбираешься, чей натив не завёлся.
//
//   GRAFT_PLUGIN("SIXW_GRAFT", 1);
#define GRAFT_PLUGIN(plugin_name, plugin_version)                     \
    extern "C" const char* const graft_plugin_name_    = plugin_name; \
    extern "C" const unsigned    graft_plugin_version_ = plugin_version

// Тик: место, где C++ просыпается сам. Тело — обычная функция от dt; внутри уже можно
// звать движок, потому что зовут нас со скриптового потока.
//
//   GRAFT_ON_TICK(dt) {
//       for (graft::dayz::Man man : players) { ... }
//   }
//
// Со стороны мода это одна строка (её печатает `graft protogen`):
//   modded class MissionServer { void OnUpdate(float t) { super.OnUpdate(t); GraftTick(t); } }
#define GRAFT_ON_TICK(dt)      GRAFT_ON_TICK_(dt, __COUNTER__)
#define GRAFT_ON_TICK_(dt, id) GRAFT_ON_TICK__(dt, id)
#define GRAFT_ON_TICK__(dt, id)                                                  \
    static void                               GRAFT_CAT(graft_tick_, id)(float); \
    static const ::graft::detail::tick_binder GRAFT_CAT(graft_tick_bind_, id){   \
        &GRAFT_CAT(graft_tick_, id)};                                            \
    static void GRAFT_CAT(graft_tick_, id)(float dt)

// Загрузка: место, где плагин может ставить врезки в движок (graft::hook). Зовётся из
// graft_plugin_entry, когда хост уже представился, а движок ещё не создал потоков.
//
//   GRAFT_ON_LOAD() { graft::hook(&Engine_Foo, &my_foo, &g_foo); }
#define GRAFT_ON_LOAD()    GRAFT_ON_LOAD_(__COUNTER__)
#define GRAFT_ON_LOAD_(id) GRAFT_ON_LOAD__(id)
#define GRAFT_ON_LOAD__(id)                                                    \
    static void                               GRAFT_CAT(graft_load_, id)();    \
    static const ::graft::detail::load_binder GRAFT_CAT(graft_load_bind_, id){ \
        &GRAFT_CAT(graft_load_, id)};                                          \
    static void GRAFT_CAT(graft_load_, id)()

#define GRAFT_BINDINGS(script_module)      GRAFT_BINDINGS_(script_module, __COUNTER__)
#define GRAFT_BINDINGS_(script_module, id) GRAFT_BINDINGS__(script_module, id)
#define GRAFT_BINDINGS__(script_module, id)                                              \
    static void                          GRAFT_CAT(graft_bind_, id)(::graft::bindings&); \
    static const ::graft::detail::binder GRAFT_CAT(graft_binder_, id){                   \
        script_module, &GRAFT_CAT(graft_bind_, id)};                                     \
    static void GRAFT_CAT(graft_bind_, id)(::graft::bindings & bind)
