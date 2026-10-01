// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <type_traits>

#include "graft/marshal.hpp"

// Вызов В ДРУГУЮ СТОРОНУ: как graft зовёт маршалируемые `proto` самого движка.
//
// Зачем вообще. Прямой `proto native` мы зовём давно — это обычный fastcall по impl из
// дескриптора (ref::call). Но «второй этаж» скриптового API маршалируемый, а не
// нативный: typename.Spawn (создать любой скриптовый объект), EnScript.GetClassVar
// (глобальная переменная, то есть корень объектного графа), ScriptModule.CallFunction
// (позвать скриптовую функцию), PlayerIdentity.GetPlainId. Один примитив открывает всё.
//
// Как. Движок скриптовую переменную НЕ конструирует: подготовка кадра (sub_1403663B0,
// разбор в re/README.md) копирует 40-байтный шаблон из дескриптора вызываемой функции.
// Мы делаем ровно то же — и потому не гадаем, что лежит в полях, которых не разобрали:
// они доезжают до движка байт в байт такими, какими он их сам туда положил.
//
// Шаблона нет у переменной ВОЗВРАТА (дескриптор её не носит), у параметра `void` и у
// глобального `proto` без дескриптора — там переменную собираем по тегу так же, как её
// размечает компилятор (script::var::of_tag). Если однажды окажется, что этого мало,
// чинить надо там и только там.
namespace graft {

// Результат маршалируемого вызова. Аргументы отдаются ПОСЛЕ вызова: у `out`-параметра
// движок пишет прямо в переменную блока, и прочитать её обратно — единственный способ
// увидеть результат.
struct proto_result {
    value ret;  // пусто у `proto void`
    std::array<value, script::max_proto_args> args{};
    std::size_t count = 0;

    value arg(std::size_t index) const { return index < count ? args[index] : value{}; }
};

namespace detail {

// Тег для переменной, шаблона которой нет (возврат, и «любой» параметр `void`).
inline std::uint32_t tag_of(const value& v) {
    if (v.is<bool>()) {
        return script::tag_bool;
    }
    if (v.is<i32>()) {
        return script::tag_int;
    }
    if (v.is<f32>()) {
        return script::tag_float;
    }
    if (v.is<vector>()) {
        return script::tag_vector;
    }
    if (v.is<obj>()) {
        return script::tag_class;
    }
    if (v.is<type>()) {
        return script::tag_typename;
    }
    return v.is<std::string>() ? script::tag_string : 0u;
}

// Шаблон параметра функции с ровно таким тегом; nullptr, если такого параметра нет.
inline const void* param_template_of_tag(const script::method& fn, std::uint32_t tag) {
    for (std::size_t i = 0; i < script::param_count(fn); ++i) {
        const void* tpl = script::param_template(fn, i);
        if (tpl && *reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(tpl) +
                                                           layout::var_type) == tag) {
            return tpl;
        }
    }
    return nullptr;
}

// Контекст встроенных типов — туда смотрит +24 всякой переменной int, string, vector, в
// каком бы модуле её ни объявили. Его носит дескриптор корня иерархии, Class. nullptr —
// движок ещё не разобрал ядро (или его нет вовсе, как в тестах): тогда собирать переменную
// не из чего, и вызов получает отказ no_template, а не переменную с нулём в +24 (с ним
// движок падает на разрешении типа — проверено).
inline void* builtin_context() {
    void* root = script::find_class("Class");
    return root ? *reinterpret_cast<void**>(static_cast<char*>(root) + layout::class_context)
                : nullptr;
}

// Переменная встроенного типа там, где шаблона нет (см. script::var::of_tag).
inline bool builtin_var(std::uint32_t tag, script::var& out) {
    void* ctx = tag != 0 ? builtin_context() : nullptr;
    if (!ctx) {
        return false;
    }
    out = script::var::of_tag(tag, ctx);
    return true;
}

// Флаги, которые шаблон чужого параметра принёс, а переменной возврата не нужны: const,
// «не писать» и out. С 0x40 движок просто молча не запишет результат (проверено в
// GetModule и Spawn — оба его читают).
//
// Кроме vector: у него 0x40 значит другое — «значение лежит по указателю из +0». Его
// ставит каждой vector-переменной сама подготовка кадра (sub_1403663B0), и только с ним
// присваивание копирует 12 байт через указатель (sub_140347CB0); без него движок пишет
// 4 байта прямо в +0, поверх указателя. Так `proto vector` (Object.ModelToWorld)
// возвращал нули.
inline void make_writable(script::var& v) {
    auto* flags = reinterpret_cast<std::uint32_t*>(v.raw + layout::var_flags);
    *flags &= ~0xD0u;
    if ((v.tag() & script::type_family) == script::type_vector) {
        *flags |= 0x40u;
    }
}

}  // namespace detail

// Позвать ГЛОБАЛЬНЫЙ `proto` движка по одному только имплу. Дескриптора у такой функции
// взять неоткуда (см. `blind` ниже), поэтому она приходит сюда голой: импл библиотека знает
// с регистрации — движок сам пронёс его мимо нас вместе с именем.
inline script::method as_global(void* impl) {
    script::method fn;
    fn.impl = impl;
    fn.desc = nullptr;
    fn.flags = layout::flag_static | layout::flag_marshalled;
    fn.executable = impl != nullptr;
    return fn;
}

// Позвать маршалируемый `proto` движка.
//
// self == nullptr — это НЕ ошибка, а вторая форма вызова. Их ровно две, и разбор
// ванильных имплов (re/README.md) показывает обе:
//
//   метод настоящего КЛАССА     impl(self, void*** args, void** ret)   — объект в rcx
//   статический и метод ТИПА-ЗНАЧЕНИЯ  impl(void*** args, void** ret)  — rcx вообще нет
//
// Второй формы у нас сначала не было, и это стоило падения: движок читал rcx как массив
// аргументов. Видно её прямо в сигнатурах: typename.Spawn — `f(a1, a2)`, где `**(*a1)[0]`
// это сам дескриптор класса, а `*a2` — переменная возврата; EnScript.GetClassVar — то же
// самое на четырёх аргументах.
//
// ret_tag — чем объявлен возврат. 0 значит `void`: переменная всё равно подаётся (пустая
// и правильно размеченная), потому что ванильные имплы её наличие не проверяют.
inline std::expected<proto_result, miss> call_proto(const script::method& fn, void* self,
                                                    std::span<const value> args,
                                                    std::uint32_t ret_tag = 0) {
    // Строковые аргументы кладутся в ту же арену, что и возвращаемый текст, — а трамплин
    // натива открывает её только когда САМ возвращает строку. Без своей области вызов
    // наружу из натива, возвращающего int, копил бы строки без края: замерено ростом
    // peak_demand до 50 КБ при кольце в 16 КБ, поймано кейсом Arena_RingHasRoomToSpare.
    const detail::call_scope alive;
    if (!fn.impl) {
        return std::unexpected(miss::not_found);
    }
    // В impl скриптового метода лежит байткод — вызов как C-функции роняет процесс.
    if (!fn.callable()) {
        return std::unexpected(miss::not_native);
    }
    if (args.size() > script::max_proto_args) {
        return std::unexpected(miss::too_many_args);
    }
    // `proto external` у метода объекта устроен как метод типа-значения: в rcx объекта
    // нет, он едет ПЕРВЫМ В БЛОКЕ, и параметров в дескрипторе на один больше объявленных
    // (IEntity.GetLocalPosition: объявлено 0, в дескрипторе 1). Вызывающей стороне знать
    // это незачем — решает бит дескриптора, а номера out-аргументов в ответе остаются её.
    if (self && (fn.flags & (layout::flag_external | layout::flag_static)) == layout::flag_external) {
        if (args.size() + 1 > script::max_proto_args) {
            return std::unexpected(miss::too_many_args);
        }
        value with_self[script::max_proto_args];
        with_self[0] = value{obj{self}};
        std::ranges::copy(args, with_self + 1);
        auto r = call_proto(fn, nullptr, std::span{with_self, args.size() + 1}, ret_tag);
        if (r) {
            std::shift_left(r->args.begin(), r->args.begin() + r->count, 1);
            --r->count;
        }
        return r;
    }
    // ДЕСКРИПТОРА МОЖЕТ И НЕ БЫТЬ. У метода класса он есть всегда, а вот у ГЛОБАЛЬНОГО
    // `proto` движка (Print, ErrorEx) он живёт в таблице функций скриптового модуля, до
    // которой из C++ хода нет: наружу торчит только импл, и тот — с регистрации, по
    // имени. Тогда арность знает вызывающая сторона, а переменные собираются по тегу,
    // как и для параметров, объявленных `void` (var::of_tag).
    const bool blind = fn.desc == nullptr;
    // Арность сверяем с дескриптором, а не с надеждой: неполный блок — это кадр, в
    // котором движок прочитает мусор, и разбираться потом придётся по минидампу.
    if (!blind && args.size() != script::param_count(fn)) {
        return std::unexpected(miss::wrong_arity);
    }

    script::var slots[script::max_proto_args];
    vector vectors[script::max_proto_args]{};
    void* block[script::max_proto_args]{};

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::uint32_t want = detail::tag_of(args[i]);
        if (blind) {
            if (!detail::builtin_var(want, slots[i])) {
                return std::unexpected(miss::no_template);
            }
        } else if (const void* tpl = script::param_template(fn, i)) {
            slots[i] = script::var::from_template(tpl);
        } else {
            return std::unexpected(miss::no_template);
        }
        const std::uint32_t have = slots[i].tag() & script::type_family;
        if (have == script::type_any || have == 0) {
            // Параметр объявлен как `void` — это «любой тип», и настоящую переменную
            // движок собирает на месте вызова по фактическому аргументу. Значит и мы
            // собираем переменную нужного типа целиком, а не переписываем тег в чужой:
            // от типа зависит не только тег.
            const std::uint32_t keep =
                *reinterpret_cast<const std::uint32_t*>(slots[i].raw + layout::var_flags);
            if (!detail::builtin_var(want, slots[i])) {
                return std::unexpected(miss::no_template);
            }
            *reinterpret_cast<std::uint32_t*>(slots[i].raw + layout::var_flags) |= keep;
        } else if (want != 0 && (want & script::type_family) != have) {
            // Сигнатура разошлась с движком. Ловим это ЗДЕСЬ, а не падением внутри
            // движка: сгенерированное зеркало может отстать от игры (ветки #ifdef,
            // патч), и отказ со строкой в журнале — единственный приемлемый исход.
            return std::unexpected(miss::wrong_type);
        }
        // Строку в `out`-параметр не отдаём: ею владеет движок и освободит её сам, а
        // наша живёт в кольце — это порча кучи, тот же запрет, что и на запись char* в
        // array<string>. ponytail: путь наверх есть — подкладывать движковую пустую
        // строку (как var::of_tag, её же освобождение пропускает), но пока такого вызова нет.
        const std::uint32_t flags =
            *reinterpret_cast<const std::uint32_t*>(slots[i].raw + layout::var_flags);
        if ((flags & 0x80) != 0 &&
            (slots[i].tag() & script::type_family) == script::type_string) {
            return std::unexpected(miss::unsafe_arg);
        }
        // vector в 8 байт значения не влезает — там указатель на сами 12 байт. В
        // статическом шаблоне его нет, поэтому подкладываем своё место.
        if ((slots[i].tag() & script::type_family) == script::type_vector) {
            void* storage = &vectors[i];
            std::memcpy(slots[i].raw, &storage, sizeof storage);
        }
        detail::write_var(slots[i].raw, args[i]);
        // Объект — тем способом, каким его держит переменная этого типа: где ждут
        // обёртку, голый указатель импл прочтёт как обёртку и уйдёт в мусор. Обёртки нет
        // — отказ, а не попытка.
        if (void* object = args[i].is<obj>() ? args[i].as<obj>().ptr : nullptr;
            object && script::holds_wrapper(slots[i].raw)) {
            void* wrapper = script::wrapper_of(object);
            if (!wrapper) {
                return std::unexpected(miss::unsafe_arg);
            }
            std::memcpy(slots[i].raw, &wrapper, sizeof wrapper);
        }
        block[i] = slots[i].raw;
    }

    // Переменная возврата подаётся ВСЕГДА, даже у `proto void`: ванильные имплы её
    // разыменовывают, лишь проверяя на ноль сам указатель. Для void собираем int — это
    // законная переменная, в которую просто никто не запишет.
    //
    // Шаблон берём сперва у САМОЙ вызываемой функции: параметр того же типа размечен её же
    // компилятором, и ходить никуда не надо. Нет такого — собираем по тегу.
    const std::uint32_t want_ret = ret_tag != 0 ? ret_tag : script::tag_int;
    script::var         ret_var;
    if (const void* own = blind ? nullptr : detail::param_template_of_tag(fn, want_ret)) {
        ret_var = script::var::from_template(own);
    } else if (!detail::builtin_var(want_ret, ret_var)) {
        return std::unexpected(miss::no_template);
    }
    detail::make_writable(ret_var);
    vector ret_vector{};
    if ((ret_var.tag() & script::type_family) == script::type_vector) {
        void* storage = &ret_vector;
        std::memcpy(ret_var.raw, &storage, sizeof storage);
    }

    // Блок аргументов — не массив, а СТРУКТУРА С ДВУМЯ МАССИВАМИ: по +0 движок берёт
    // указатели на значения, по +8 — на сами переменные. Ванильные имплы пользуются то
    // одним, то другим (`typename.Spawn` — только +0, `EnScript.GetClassVar` — обоими
    // через sub_1403672C0), и подать один голый массив значит уронить второй вид.
    // Значение живёт в самой переменной по +0, поэтому оба массива у нас — один и тот же.
    struct arg_block {
        void** values;
        void** vars;
    };
    arg_block block_desc{block, block};
    void* ret_ptr = ret_var.raw;
    // Дескриптор — последнее слово: у статического метода объекта нет по определению,
    // сколько бы вызывающая сторона ни передала.
    if (self && (fn.flags & layout::flag_static) == 0) {
        reinterpret_cast<std::int64_t(__fastcall*)(void*, arg_block*, void**)>(fn.impl)(
            self, &block_desc, &ret_ptr);
    } else {
        reinterpret_cast<std::int64_t(__fastcall*)(arg_block*, void**)>(fn.impl)(&block_desc,
                                                                                &ret_ptr);
    }

    proto_result out;
    out.count = args.size();
    for (std::size_t i = 0; i < args.size(); ++i) {
        out.args[i] = detail::read_var(slots[i].raw);
    }
    // У `proto void` переменная возврата была нужна только для того, чтобы движку было
    // куда не писать. Читать её нечего — иначе наружу поехал бы её ноль.
    if (ret_tag != 0) {
        out.ret = detail::read_var(ret_var.raw);
    }
    return out;
}

namespace detail {

// ── Типизированная обёртка ──────────────────────────────────────────────────
// Дальше — только перевод обычных типов C++ в value и обратно, чтобы сгенерированное
// зеркало движкового API читалось как обычный C++.

// Out-аргумент маршалируемого вызова передаётся как std::ref(x): после вызова в x
// ляжет то, что движок записал в переменную блока. Без метки аргумент — только вход.
template <class T>
inline constexpr bool written_back = false;
template <class T>
inline constexpr bool written_back<std::reference_wrapper<T>> = true;

template <class T>
value to_arg(const T& v) {
    if constexpr (written_back<T>) {
        return to_arg(v.get());
    } else if constexpr (script_class<T>) {
        return value{obj{v.ptr}};
    } else if constexpr (requires { v.ptr; }) {
        // Контейнер (array, set, map) в переменной — тоже ссылка на объект движка: тег
        // семейства class, значение — указатель (ErrorModuleHandler.GetErrorModules).
        return value{obj{v.ptr}};
    } else if constexpr (std::is_same_v<T, const char*>) {
        return value{std::string{v ? v : ""}};
    } else if constexpr (std::is_convertible_v<T, std::string_view> &&
                         !std::is_same_v<T, str>) {
        return value{std::string{std::string_view{v}}};
    } else {
        return value{v};
    }
}

template <class R>
consteval std::uint32_t ret_tag() {
    if constexpr (std::is_void_v<R>) {
        return 0;
    } else if constexpr (std::is_same_v<R, bool>) {
        return script::tag_bool;
    } else if constexpr (std::is_same_v<R, i32>) {
        return script::tag_int;
    } else if constexpr (std::is_same_v<R, f32>) {
        return script::tag_float;
    } else if constexpr (std::is_same_v<R, vector>) {
        return script::tag_vector;
    } else if constexpr (std::is_same_v<R, std::string> || std::is_same_v<R, str>) {
        return script::tag_string;
    } else if constexpr (std::is_same_v<R, type>) {
        return script::tag_typename;
    } else {
        return script::tag_class;
    }
}

template <class R>
R from_value(const value& v) {
    if constexpr (script_class<R>) {
        return R{v.as<obj>().ptr};
    } else if constexpr (std::is_same_v<R, std::string>) {
        return std::string{v.view()};
    } else {
        return v.as<R>();
    }
}

template <class R>
R from_result(const proto_result& r) {
    if constexpr (std::is_void_v<R>) {
        return;
    } else {
        return from_value<R>(r.ret);
    }
}

// Раздать out-аргументы обратно вызывающему: i-й элемент пачки — i-я переменная блока.
template <class... A>
void write_back(const proto_result& r, const A&... args) {
    std::size_t i   = 0;
    const auto  one = [&]<class T>(const T& arg) {
        if constexpr (written_back<T>) {
            arg.get() = from_value<std::remove_cv_t<typename T::type>>(r.arg(i));
        }
        ++i;
    };
    (one(args), ...);
}

template <class R, class... A>
std::expected<R, miss> finish_proto(const std::expected<proto_result, miss>& r,
                                    const A&... args) {
    if (!r) {
        return std::unexpected(r.error());
    }
    write_back(*r, args...);
    if constexpr (std::is_void_v<R>) {
        return {};
    } else {
        return from_result<R>(*r);
    }
}

// Обёртка над call_proto с поиском метода по именам класса и метода. Дескриптор ищется
// один раз на пару <класс, метод> — той же мемоизацией, что и у прямого вызова.
// Хвостовой пустой элемент — не запас, а единственный способ объявить массив при нуле
// аргументов; в блок уходит ровно sizeof...(A) первых.
template <class R, name_t Klass, name_t Method, class... A>
std::expected<R, miss> proto_invoke(void* self, const A&... args) {
    // Метод объекта без объекта — промах, а не вторая форма вызова: ту выбирает
    // proto_invoke_on_value, и выбирает осознанно.
    if (!self) {
        return std::unexpected(miss::null_object);
    }
    const script::method fn = engine_method<Klass, Method>();
    const value block[] = {to_arg(args)..., value{}};
    return finish_proto<R>(
        call_proto(fn, self, std::span{block}.first(sizeof...(A)), ret_tag<R>()), args...);
}

// Статический `proto`: объекта нет, двухаргументная форма (DayZPhysics.RayCastBullet).
template <class R, name_t Klass, name_t Method, class... A>
std::expected<R, miss> proto_invoke_static(const A&... args) {
    const script::method fn      = engine_method<Klass, Method>();
    const value          block[] = {to_arg(args)..., value{}};
    return finish_proto<R>(
        call_proto(fn, nullptr, std::span{block}.first(sizeof...(A)), ret_tag<R>()), args...);
}

// То же, но у метода приёмник — ЗНАЧЕНИЕ, а не объект.
//
// Разница снята с живого движка (кейс Out_ReceiverKindDecidesBlockShape): у методов
// объекта в дескрипторе ровно столько параметров, сколько объявлено, — сам объект едет
// в rcx. А у методов типов-значений (`typename`, `string`) параметров на один больше:
// объекта, который можно положить в rcx, у них нет, поэтому приёмник едет ПЕРВЫМ В
// БЛОКЕ. Измеренное: string.Substring(2 объявлено) -> 3, typename.Spawn(0) -> 1,
// PlayerIdentity.GetPlainId(0) -> 0, EnScript.GetClassVar(4, static) -> 4.
template <class R, name_t Klass, name_t Method, class... A>
std::expected<R, miss> proto_invoke_on_value(const value& receiver, const A&... args) {
    const script::method fn = engine_method<Klass, Method>();
    const value block[] = {receiver, to_arg(args)...};
    // Объекта нет — значит нет и rcx: зовём двухаргументную форму.
    return finish_proto<R>(call_proto(fn, nullptr, block, ret_tag<R>()), receiver, args...);
}

}  // namespace detail
}  // namespace graft
