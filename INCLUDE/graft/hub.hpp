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

// Хаб событий — ТОЛЬКО ХОСТ. Плагины видят его через graft_host_api (поля «С 0.6») и
// graft/events.hpp; что обещано им, записано там.
//
// Устройство в одну строку: всё состояние (темы, подписчики, таймеры) трогает только поток
// скрипта, поэтому замков на нём нет; с других потоков в хаб входит одна очередь под мьютексом —
// вызов с чужого потока превращается в задание этой очереди. Размеры тем — отдельно, под своим
// замком: отказ по размеру обязан прийти сразу, в том же вызове, иначе плагин держал бы
// указатель на уже отпущенное замыкание.
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

    bool post(void (*run)(void*), void (*drop)(void*), void* ctx, std::string_view where);
    // Своё задание хоста: без замыкания плагина.
    bool post_own(std::move_only_function<void()> fn, std::string_view where);

    std::uint64_t subscribe(std::string_view topic, std::uint32_t size, void (*handler)(void*, const void*),
                            void (*drop)(void*), void* ctx, std::string_view where);
    void unsubscribe(std::uint64_t id);
    bool publish(std::string_view topic, std::uint32_t size, const void* event, bool state);
    std::uint64_t timer(std::chrono::milliseconds first, std::chrono::milliseconds every, void (*run)(void*),
                        void (*drop)(void*), void* ctx, std::string_view where);

    // Кадр: сработавшие таймеры, затем очередь в пределах budget (хотя бы одно задание).
    void frame(clock::duration budget);

    std::size_t queued() const;
    std::size_t subscribers(std::string_view topic) const;

private:
    struct sub {
        std::uint64_t id = 0;
        void (*handler)(void*, const void*) = nullptr;
        closure box;
        std::string where;
        bool dead = false;
    };
    struct topic {
        std::vector<std::unique_ptr<sub>> subs;
        std::vector<std::byte> last;
        bool has_last  = false;
        int delivering = 0;
        bool dirty     = false;
    };
    struct timer_entry {
        clock::duration every{};
        void (*run)(void*) = nullptr;
        closure box;
        std::string where;
    };
    struct task {
        std::move_only_function<void()> fn;
        void* code = nullptr;  // чей код: по адресу хост называет DLL
        std::string where;
    };
    struct text_hash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    };

    bool size_fits(std::string_view topic, std::uint32_t size, std::string_view where);
    topic& topic_of(std::string_view name);
    void add(std::string_view topic, std::uint64_t id, void (*handler)(void*, const void*), closure box,
             std::string where);
    void remove(std::uint64_t id);
    void publish_here(std::string_view name, const void* event, std::uint32_t size, bool state);
    void deliver(topic& t, const void* event);
    void deliver_last(std::uint64_t id);
    void add_timer(std::uint64_t id, clock::duration first, clock::duration every, void (*run)(void*), closure box,
                   std::string where);
    void run_timers(clock::time_point now);
    void drain(clock::duration budget);
    void enqueue(task t);
    template <class F>
    void call(void* code, const std::string& where, F body) const;

    now_fn now_;
    std::atomic<std::uint32_t> thread_{0};
    std::atomic<std::uint64_t> next_{1};

    mutable std::mutex queue_lock_;
    std::deque<task> queue_;

    std::mutex sizes_lock_;
    std::unordered_map<std::string, std::pair<std::uint32_t, std::string>, text_hash, std::equal_to<>> sizes_;

    // Дальше — только поток скрипта.
    std::unordered_map<std::string, std::unique_ptr<topic>, text_hash, std::equal_to<>> topics_;
    std::unordered_map<std::uint64_t, topic*> topic_of_sub_;
    std::unordered_map<std::uint64_t, std::unique_ptr<timer_entry>> timers_;
    std::priority_queue<std::pair<clock::time_point, std::uint64_t>, std::vector<std::pair<clock::time_point, std::uint64_t>>,
                        std::greater<>>
        due_;
    std::uint64_t running_timer_ = 0;
    bool running_cancelled_      = false;
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

}  // namespace graft::events
