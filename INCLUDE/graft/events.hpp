// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <source_location>
#include <string_view>
#include <type_traits>
#include <utility>

#include "graft/abi.h"

// СОБЫТИЯ ВМЕСТО ТИКА. С 0.6 плагину не нужно сидеть на GRAFT_ON_TICK и спрашивать каждый
// кадр «пора?»: он объявляет, на что отвечает, ставит работу на поток скрипта из любого потока
// и заводит таймеры. Тик остаётся — для того, что правда нужно каждый кадр.
//
//   struct zones_changed {                                  // событие — простая структура с именем
//       static constexpr std::string_view name = "ce.zones.changed";
//       std::uint64_t generation;
//   };
//
//   graft::on<graft::running> started{&start};              // объявление в любом файле плагина
//   graft::on<zones_changed>  changed{[](const zones_changed& e) { ... }};
//
//   graft::publish(zones_changed{.generation = 7});         // из любого потока
//   graft::post([] { /* движок */ });                       // на поток скрипта из любого потока
//   graft::every(std::chrono::hours{1}, &backup);           // до конца плагина; .cancel() — снять
//
// ПРАВИЛА — их держит хост, а не дисциплина плагина:
//   1. Обработчики, задания и таймеры идут ТОЛЬКО на потоке скрипта: там же, где кадр и нативы,
//      поэтому из них можно звать движок. Своего потока у graft нет и не будет.
//   2. Публикация на потоке скрипта зовёт подписчиков СРАЗУ, по порядку подписки: ссылки на
//      объекты движка в событии живы весь обработчик. С другого потока — копия в очередь.
//   3. Очередь разбирает кадр в общем бюджете (`-graftBudget=<мс>`, по умолчанию 2), хотя бы одно
//      задание за кадр. Поставленное из задания — на следующем кадре.
//   4. Каждый вызов под своей ловушкой: упал один — остальные идут; в журнале DLL и «файл:строка».
//   5. Подписка — объект on<E>. Объявлен в файле, вне функций, — подписка живёт, пока жив плагин,
//      и GRAFT_ON_LOAD для неё не нужен: хосту её отдаст точка входа плагина. Полем класса или
//      в функции — снимается вместе с объектом (с чужого потока — дождавшись идущего вызова).
//      Таймер и задание объекта не требуют: живут до cancel(), срабатывания или конца плагина.
//   6. Событие едет между DLL байтами: только тривиально копируемое, размер сверяется по имени —
//      разный у подписчика и издателя значит отказ. Сменился смысл полей — новое имя.
//   7. Состояние (publish_state) хост помнит: опоздавший подписчик получит его на ближайшем кадре.
//   8. Тема без подписчиков ничего не стоит: publish — одно чтение счётчика, а события хоста
//      (игроки, миссия) движок не опрашивают, пока на них никто не подписан. Свой дорогой
//      источник плагин будит так же — source<E> или wanted<E>().
//
// Совместимость: ABI прежний (7), поля хоста дописаны в конец graft_host_api. Плагин 0.6 на хосте
// старше 0.6 грузится, но события ему недоступны: on/post/... отвечают «нет», хост пишет причину.
namespace graft {

template <class E>
concept event = std::is_trivially_copyable_v<E> && requires {
    { E::name } -> std::convertible_to<std::string_view>;
};

// Строковое поле события — массив с нулём в конце: по DLL едут байты, не std::string.
template <std::size_t N>
std::string_view as_text(const char (&field)[N]) {
    return {field, strnlen(field, N)};
}

// Записать строку в поле. Длиннее поля — обрезается по границе символа UTF-8.
template <std::size_t N>
void put(char (&field)[N], std::string_view s) {
    std::size_t n = s.size() < N - 1 ? s.size() : N - 1;
    while (n < s.size() && n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) {
        --n;
    }
    std::memcpy(field, s.data(), n);
    field[n] = '\0';
}

// ── События хоста ────────────────────────────────────────────────────────────
// Этапы — состояния: подписавшийся позже получит их сразу (на ближайшем кадре).
struct linked { // движок зовётся: классы находятся, нативы привязаны
    static constexpr std::string_view name = "graft.linked";
};

struct running { // пошли кадры, у объектного графа есть корень (graft::game())
    static constexpr std::string_view name = "graft.running";
};

// Кадр — на потоке скрипта, раз в кадр, после старых тиков. Только для работы, которой и правда
// нужен каждый кадр; остальное — события, таймеры, post.
struct tick {
    static constexpr std::string_view name   = "graft.tick";
    float                             dt     = 0; // timeslice движка
    std::uint64_t                     number = 0; // номер кадра с начала процесса
};

// Миссия поднялась: объекты можно ставить (как ванильный ObjectSpawner — на её загрузке). Состояние.
struct mission_ready {
    static constexpr std::string_view name = "graft.mission_ready";
    char                              world[64]{}; // GetWorldName
};

// Сколько игроков на сервере и мест. Состояние; публикуется при смене, проверка — раз в секунду.
struct players_online {
    static constexpr std::string_view name  = "graft.players_online";
    std::int32_t                      count = 0;
    std::int32_t                      slots = 0; // maxPlayers конфига сервера
};

// Игрок появился в списке сервера (PlayerIdentity). Первая проверка после подписки называет
// вошедшими всех, кто уже на сервере.
struct player_joined {
    static constexpr std::string_view name = "graft.player_joined";
    char                              uid[32]{};  // Steam ID (GetPlainId)
    char                              nick[64]{}; // GetName
};

struct player_left {
    static constexpr std::string_view name = "graft.player_left";
    char                              uid[32]{};
};

namespace detail {

// Жизнь подписки. busy — идущие вызовы: снятие с чужого потока ждёт его нуля. Пишет busy только поток
// скрипта (вызовы идут только там), поэтому вызов отмечается без lock-инструкций, а порядок «снят → вызова
// нет» держит барьер снимающего (wait_idle). id — номер у хоста (0 — ещё не отдана: хоста пока нет; или снята).
struct slot {
    std::atomic<bool>          alive{true};
    std::atomic<int>           busy{0};
    std::atomic<std::uint64_t> id{0};
};

template <class F>
struct boxed : slot {
    F fn;

    explicit boxed(F&& f) : fn(std::move(f)) {}

    explicit boxed(const F& f) : fn(f) {}
};

// Мост к хосту. У плагина — поля graft_host_api (client.cpp), у самого хоста — его хаб
// (events.cpp). Неудача (нет хоста 0.6, отказ) — drop уже позван, s->alive снят. До знакомства
// с хостом (статики плагина) заявки ждут точки входа плагина.
bool               events_ready();
bool               host_bound(); // хост уже представился: объявления вне функций — раньше
bool               on_script_thread();
bool               script_thread_known(); // кадры уже идут: поток скрипта известен
std::uint64_t      epoch();               // меняется со сменой хоста; 0 — хоста нет
std::uint64_t      frames();
const graft_topic* topic(const char* name, std::uint32_t size, const char* where);
bool               post(void (*run)(void*), void (*drop)(void*), void* ctx, const char* where);
bool               subscribe(const char* topic, std::uint32_t size, void (*handler)(void*, const void*), void (*drop)(void*), void* ctx, const char* where, const std::shared_ptr<slot>& s);
bool               watch(const char* topic, std::uint32_t size, void (*changed)(void*, std::uint8_t), void (*drop)(void*), void* ctx, const char* where, const std::shared_ptr<slot>& s);
void               unsubscribe(std::uint64_t id);
bool               publish(const graft_topic* topic, const void* event, bool state);
bool               add_timer(std::uint32_t first_ms, std::uint32_t every_ms, void (*run)(void*), void (*drop)(void*), void* ctx, const char* where, const std::shared_ptr<slot>& s);
// Снятие с чужого потока: барьер на все потоки процесса и ожидание идущего вызова (SRC/graft/slot.cpp).
void wait_idle(slot& s);

inline std::uint32_t listeners(const graft_topic& t) {
    return std::atomic_ref<std::uint32_t>(const_cast<std::uint32_t&>(t.listeners)).load(std::memory_order_relaxed);
}

// «файл:строка» без выделений: строка уходит хосту на копию.
struct where_text {
    char text[192]{};
};

inline where_text where_of(std::source_location at) {
    where_text       out;
    std::string_view file = at.file_name();
    // Путь сборщика у всех свой; в журнал — хвост, по которому файл и так узнают.
    if (file.size() > 120) {
        file = file.substr(file.size() - 120);
    }
    const auto end = std::format_to_n(out.text, sizeof out.text - 1, "{}:{}", file, at.line());
    *end.out       = '\0';
    return out;
}

// Имя темы уходит хосту строкой C: имя события обязано быть литералом.
template <event E>
consteval bool name_is_literal() {
    return E::name.data()[E::name.size()] == '\0';
}

// Тема события E — один раз на хост: дальше publish и wanted читают её счётчик без хоста.
template <event E>
const graft_topic* topic_of() {
    static_assert(name_is_literal<E>(), "имя события — строковый литерал: E::name = \"...\"");
    static std::atomic<const graft_topic*> cached{nullptr};
    static std::atomic<std::uint64_t>      cached_for{0};
    const std::uint64_t                    now = epoch();
    if (now == 0) {
        return nullptr;
    }
    if (cached_for.load(std::memory_order_acquire) != now) {
        cached.store(topic(E::name.data(), static_cast<std::uint32_t>(sizeof(E)), "publish"), std::memory_order_release);
        cached_for.store(now, std::memory_order_release); // отказ тоже запомнен: хост скажет о нём один раз
    }
    return cached.load(std::memory_order_acquire);
}

// Горячий путь: обычные запись и чтение, без lock-инструкций и без пробуждения ждущих. Отметить вызов (busy),
// потом проверить alive — снимающий делает наоборот и между ними ставит барьер на все потоки (wait_idle): так
// хотя бы один из двоих видит другого. __finally — чтобы busy снимался и при аппаратном сбое внутри обработчика
// (ловит хост выше). В функции с __try не должно быть объектов с деструктором — здесь только числа и указатели.
inline void call_slot(slot* s, void (*body)(slot*, const void*), const void* arg) {
    const int was = s->busy.load(std::memory_order_relaxed);
    s->busy.store(was + 1, std::memory_order_relaxed);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    __try {
        if (s->alive.load(std::memory_order_relaxed)) {
            body(s, arg);
        }
    } __finally {
        s->busy.store(was, std::memory_order_release);
    }
}

// Замыкание у хоста — владение коробкой: отпускает его drop, а объект на стороне плагина (on<E>,
// timer) держит ту же коробку, пока жив сам.
using holder = std::shared_ptr<slot>;

template <class B>
B* box_of(void* ctx) {
    return static_cast<B*>(static_cast<holder*>(ctx)->get());
}

inline void drop_holder(void* ctx) {
    delete static_cast<holder*>(ctx);
}

template <event E, class B>
void call_handler(void* ctx, const void* e) {
    call_slot(box_of<B>(ctx), [](slot* s, const void* ev) {
        auto& fn = static_cast<B*>(s)->fn;
        if constexpr (std::invocable<decltype(fn)&, const E&>) {
            fn(*static_cast<const E*>(ev));
        } else {
            fn();
        } }, e);
}

template <class B>
void call_watcher(void* ctx, std::uint8_t listened) {
    const bool flag = listened != 0;
    call_slot(box_of<B>(ctx), [](slot* s, const void* arg) { static_cast<B*>(s)->fn(*static_cast<const bool*>(arg)); }, &flag);
}

template <class B>
void call_timer(void* ctx) {
    call_slot(box_of<B>(ctx), [](slot* s, const void*) { static_cast<B*>(s)->fn(); }, nullptr);
}

template <class T>
void drop_box(void* ctx) {
    delete static_cast<T*>(ctx);
}

template <class F>
void run_once(void* ctx) {
    (*static_cast<F*>(ctx))();
}

// Снять: новый вызов не начнётся; с чужого потока — и идущий уже кончился.
inline void cancel(slot& s) {
    s.alive.store(false, std::memory_order_relaxed);
    if (!on_script_thread()) {
        wait_idle(s);
    }
    if (const std::uint64_t id = s.id.exchange(0)) {
        unsubscribe(id); // после него хост вправе отпустить замыкание
    }
}

// Общее у on<E> и source<E>: объявленное вне функций живёт с плагином, остальное — с объектом.
class binding {
public:
    binding(const binding&)            = delete;
    binding& operator=(const binding&) = delete;

    // Снять сейчас. Объявленное вне функций тоже можно.
    void reset() {
        if (slot_) {
            cancel(*slot_);
            slot_.reset();
        }
    }

    // Подписка стоит: хост принял её или примет на точке входа плагина.
    explicit operator bool() const noexcept { return slot_ && slot_->alive.load(std::memory_order_acquire); }

protected:
    binding() : forever_(!host_bound()) {}

    // Статик, построенный раньше хоста, разрушается на выходе процесса, когда поток скрипта уже
    // убит: снимать нечего и ждать некого.
    ~binding() {
        if (!forever_) {
            reset();
        }
    }

    std::shared_ptr<slot> slot_;

private:
    bool forever_;
};

} // namespace detail

// Подписка на событие E. Обработчик — с событием или без аргументов:
//   graft::on<graft::running>        started{&start};
//   graft::on<graft::players_online> online{[](const graft::players_online& p) { ... }};
template <event E>
class on : public detail::binding {
public:
    template <class F>
        requires std::invocable<std::decay_t<F>&, const E&> || std::invocable<std::decay_t<F>&>
    on(F&& handler, std::source_location at = std::source_location::current()) {
        static_assert(detail::name_is_literal<E>(), "имя события — строковый литерал: E::name = \"...\"");
        using box_t     = detail::boxed<std::decay_t<F>>;
        auto       box  = std::make_shared<box_t>(std::forward<F>(handler));
        const auto here = detail::where_of(at);
        slot_           = box;
        detail::subscribe(E::name.data(), static_cast<std::uint32_t>(sizeof(E)), &detail::call_handler<E, box_t>, &detail::drop_holder, new detail::holder(std::move(box)), here.text, slot_);
    }
};

// Источник события E: changed(true) — у темы появился подписчик, changed(false) — ушёл последний.
// Так дорогое (опрос движка, свой таймер) начинается только тогда, когда его кто-то ждёт.
//   graft::source<zones_changed> feed{[](bool wanted) { ... }};
template <event E>
class source : public detail::binding {
public:
    template <class F>
        requires std::invocable<std::decay_t<F>&, bool>
    source(F&& changed, std::source_location at = std::source_location::current()) {
        using box_t     = detail::boxed<std::decay_t<F>>;
        auto       box  = std::make_shared<box_t>(std::forward<F>(changed));
        const auto here = detail::where_of(at);
        slot_           = box;
        detail::watch(E::name.data(), static_cast<std::uint32_t>(sizeof(E)), &detail::call_watcher<box_t>, &detail::drop_holder, new detail::holder(std::move(box)), here.text, slot_);
    }
};

// Таймер. Сам объект ничего не держит: выбросили — таймер идёт дальше.
class timer {
public:
    timer() = default;

    explicit timer(std::weak_ptr<detail::slot> s) noexcept : slot_(std::move(s)) {}

    // Снять. С чужого потока — дождавшись вызова, который уже идёт.
    void cancel() {
        if (const auto s = slot_.lock()) {
            detail::cancel(*s);
        }
        slot_.reset();
    }

    // Ещё стоит: не снят и (разовый) не сработал.
    explicit operator bool() const noexcept {
        const auto s = slot_.lock();
        return s && s->alive.load(std::memory_order_acquire);
    }

private:
    std::weak_ptr<detail::slot> slot_;
};

// Есть ли у хоста события (хост 0.6 и новее).
inline bool events_available() {
    return detail::events_ready();
}

// Это поток скрипта: кадр, нативы, обработчики событий.
inline bool on_script_thread() {
    return detail::on_script_thread();
}

// Кадров движка с запуска; из любого потока. FPS — разницей раз в секунду, без graft::tick.
// 0 — хост старше.
inline std::uint64_t frames() {
    return detail::frames();
}

// Ждёт ли E кто-нибудь. Для источника, которому событие дорого собрать.
template <event E>
bool wanted() {
    const graft_topic* t = detail::topic_of<E>();
    return t && detail::listeners(*t) != 0;
}

// Опубликовать. С потока скрипта — подписчикам сразу; с другого — на ближайшем кадре. Подписчиков
// нет — ничего не делает. Ложь — хост отказал: размер темы другой или хост старше 0.6.
template <event E>
bool publish(const E& e) {
    const graft_topic* t = detail::topic_of<E>();
    if (!t) {
        return false;
    }
    return detail::listeners(*t) == 0 || detail::publish(t, &e, false);
}

// Опубликовать и запомнить: опоздавший подписчик получит последнее значение.
template <event E>
bool publish_state(const E& e) {
    const graft_topic* t = detail::topic_of<E>();
    return t && detail::publish(t, &e, true);
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
    return ms <= 0 ? 0u : ms >= 0xFFFFFFFFll ? 0xFFFFFFFFu
                                             : static_cast<std::uint32_t>(ms);
}

template <class F>
timer start_timer(std::uint32_t first, std::uint32_t every, F&& fn, std::source_location at) {
    using box_t                = boxed<std::decay_t<F>>;
    auto                  box  = std::make_shared<box_t>(std::forward<F>(fn));
    const auto            here = where_of(at);
    std::shared_ptr<slot> s    = box;
    add_timer(first, every, &call_timer<box_t>, &drop_holder, new holder(std::move(box)), here.text, s);
    return timer{s};
}

} // namespace detail

// Один раз через delay — на потоке скрипта. Точность — кадр.
template <class Rep, class Period, class F>
    requires std::invocable<std::decay_t<F>&>
timer after(std::chrono::duration<Rep, Period> delay, F&& fn, std::source_location at = std::source_location::current()) {
    return detail::start_timer(detail::as_ms(delay), 0, std::forward<F>(fn), at);
}

// Каждые period — на потоке скрипта. Пропущенное за простой не догоняется пачкой.
template <class Rep, class Period, class F>
    requires std::invocable<std::decay_t<F>&>
timer every(std::chrono::duration<Rep, Period> period, F&& fn, std::source_location at = std::source_location::current()) {
    const std::uint32_t ms = detail::as_ms(period) == 0 ? 1u : detail::as_ms(period);
    return detail::start_timer(ms, ms, std::forward<F>(fn), at);
}

} // namespace graft
