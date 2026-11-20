// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "graft/abi.h"

// Хаб событий — ТОЛЬКО ХОСТ. Плагины видят его через graft_host_api (поля «С 0.6») и
// graft/events.hpp; что обещано им, записано там.
//
// Устройство в одну строку: подписчиков, источники и таймеры трогает только поток скрипта,
// поэтому замков на них нет; с других потоков в хаб входит одна очередь под мьютексом — вызов
// с чужого потока превращается в задание этой очереди. Темы — под своим замком: их заводят с
// любого потока (издатель спрашивает тему один раз), и отказ по размеру обязан прийти сразу, в
// том же вызове, иначе плагин держал бы указатель на уже отпущенное замыкание. Сколько у темы
// подписчиков, издатель читает без хаба — счётчиком в самой теме.
namespace graft::events {

using clock = std::chrono::steady_clock;

// Замыкание плагина: память плагина, отпустить её может только его же drop.
class closure {
public:
    closure() = default;

    closure(void (*drop)(void*), void* ctx) : drop_(drop), ctx_(ctx) {}

    closure(closure&& other) noexcept
        : drop_(std::exchange(other.drop_, nullptr)), ctx_(std::exchange(other.ctx_, nullptr)) {}

    closure& operator=(closure&& other) noexcept {
        if (this != &other) {
            release();
            drop_ = std::exchange(other.drop_, nullptr);
            ctx_  = std::exchange(other.ctx_, nullptr);
        }
        return *this;
    }

    closure(const closure&)            = delete;
    closure& operator=(const closure&) = delete;

    ~closure() { release(); }

    void* ctx() const { return ctx_; }

    void release();

private:
    void (*drop_)(void*) = nullptr;
    void* ctx_           = nullptr;
};

class hub {
public:
    using now_fn = clock::time_point (*)();

    explicit hub(now_fn now = &clock::now);
    hub(const hub&)            = delete;
    hub& operator=(const hub&) = delete;
    ~hub();

    // Поток скрипта — этот. Хост зовёт с кадра; до первого кадра потока скрипта нет,
    // и всё, что приходит, ждёт в очереди.
    void bind_here();
    bool on_thread() const;
    bool bound() const;

    // Свой у каждого хаба: по нему издатели узнают, что их тема — от другого хаба (тесты).
    std::uint64_t serial() const { return serial_; }

    bool post(void (*run)(void*), void (*drop)(void*), void* ctx, std::string_view where);
    // Своё задание хоста: без замыкания плагина.
    bool post_own(std::move_only_function<void()> fn, std::string_view where);

    // Тема по имени, с любого потока. Размер не тот — пусто и строка в журнале.
    const graft_topic* topic(std::string_view name, std::uint32_t size, std::string_view where);

    std::uint64_t subscribe(std::string_view topic, std::uint32_t size, void (*handler)(void*, const void*), void (*drop)(void*), void* ctx, std::string_view where);
    // Источник темы: changed(true) — подписчик появился, (false) — ушёл последний; на потоке скрипта.
    std::uint64_t watch(std::string_view topic, std::uint32_t size, void (*changed)(void*, std::uint8_t), void (*drop)(void*), void* ctx, std::string_view where);
    std::uint64_t watch_own(std::string_view topic, std::uint32_t size, std::move_only_function<void(bool)> changed, std::string_view where);
    void          unsubscribe(std::uint64_t id);
    bool          publish(std::string_view topic, std::uint32_t size, const void* event, bool state);
    bool          publish(const graft_topic* topic, const void* event, bool state);
    std::uint64_t timer(std::chrono::milliseconds first, std::chrono::milliseconds every, void (*run)(void*), void (*drop)(void*), void* ctx, std::string_view where);
    std::uint64_t timer_own(std::chrono::milliseconds first, std::chrono::milliseconds every, std::move_only_function<void()> fn, std::string_view where);

    // Кадр: graft::tick (если его ждут), сработавшие таймеры, затем очередь в пределах budget.
    void tick(float dt, std::uint64_t number);
    void frame(clock::duration budget);
    // То же, но часы — только когда по сумме dt кадров ближайший таймер уже мог подойти (или раз в 64 кадра):
    // кадр с таймерами, которым не пора, не читает QPC.
    void frame(clock::duration budget, clock::duration dt);

    std::size_t queued() const;
    std::size_t subscribers(std::string_view topic) const;
    std::size_t timers() const;

private:
    struct sub {
        std::uint64_t id                    = 0;
        void (*handler)(void*, const void*) = nullptr;
        closure     box;
        std::string where;
        bool        dead = false;
    };

    struct watcher {
        std::uint64_t                       id = 0;
        std::move_only_function<void(bool)> changed;
        void*                               code = nullptr; // чей код: по адресу хост называет DLL
        std::string                         where;
        bool                                told = false; // что ему сказано последним
        bool                                dead = false;
    };

    struct topic_rec : graft_topic {
        std::string                           name;
        std::string                           where; // кто завёл: в отказе по размеру называют обоих
        std::vector<std::unique_ptr<sub>>     subs;
        std::vector<std::unique_ptr<watcher>> watchers;
        std::vector<std::byte>                last;
        bool                                  has_last   = false;
        int                                   delivering = 0;
        bool                                  dirty      = false;
        bool                                  telling    = false; // задание «сказать источникам» уже стоит
    };

    struct timer_entry {
        clock::duration                 every{};
        std::move_only_function<void()> fn;
        void*                           code = nullptr;
        std::string                     where;
    };

    struct task {
        std::move_only_function<void()> fn;
        void*                           code = nullptr;
        std::string                     where;
    };

    struct link {
        topic_rec* topic   = nullptr;
        bool       watcher = false;
    };

    struct text_hash {
        using is_transparent = void;

        std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    };

    topic_rec*    find_or_add(std::string_view name, std::uint32_t size, std::string_view where);
    void          add(topic_rec& t, std::uint64_t id, void (*handler)(void*, const void*), closure box, std::string where);
    void          add_watcher(topic_rec& t, std::unique_ptr<watcher> w);
    std::uint64_t watch_on(topic_rec& t, std::move_only_function<void(bool)> changed, void* code, std::string_view where);
    void          remove(std::uint64_t id);
    void          count(topic_rec& t, int delta);
    void          tell_later(topic_rec& t);
    void          tell(topic_rec& t);
    void          publish_here(topic_rec& t, const void* event, bool state);
    void          deliver(topic_rec& t, const void* event);
    void          deliver_last(std::uint64_t id);
    void          sweep(topic_rec& t);
    void          add_timer(std::uint64_t id, clock::duration first, std::unique_ptr<timer_entry> entry);
    std::uint64_t start_timer(clock::duration first, std::unique_ptr<timer_entry> entry);
    void          run_timers(clock::time_point now);
    void          drain(clock::duration budget);
    void          enqueue(task t);
    template <class F>
    void call(void* code, const std::string& where, F body) const;

    now_fn                     now_;
    std::uint64_t              serial_;
    std::atomic<std::uint32_t> thread_{0};
    std::atomic<std::uint64_t> next_{1};

    mutable std::mutex       queue_lock_;
    std::deque<task>         queue_;
    std::atomic<std::size_t> waiting_{0}; // заданий в очереди: пустой кадр не берёт замок

    mutable std::mutex                                                                      topics_lock_;
    std::unordered_map<std::string, std::unique_ptr<topic_rec>, text_hash, std::equal_to<>> topics_;
    topic_rec*                                                                              tick_ = nullptr;

    // Дальше — только поток скрипта.
    std::unordered_map<std::uint64_t, link>                         links_;
    std::unordered_map<std::uint64_t, std::unique_ptr<timer_entry>> timers_;
    std::priority_queue<std::pair<clock::time_point, std::uint64_t>, std::vector<std::pair<clock::time_point, std::uint64_t>>, std::greater<>>
                  due_;
    std::uint64_t running_timer_     = 0;
    bool          running_cancelled_ = false;
    // Оценка «сейчас» без часов: последнее чтение часов и сумма dt кадров после него.
    clock::time_point clock_at_{};
    clock::duration   since_clock_{};
    std::uint32_t     frames_since_clock_ = 0;
};

// Хаб хоста: живёт до конца процесса (объекты движка умирают и после статиков).
hub& host();
// Хаб, которым пользуются graft::on/publish/post на стороне хоста. Тестам — подменить своим.
hub& active();

class scoped {
public:
    explicit scoped(hub& h);
    ~scoped();
    scoped(const scoped&)            = delete;
    scoped& operator=(const scoped&) = delete;

private:
    hub* was_;
};

// Кадр хоста: поток скрипта, событие graft::tick, таймеры, очередь. Зовут врезка кадра и GraftTick.
void frame(float dt, std::uint64_t number);
// Бюджет очереди за кадр: ключ -graftBudget=<мс>, по умолчанию 2 мс.
clock::duration frame_budget();

} // namespace graft::events
