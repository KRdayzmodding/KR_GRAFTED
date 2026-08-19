// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include <array>
#include <cstdint>
#include <expected>
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
// Шаблона нет только у переменной ВОЗВРАТА (дескриптор её не носит) — её собираем по
// тегу. Если однажды окажется, что этого мало, чинить надо здесь и только здесь.
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

// ── Донорские шаблоны ────────────────────────────────────────────────────────
// Движок НИКОГДА не собирает скриптовую переменную с нуля — он копирует готовый
// 40-байтный шаблон (sub_1403663B0). Значит и нам собирать нельзя: у переменной есть
// поля, которых мы не разобрали, и одно из них движку нужно для разрешения типа.
// Синтезированная переменная с верным тегом, но пустым остатком роняет процесс —
// проверено падением: в момент разрешения типа rcx = 0, а в rax наш же тег.
//
// Параметрам шаблон даёт сама вызываемая функция. А вот возврату его взять неоткуда:
// дескриптор функции его не носит. Поэтому берём у ЧУЖОГО ПАРАМЕТРА того же типа —
// функции ниже выбраны за то, что они есть в любой сборке и вместе покрывают все
// семейства. Не нашлось донора — отказ, а не попытка сочинить.
struct donor_site {
    const char* class_name;
    const char* method;
    std::size_t param;
};

inline const void* donor_template(std::uint32_t tag) {
    struct slot {
        std::uint32_t tag;
        donor_site from;
    };
    // EnScript.GetClassVar(Class inst, string varname, int index, out void result)
    // Physics.SetMass(float), Physics.SetVelocity(vector)
    // EnProfiler.GetTimeOfClass(typename clss, bool immediate)
    static constexpr slot kDonors[] = {
        {script::tag_class, {"EnScript", "GetClassVar", 0}},
        {script::tag_string, {"EnScript", "GetClassVar", 1}},
        {script::tag_int, {"EnScript", "GetClassVar", 2}},
        {script::tag_float, {"Physics", "SetMass", 0}},
        {script::tag_vector, {"Physics", "SetVelocity", 0}},
        {script::tag_typename, {"EnProfiler", "GetTimeOfClass", 0}},
        {script::tag_bool, {"EnProfiler", "GetTimeOfClass", 1}},
    };
    static const void* found[std::size(kDonors)]{};
    for (std::size_t i = 0; i < std::size(kDonors); ++i) {
        if (kDonors[i].tag != tag) {
            continue;
        }
        if (!found[i]) {
            // Промах не кэшируем: первый вызов может случиться раньше, чем движок
            // разобрал нужный модуль.
            found[i] = script::param_template(
                script::find_method(kDonors[i].from.class_name, kDonors[i].from.method),
                kDonors[i].from.param);
        }
        return found[i];
    }
    return nullptr;
}

// Флаги, которые донор принёс от параметра, а переменной возврата не нужны: const,
// «не писать» и out. С 0x40 движок просто молча не запишет результат (проверено в
// GetModule и Spawn — оба его читают).
inline void make_writable(script::var& v) {
    auto* flags = reinterpret_cast<std::uint32_t*>(v.raw + layout::var_flags);
    *flags &= ~0xD0u;
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
    // ДЕСКРИПТОРА МОЖЕТ И НЕ БЫТЬ. У метода класса он есть всегда, а вот у ГЛОБАЛЬНОГО
    // `proto` движка (Print, ErrorEx) он живёт в таблице функций скриптового модуля, до
    // которой из C++ хода нет: наружу торчит только импл, и тот — с регистрации, по
    // имени. Тогда арность знает вызывающая сторона, а шаблоны берутся у доноров, как
    // они и так берутся для параметров, объявленных `void`.
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
        const void* tpl = blind ? detail::donor_template(detail::tag_of(args[i]))
                                : script::param_template(fn, i);
        if (!tpl) {
            return std::unexpected(miss::no_template);
        }
        slots[i] = script::var::from_template(tpl);
        const std::uint32_t want = detail::tag_of(args[i]);
        const std::uint32_t have = slots[i].tag() & script::type_family;
        if (have == script::type_any || have == 0) {
            // Параметр объявлен как `void` — это «любой тип», и настоящую переменную
            // движок собирает на месте вызова по фактическому аргументу. Значит и мы
            // берём готовую переменную нужного типа, а не переписываем тег в чужой:
            // от типа зависит не только тег.
            const std::uint32_t keep =
                *reinterpret_cast<const std::uint32_t*>(slots[i].raw + layout::var_flags);
            const void* filled = detail::donor_template(want);
            if (!filled) {
                return std::unexpected(miss::no_template);
            }
            slots[i] = script::var::from_template(filled);
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
        // строку (ctx+1144, её же освобождение пропускает), но пока такого вызова нет.
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
        block[i] = slots[i].raw;
    }

    // Переменная возврата подаётся ВСЕГДА, даже у `proto void`: ванильные имплы её
    // разыменовывают, лишь проверяя на ноль сам указатель. Для void берём донора int —
    // это законная переменная, в которую просто никто не запишет.
    //
    // Шаблон ищем сперва у САМОЙ вызываемой функции: если у неё есть параметр того же
    // типа, это лучший донор какой бывает — он от неё же и ходить никуда не надо.
    const std::uint32_t want_ret = ret_tag != 0 ? ret_tag : script::tag_int;
    const void* ret_tpl = nullptr;
    for (std::size_t i = 0; !blind && i < script::param_count(fn) && !ret_tpl; ++i) {
        const void* tpl = script::param_template(fn, i);
        if (tpl && *reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(tpl) +
                                                           layout::var_type) == want_ret) {
            ret_tpl = tpl;
        }
    }
    if (!ret_tpl) {
        ret_tpl = detail::donor_template(want_ret);
    }
    if (!ret_tpl) {
        return std::unexpected(miss::no_template);
    }
    script::var ret_var = script::var::from_template(ret_tpl);
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
    // куда не писать. Читать её нечего — иначе наружу поехал бы ноль донора.
    if (ret_tag != 0) {
        out.ret = detail::read_var(ret_var.raw);
    }
    return out;
}

namespace detail {

// ── Типизированная обёртка ──────────────────────────────────────────────────
// Дальше — только перевод обычных типов C++ в value и обратно, чтобы сгенерированное
// зеркало движкового API читалось как обычный C++.

template <class T>
value to_arg(const T& v) {
    if constexpr (script_class<T>) {
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
R from_result(const proto_result& r) {
    if constexpr (std::is_void_v<R>) {
        return;
    } else if constexpr (script_class<R>) {
        return R{r.ret.as<obj>().ptr};
    } else if constexpr (std::is_same_v<R, std::string>) {
        return std::string{r.ret.view()};
    } else {
        return r.ret.as<R>();
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
    const auto r = call_proto(fn, self, std::span{block}.first(sizeof...(A)), ret_tag<R>());
    if (!r) {
        return std::unexpected(r.error());
    }
    if constexpr (std::is_void_v<R>) {
        return {};
    } else {
        return from_result<R>(*r);
    }
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
    const auto r = call_proto(fn, nullptr, block, ret_tag<R>());
    if (!r) {
        return std::unexpected(r.error());
    }
    if constexpr (std::is_void_v<R>) {
        return {};
    } else {
        return from_result<R>(*r);
    }
}

}  // namespace detail
}  // namespace graft
