#pragma once
#include <cstring>
#include <string>
#include <vector>

#include "graft/abi.h"
#include "graft/dispatch.hpp"

// Привязка: чем перечисляются нативы модуля, и макросы GRAFT_BINDINGS / GRAFT_PLUGIN.
namespace graft {

inline const native* natives() {
    return detail::head();
}

// Имя служебного натива, освобождающего состояние объекта. Суффикс — метка плагина
// (её ставит сборка: -DGRAFT_PLUGIN_TAG=имя), потому что двум плагинам, держащим
// состояние на ОДНОМ скриптовом классе, нужен каждому свой: имя метода в классе одно
// на всех, и второй просто затёр бы первого. Генератор зовёт из деструктора то имя,
// которое здесь получилось.
#ifdef GRAFT_PLUGIN_TAG
#define GRAFT_DISPOSE_NAME "NativeDispose_" GRAFT_PLUGIN_TAG
#else
#define GRAFT_DISPOSE_NAME "NativeDispose"
#endif

// Сколько экземпляров класса C сейчас живёт на стороне C++ (по одному на скриптовый
// объект, который хоть раз звал натив). Диагностика: если после смерти скриптовых
// объектов число не падает, значит их скриптовый деструктор не зовёт NativeDispose.
template <class C>
std::size_t live_instances() {
    return detail::instances<C>().size();
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
//       bind.global<&Ping>("SeraphGraftPing");
//       bind.class_<Table>("CppHashMap").method<&Table::Set>("Set").method<&Table::Count>("Count");
//   }
//
// Имя скриптового класса берётся из самого типа (script_object<"...">), объект в методы
// попадает сам, а поля живут столько же, сколько скриптовый объект.
template <name_t N>
using script_object = ref<N>;

// Объявление уже написано в скрипте руками — генератор такие записи не печатает. Нужно
// там, где он не выразит синтаксис: шаблонный класс мода, свои модификаторы.
inline constexpr struct declared_t {
} declared{};

class bindings {
public:
    explicit bindings(const char* script_module) : module_(script_module) {}

    // Методы одного скриптового класса.
    template <class C>
    class class_scope {
    public:
        // Имя может нести параметры шаблона: "CppHashMap<Class K, Class V>". Регистрация
        // идёт по имени до '<' (сам шаблонный класс), а объявление печатается целиком.
        class_scope(const char* class_name, const char* module, bool generate)
            : class_name_(class_name), module_(module), generate_(generate) {
            if (const char* angle = std::strchr(class_name, '<')) {
                declare_ = class_name;
                class_name_ = detail::intern(std::string(class_name, static_cast<std::size_t>(angle - class_name)));
            }
            // У класса с полями экземпляр живёт вместе со скриптовым объектом, значит
            // о смерти объекта надо узнавать — иначе движок переиспользует адрес и новый
            // объект получит чужие поля. Выбора тут нет, поэтому натив вешается сам;
            // скрипту остаётся позвать его из деструктора:
            //   private proto native void NativeDispose();
            //   void ~Класс() { NativeDispose(); }
            if constexpr (detail::has_fields<C>) {
                static bool registered = false;
                if (!registered) {
                    registered = true;
                    static constexpr const char* no_args[] = {nullptr};
                    put(GRAFT_DISPOSE_NAME,
                        reinterpret_cast<void*>(&detail::forget_native<C>), false, "void",
                        no_args);
                }
            }
        }

        // F — метод класса, константный метод или свободная функция, берущая объект
        // первым параметром. Объект в объявление скрипта не попадает.
        //
        // Если в сигнатуре есть graft::value, метод обслуживает ШАБЛОННЫЙ скриптовый
        // класс: одна привязка на общий класс работает для всех инстанциаций, а типы
        // приезжают в рантайме. Библиотека переключается на этот путь сама.
        template <auto F>
        class_scope& method(const char* name) {
            if constexpr (detail::marshalled_member<F>) {
                using T = detail::marshal_thunk<C, F>;
                return put(name, reinterpret_cast<void*>(&T::call), false, T::ret, T::args, true);
            } else {
                using T = detail::method_thunk<C, F>;
                return put(name, reinterpret_cast<void*>(&T::call), false, T::ret, T::args);
            }
        }
        // Статический метод: объекта нет, подойдёт любая свободная функция.
        template <auto F>
        class_scope& static_method(const char* name) {
            using T = detail::free_thunk<F>;
            return put(name, reinterpret_cast<void*>(&T::call), true, T::ret, T::args);
        }

    private:
        class_scope& put(const char* name, void* impl, bool is_static, const char* ret,
                         const char* const* args, bool marshalled = false) {
            detail::add({class_name_, name, impl, is_static, ret, args, nullptr, marshalled, module_,
                         generate_, declare_});
            return *this;
        }
        const char* class_name_;
        const char* module_;
        bool generate_;
        const char* declare_ = nullptr;
    };

    template <auto Fn>
    bindings& global(const char* name) {
        return put_global<Fn>(name, true);
    }
    template <auto Fn>
    bindings& global(const char* name, declared_t) {
        return put_global<Fn>(name, false);
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
                declare_ = class_name;
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
            // У шаблонного класса поля есть всегда (иначе он бы не был шаблонным по
            // хранилищу), поэтому освобождение вешаем сразу — как и обычному классу.
            using D = detail::dispose2<C, detail::script_param_types, detail::script_param_types>;
            static constexpr const char* no_args[] = {nullptr};
            detail::add({class_name_, GRAFT_DISPOSE_NAME, reinterpret_cast<void*>(&D::call), false, "void",
                         no_args, nullptr, true, module_, true, declare_});
        }

        template <class Pick>
        template_scope& method(Pick, const char* name) {
            using D = detail::dispatch2<C, Pick, detail::script_param_types,
                                        detail::script_param_types>;
            using MA = decltype(Pick{}.template operator()<C<i32, f32>>());
            using MB = decltype(Pick{}.template operator()<C<f32, i32>>());
            using Sig = detail::template_sig<MA, MB>;
            detail::add({class_name_, name, reinterpret_cast<void*>(&D::call), false,
                         Sig::ret(params_, param_count_), Sig::args(params_, param_count_),
                         nullptr, true, module_, true, declare_});
            return *this;
        }

    private:
        static constexpr std::size_t kMaxParams = 4;
        const char* class_name_;
        const char* module_;
        const char* declare_ = nullptr;
        std::string_view params_[kMaxParams]{};
        std::size_t param_count_ = 0;
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
    // ...или из самого типа, если он наследует script_object<"Имя">.
    template <class C>
    class_scope<C> class_() {
        return {C::script_class.value, module_, true};
    }
    template <class C>
    class_scope<C> class_(declared_t) {
        return {C::script_class.value, module_, false};
    }

private:
    template <auto Fn>
    bindings& put_global(const char* name, bool generate) {
        using T = detail::free_thunk<Fn>;
        detail::add({nullptr, name, reinterpret_cast<void*>(&T::call), false, T::ret, T::args,
                     nullptr, false, module_, generate});
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
}  // namespace detail

// Текст объявления для скрипта: одна строка / целый файл модуля.
// Формы с явным списком дескрипторов нужны генератору: он достаёт их из чужой DLL
// плагина, где своего реестра ему не видно.
std::string proto_decl(const native& n);
std::string proto_decl(const graft_native_desc& n);
std::string proto_file(const char* module = "1_Core");
std::string proto_file(const std::vector<const graft_native_desc*>& source, const char* module);
// Модули, в которых есть хоть одно объявление (для генератора).
std::vector<std::string> proto_modules();
std::vector<std::string> proto_modules(const std::vector<const graft_native_desc*>& source);

}  // namespace graft


#define GRAFT_CAT_(a, b) a##b
#define GRAFT_CAT(a, b) GRAFT_CAT_(a, b)

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
#define GRAFT_M(name) ([]<class T>() { return &T::name; })
#define GRAFT_METHOD(name) GRAFT_M(name), #name

// Паспорт плагина: имя, под которым он представляется хосту, и своя версия. Ровно один
// раз на плагин, в любом его файле. Без него не слинкуется — и это лучше, чем безымянный
// плагин в журнале, когда разбираешься, чей натив не завёлся.
//
//   GRAFT_PLUGIN("SIXW_GRAFT", 1);
#define GRAFT_PLUGIN(plugin_name, plugin_version)                     \
    extern "C" const char* const graft_plugin_name_ = plugin_name;    \
    extern "C" const unsigned graft_plugin_version_ = plugin_version

// Тик: место, где C++ просыпается сам. Тело — обычная функция от dt; внутри уже можно
// звать движок, потому что зовут нас со скриптового потока.
//
//   GRAFT_ON_TICK(dt) {
//       for (graft::dayz::Man man : players) { ... }
//   }
//
// Со стороны мода это одна строка (её печатает `graft protogen`):
//   modded class MissionServer { void OnUpdate(float t) { super.OnUpdate(t); GraftTick(t); } }
#define GRAFT_ON_TICK(dt) GRAFT_ON_TICK_(dt, __COUNTER__)
#define GRAFT_ON_TICK_(dt, id) GRAFT_ON_TICK__(dt, id)
#define GRAFT_ON_TICK__(dt, id)                                                    \
    static void GRAFT_CAT(graft_tick_, id)(float);                                 \
    static const ::graft::detail::tick_binder GRAFT_CAT(graft_tick_bind_, id){     \
        &GRAFT_CAT(graft_tick_, id)};                                              \
    static void GRAFT_CAT(graft_tick_, id)(float dt)

#define GRAFT_BINDINGS(script_module) GRAFT_BINDINGS_(script_module, __COUNTER__)
#define GRAFT_BINDINGS_(script_module, id) GRAFT_BINDINGS__(script_module, id)
#define GRAFT_BINDINGS__(script_module, id)                                    \
    static void GRAFT_CAT(graft_bind_, id)(::graft::bindings&);                \
    static const ::graft::detail::binder GRAFT_CAT(graft_binder_, id){         \
        script_module, &GRAFT_CAT(graft_bind_, id)};                           \
    static void GRAFT_CAT(graft_bind_, id)(::graft::bindings & bind)
