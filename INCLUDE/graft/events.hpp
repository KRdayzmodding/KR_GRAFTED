// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <format>
#include <source_location>
#include <string_view>
#include <type_traits>
#include <utility>

// СОБЫТИЯ ВМЕСТО ТИКА. С 0.6 плагину не нужно сидеть на GRAFT_ON_TICK и спрашивать каждый
// кадр «пора?»: он подписывается на то, что случилось, ставит работу на поток скрипта из
// любого потока и заводит таймеры. Тик остаётся — для того, что правда нужно каждый кадр.
//
//   struct zones_changed {                                  // событие — простая структура с именем
//       static constexpr std::string_view name = "ce.zones.changed";
//       std::uint64_t generation;
//   };
//   graft::subscription s = graft::on<zones_changed>([&](const zones_changed& e) { ... });
//   graft::publish(zones_changed{.generation = 7});         // из любого потока
//   graft::post([] { /* движок */ });                       // на поток скрипта из любого потока
//   graft::subscription t = graft::every(std::chrono::hours{1}, [&] { ... });
//   graft::subscription r = graft::on<graft::running>([](const graft::running&) { ... });
//
// ПРАВИЛА — их держит хост, а не дисциплина плагина:
//   1. Обработчики, задания и таймеры идут ТОЛЬКО на потоке скрипта: там же, где кадр и нативы,
//      поэтому из них можно звать движок. Своего потока у graft нет и не будет.
//   2. Публикация на потоке скрипта зовёт подписчиков СРАЗУ, по порядку подписки: ссылки на
//      объекты движка в событии живы весь обработчик. С другого потока — копия в очередь.
//   3. Очередь разбирает кадр в общем бюджете (`-graftBudget=<мс>`, по умолчанию 2), хотя бы одно
//      задание за кадр. Поставленное из задания — на следующем кадре.
//   4. Каждый вызов под своей ловушкой: упал один — остальные идут; в журнале DLL и «файл:строка».
//   5. Подписка — объект: разрушен — снята. С чужого потока снятие ждёт обработчик, уже идущий
//      на потоке скрипта; изнутри своего обработчика — можно, замыкание живо до конца вызова.
//   6. Событие едет между DLL байтами: только тривиально копируемое, размер сверяется по имени —
//      разный у подписчика и издателя значит отказ. Сменился смысл полей — новое имя.
//   7. Состояние (publish_state) хост помнит: опоздавший подписчик получит его на ближайшем кадре.
//
// Совместимость: ABI прежний (7), поля хоста дописаны в конец graft_host_api. Плагин 0.6 на хосте
// старше 0.6 грузится, но события ему недоступны: on/post/... отвечают «нет», хост пишет причину.
namespace graft {

// ── События самого хоста ─────────────────────────────────────────────────────
// Этапы — состояния: подписавшийся позже получит их сразу (на ближайшем кадре).
struct linked {  // движок зовётся: классы находятся, нативы привязаны
    static constexpr std::string_view name = "graft.linked";
};
struct running {  // пошли кадры, у объектного графа есть корень (graft::game())
    static constexpr std::string_view name = "graft.running";
};
// Кадр — на потоке скрипта, раз в кадр, после старых тиков. Только для работы, которой и правда
// нужен каждый кадр; остальное — события, таймеры, post.
struct tick {
    static constexpr std::string_view name = "graft.tick";
    float dt = 0;               // timeslice движка
    std::uint64_t number = 0;   // номер кадра с начала процесса
};

template <class E>
concept event = std::is_trivially_copyable_v<E> && requires {
    { E::name } -> std::convertible_to<std::string_view>;
};

namespace detail {

// Мост к хосту. У плагина — поля graft_host_api (client.cpp), у самого хоста — его хаб
// (events.cpp). Неудача (нет хоста 0.6, отказ) — drop уже позван: замыкание не течёт.
bool events_ready();
bool on_script_thread();
bool script_thread_known();  // кадры уже идут: поток скрипта известен
bool post(void (*run)(void*), void (*drop)(void*), void* ctx, const char* where);
std::uint64_t subscribe(const char* topic, std::uint32_t size, void (*handler)(void*, const void*),
                        void (*drop)(void*), void* ctx, const char* where);
void unsubscribe(std::uint64_t id);
bool publish(const char* topic, std::uint32_t size, const void* event, bool state);
std::uint64_t add_timer(std::uint32_t first_ms, std::uint32_t every_ms, void (*run)(void*), void (*drop)(void*),
                        void* ctx, const char* where);

// «файл:строка» без выделений: строка уходит хосту на копию.
struct where_text {
    char text[192]{};
};

inline where_text where_of(std::source_location at) {
    where_text out;
    std::string_view file = at.file_name();
    // Путь сборщика у всех свой; в журнал — хвост, по которому файл и так узнают.
    if (file.size() > 120) {
        file = file.substr(file.size() - 120);
    }
    const auto end = std::format_to_n(out.text, sizeof out.text - 1, "{}:{}", file, at.line());
    *end.out = '\0';
    return out;
}

// Имя темы уходит хосту строкой C: имя события обязано быть литералом.
template <event E>
consteval bool name_is_literal() {
    return E::name.data()[E::name.size()] == '\0';
}

// Жизнь подписки. busy считает идущие вызовы: снятие с чужого потока ждёт его нуля.
struct slot {
    std::atomic<bool> alive{true};
    std::atomic<int> busy{0};
};

template <class F>
struct boxed : slot {
    F fn;
    explicit boxed(F&& f) : fn(std::move(f)) {}
    explicit boxed(const F& f) : fn(f) {}
};

// __finally — чтобы busy снимался и при аппаратном сбое внутри обработчика (ловит хост выше).
// В функции с __try не должно быть объектов с деструктором — здесь только указатели.
inline void call_slot(slot* s, void (*body)(slot*, const void*), const void* arg) {
    s->busy.fetch_add(1, std::memory_order_acquire);
    __try {
        if (s->alive.load(std::memory_order_acquire)) {
            body(s, arg);
        }
    } __finally {
        s->busy.fetch_sub(1, std::memory_order_release);
        s->busy.notify_all();
    }
}

template <event E, class B>
void call_handler(void* ctx, const void* e) {
    call_slot(static_cast<B*>(ctx), [](slot* s, const void* ev) { static_cast<B*>(s)->fn(*static_cast<const E*>(ev)); }, e);
}

template <class B>
void call_timer(void* ctx) {
    call_slot(static_cast<B*>(ctx), [](slot* s, const void*) { static_cast<B*>(s)->fn(); }, nullptr);
}

template <class T>
void drop_box(void* ctx) {
    delete static_cast<T*>(ctx);
}

template <class F>
void run_once(void* ctx) {
    (*static_cast<F*>(ctx))();
}

}  // namespace detail

// Подписка или таймер. Только перемещается; разрушение снимает.
class subscription {
public:
    subscription() = default;
    subscription(std::uint64_t id, detail::slot* s) noexcept : id_(id), slot_(s) {}
    subscription(subscription&& other) noexcept
        : id_(std::exchange(other.id_, 0)), slot_(std::exchange(other.slot_, nullptr)) {}
    subscription& operator=(subscription&& other) noexcept {
        if (this != &other) {
            reset();
            id_   = std::exchange(other.id_, 0);
            slot_ = std::exchange(other.slot_, nullptr);
        }
        return *this;
    }
    subscription(const subscription&)            = delete;
    subscription& operator=(const subscription&) = delete;
    ~subscription() { reset(); }

    // Снять. После возврата новый вызов не начнётся; с чужого потока — и идущий уже кончился.
    void reset() {
        if (id_ == 0) {
            return;
        }
        slot_->alive.store(false, std::memory_order_release);
        if (!detail::on_script_thread()) {
            for (int b = slot_->busy.load(std::memory_order_acquire); b != 0; b = slot_->busy.load(std::memory_order_acquire)) {
                slot_->busy.wait(b, std::memory_order_acquire);
            }
        }
        slot_ = nullptr;
        detail::unsubscribe(std::exchange(id_, 0));  // после него хост вправе отпустить замыкание
    }

    // Оставить до конца процесса: объект забывает подписку, хост её держит. Для подписок плагина «навсегда» —
    // статик с подпиской разрушался бы на выходе процесса, когда поток скрипта уже убит.
    void release() noexcept {
        id_   = 0;
        slot_ = nullptr;
    }

    explicit operator bool() const noexcept { return id_ != 0; }
    std::uint64_t id() const noexcept { return id_; }

private:
    std::uint64_t id_    = 0;
    detail::slot* slot_ = nullptr;
};

// Есть ли у хоста события (хост 0.6 и новее).
inline bool events_available() {
    return detail::events_ready();
}

// Это поток скрипта: кадр, нативы, обработчики событий.
inline bool on_script_thread() {
    return detail::on_script_thread();
}

// Подписаться на событие E. Пусто (false) — хост отказал: другой размер у темы или хост старше 0.6.
template <event E, class F>
    requires std::invocable<std::decay_t<F>&, const E&>
[[nodiscard]] subscription on(F&& handler, std::source_location at = std::source_location::current()) {
    static_assert(detail::name_is_literal<E>(), "имя события — строковый литерал: E::name = \"...\"");
    using box_t      = detail::boxed<std::decay_t<F>>;
    auto*       box  = new box_t(std::forward<F>(handler));
    const auto  here = detail::where_of(at);
    const auto  id   = detail::subscribe(E::name.data(), static_cast<std::uint32_t>(sizeof(E)),
                                         &detail::call_handler<E, box_t>, &detail::drop_box<box_t>, box, here.text);
    return id != 0 ? subscription{id, box} : subscription{};
}

// Опубликовать. С потока скрипта — подписчикам сразу; с другого — на ближайшем кадре.
template <event E>
bool publish(const E& e) {
    static_assert(detail::name_is_literal<E>(), "имя события — строковый литерал: E::name = \"...\"");
    return detail::publish(E::name.data(), static_cast<std::uint32_t>(sizeof(E)), &e, false);
}

// Опубликовать и запомнить: опоздавший подписчик получит последнее значение.
template <event E>
bool publish_state(const E& e) {
    static_assert(detail::name_is_literal<E>(), "имя события — строковый литерал: E::name = \"...\"");
    return detail::publish(E::name.data(), static_cast<std::uint32_t>(sizeof(E)), &e, true);
}

// Сделать на потоке скрипта, на ближайшем кадре. Из любого потока, в том числе из самого скрипта.
template <class F>
    requires std::invocable<std::decay_t<F>&>
bool post(F&& fn, std::source_location at = std::source_location::current()) {
    using fn_t      = std::decay_t<F>;
    auto*      box  = new fn_t(std::forward<F>(fn));
    const auto here = detail::where_of(at);
    return detail::post(&detail::run_once<fn_t>, &detail::drop_box<fn_t>, box, here.text);
}

namespace detail {

inline std::uint32_t as_ms(std::chrono::nanoseconds d) {
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(d).count();
    return ms <= 0 ? 0u : ms >= 0xFFFFFFFFll ? 0xFFFFFFFFu : static_cast<std::uint32_t>(ms);
}

template <class F>
subscription start_timer(std::uint32_t first, std::uint32_t every, F&& fn, std::source_location at) {
    using box_t     = boxed<std::decay_t<F>>;
    auto*      box  = new box_t(std::forward<F>(fn));
    const auto here = where_of(at);
    const auto id   = add_timer(first, every, &call_timer<box_t>, &drop_box<box_t>, box, here.text);
    return id != 0 ? subscription{id, box} : subscription{};
}

}  // namespace detail

// Один раз через delay — на потоке скрипта. Точность — кадр.
template <class Rep, class Period, class F>
    requires std::invocable<std::decay_t<F>&>
[[nodiscard]] subscription after(std::chrono::duration<Rep, Period> delay, F&& fn,
                                 std::source_location at = std::source_location::current()) {
    return detail::start_timer(detail::as_ms(delay), 0, std::forward<F>(fn), at);
}

// Каждые period — на потоке скрипта. Пропущенное за простой не догоняется пачкой.
template <class Rep, class Period, class F>
    requires std::invocable<std::decay_t<F>&>
[[nodiscard]] subscription every(std::chrono::duration<Rep, Period> period, F&& fn,
                                 std::source_location at = std::source_location::current()) {
    const std::uint32_t ms = detail::as_ms(period) == 0 ? 1u : detail::as_ms(period);
    return detail::start_timer(ms, ms, std::forward<F>(fn), at);
}

}  // namespace graft
