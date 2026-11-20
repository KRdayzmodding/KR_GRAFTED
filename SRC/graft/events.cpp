// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include "graft/hub.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <string>

#include "graft/engine.hpp"
#include "graft/events.hpp"
#include "graft/guard.hpp"
#include "graft/loader.hpp"
#include "graft/plugins.hpp"

// Хаб событий хоста. Что обещано плагину — graft/events.hpp; устройство — graft/hub.hpp.
namespace graft::events {

void closure::release() {
    if (drop_) {
        const auto drop = std::exchange(drop_, nullptr);
        void*      ctx  = std::exchange(ctx_, nullptr);
        // drop — код плагина (обычно delete): ловушка та же, что у вызова, — упасть тут значит
        // уронить кадр движка.
        ::graft::detail::guarded<void>(reinterpret_cast<void*>(drop), [drop, ctx] { drop(ctx); });
    }
    ctx_ = nullptr;
}

hub::hub(now_fn now) : now_(now) {}

hub::~hub() = default;

void hub::bind_here() {
    thread_.store(GetCurrentThreadId(), std::memory_order_release);
}

bool hub::bound() const {
    return thread_.load(std::memory_order_acquire) != 0;
}

bool hub::on_thread() const {
    const std::uint32_t bound = thread_.load(std::memory_order_acquire);
    return bound != 0 && bound == GetCurrentThreadId();
}

void hub::enqueue(task t) {
    const std::lock_guard held{queue_lock_};
    queue_.push_back(std::move(t));
}

bool hub::post(void (*run)(void*), void (*drop)(void*), void* ctx, std::string_view where) {
    if (!run) {
        closure{drop, ctx}.release();
        return false;
    }
    enqueue(task{[run, box = closure{drop, ctx}]() mutable { run(box.ctx()); }, reinterpret_cast<void*>(run), std::string(where)});
    return true;
}

bool hub::post_own(std::move_only_function<void()> fn, std::string_view where) {
    enqueue(task{std::move(fn), nullptr, std::string(where)});
    return true;
}

// Отказ — сразу и громко: обоих по месту, иначе непонятно, кого из двух пересобирать.
bool hub::size_fits(std::string_view topic, std::uint32_t size, std::string_view where) {
    const std::lock_guard held{sizes_lock_};
    const auto            found = sizes_.find(topic);
    if (found == sizes_.end()) {
        sizes_.emplace(std::string(topic), std::pair{size, std::string(where)});
        return true;
    }
    if (found->second.first == size) {
        return true;
    }
    graft::log(std::format("! событие «{}»: размер {} у {} — тема уже {} ({}); отказано", topic, size, where, found->second.first, found->second.second));
    return false;
}

hub::topic& hub::topic_of(std::string_view name) {
    auto found = topics_.find(name);
    if (found == topics_.end()) {
        found = topics_.emplace(std::string(name), std::make_unique<topic>()).first;
    }
    return *found->second;
}

std::uint64_t hub::subscribe(std::string_view topic, std::uint32_t size, void (*handler)(void*, const void*), void (*drop)(void*), void* ctx, std::string_view where) {
    closure box{drop, ctx};
    if (!handler || !size_fits(topic, size, where)) {
        return 0; // box отпустит замыкание здесь же — плагин его уже не держит
    }
    const std::uint64_t id = next_.fetch_add(1, std::memory_order_relaxed);
    if (on_thread()) {
        add(topic, id, handler, std::move(box), std::string(where));
    } else {
        post_own([this, name = std::string(topic), id, handler, box = std::move(box), w = std::string(where)]() mutable {
            add(name, id, handler, std::move(box), std::move(w));
        },
                 "graft: подписка");
    }
    return id;
}

void hub::add(std::string_view name, std::uint64_t id, void (*handler)(void*, const void*), closure box, std::string where) {
    topic& t = topic_of(name);
    t.subs.push_back(std::make_unique<sub>(sub{id, handler, std::move(box), std::move(where), false}));
    topic_of_sub_[id] = &t;
    if (t.has_last) {
        // Не внутри подписки: подписчик часто — поле объекта, который ещё строится.
        post_own([this, id] { deliver_last(id); }, "graft: состояние опоздавшему");
    }
}

void hub::unsubscribe(std::uint64_t id) {
    if (id == 0) {
        return;
    }
    if (on_thread()) {
        remove(id);
    } else {
        post_own([this, id] { remove(id); }, "graft: снятие подписки");
    }
}

void hub::remove(std::uint64_t id) {
    if (const auto found = topic_of_sub_.find(id); found != topic_of_sub_.end()) {
        topic& t = *found->second;
        topic_of_sub_.erase(found);
        const auto at = std::ranges::find(t.subs, id, [](const auto& s) { return s->id; });
        if (at != t.subs.end()) {
            if (t.delivering > 0) {
                (*at)->dead = true; // идёт раздача: замыкание отпустим после неё
                t.dirty     = true;
            } else {
                t.subs.erase(at);
            }
        }
        return;
    }
    if (running_timer_ == id) {
        running_cancelled_ = true;
        return;
    }
    timers_.erase(id);
}

bool hub::publish(std::string_view topic, std::uint32_t size, const void* event, bool state) {
    if (!event || !size_fits(topic, size, "publish")) {
        return false;
    }
    if (on_thread()) {
        publish_here(topic, event, size, state);
        return true;
    }
    std::vector<std::byte> copy(size);
    std::memcpy(copy.data(), event, size);
    return post_own([this, name = std::string(topic), copy = std::move(copy), state] {
        publish_here(name, copy.data(), static_cast<std::uint32_t>(copy.size()), state);
    },
                    "graft: событие с чужого потока");
}

void hub::publish_here(std::string_view name, const void* event, std::uint32_t size, bool state) {
    topic& t = topic_of(name);
    if (state) {
        t.last.resize(size);
        std::memcpy(t.last.data(), event, size);
        t.has_last = true;
    }
    deliver(t, event);
}

void hub::deliver(topic& t, const void* event) {
    ++t.delivering;
    // По номеру, а не итератором: обработчик может подписать нового (вектор растёт). Новый
    // текущего события не получает — число берём до раздачи.
    const std::size_t count = t.subs.size();
    for (std::size_t i = 0; i < count; ++i) {
        sub* s = t.subs[i].get();
        if (s->dead) {
            continue;
        }
        const auto handler = s->handler;
        void*      ctx     = s->box.ctx();
        call(reinterpret_cast<void*>(handler), s->where, [handler, ctx, event] { handler(ctx, event); });
    }
    if (--t.delivering == 0 && t.dirty) {
        std::erase_if(t.subs, [](const auto& s) { return s->dead; });
        t.dirty = false;
    }
}

void hub::deliver_last(std::uint64_t id) {
    const auto found = topic_of_sub_.find(id);
    if (found == topic_of_sub_.end() || !found->second->has_last) {
        return;
    }
    topic&     t  = *found->second;
    const auto at = std::ranges::find(t.subs, id, [](const auto& s) { return s->id; });
    if (at == t.subs.end() || (*at)->dead) {
        return;
    }
    ++t.delivering;
    const auto  handler = (*at)->handler;
    void*       ctx     = (*at)->box.ctx();
    const void* event   = t.last.data();
    call(reinterpret_cast<void*>(handler), (*at)->where, [handler, ctx, event] { handler(ctx, event); });
    if (--t.delivering == 0 && t.dirty) {
        std::erase_if(t.subs, [](const auto& s) { return s->dead; });
        t.dirty = false;
    }
}

std::uint64_t hub::timer(std::chrono::milliseconds first, std::chrono::milliseconds every, void (*run)(void*), void (*drop)(void*), void* ctx, std::string_view where) {
    closure box{drop, ctx};
    if (!run) {
        return 0;
    }
    const std::uint64_t id = next_.fetch_add(1, std::memory_order_relaxed);
    if (on_thread()) {
        add_timer(id, first, every, run, std::move(box), std::string(where));
    } else {
        post_own([this, id, first, every, run, box = std::move(box), w = std::string(where)]() mutable {
            add_timer(id, first, every, run, std::move(box), std::move(w));
        },
                 "graft: таймер");
    }
    return id;
}

void hub::add_timer(std::uint64_t id, clock::duration first, clock::duration every, void (*run)(void*), closure box, std::string where) {
    timers_.emplace(id, std::make_unique<timer_entry>(timer_entry{every, run, std::move(box), std::move(where)}));
    due_.emplace(now_() + first, id);
}

void hub::run_timers(clock::time_point now) {
    while (!due_.empty() && due_.top().first <= now) {
        const auto [at, id] = due_.top();
        due_.pop();
        const auto found = timers_.find(id);
        if (found == timers_.end()) {
            continue; // снят
        }
        timer_entry& t     = *found->second;
        running_timer_     = id;
        running_cancelled_ = false;
        const auto run     = t.run;
        void*      ctx     = t.box.ctx();
        call(reinterpret_cast<void*>(run), t.where, [run, ctx] { run(ctx); });
        running_timer_ = 0;
        if (running_cancelled_ || t.every == clock::duration::zero()) {
            timers_.erase(id);
            continue;
        }
        // Простой не догоняется пачкой: следующий срок — от сейчас, если старый уже прошёл.
        clock::time_point next = at + t.every;
        if (next <= now) {
            next = now + t.every;
        }
        due_.emplace(next, id);
    }
}

void hub::drain(clock::duration budget) {
    const clock::time_point started = now_();
    std::size_t             count   = 0;
    {
        const std::lock_guard held{queue_lock_};
        count = queue_.size(); // поставленное по ходу — на следующий кадр
    }
    for (std::size_t i = 0; i < count; ++i) {
        task t;
        {
            const std::lock_guard held{queue_lock_};
            if (queue_.empty()) {
                return;
            }
            t = std::move(queue_.front());
            queue_.pop_front();
        }
        call(t.code, t.where, [&t] { t.fn(); });
        if (now_() - started >= budget) {
            return;
        }
    }
}

void hub::frame(clock::duration budget) {
    run_timers(now_());
    drain(budget);
}

// Чужой код — под ловушкой; при сбое журнал называет DLL по адресу кода и место по where.
template <class F>
void hub::call(void* code, const std::string& where, F body) const {
    const loader::fault_place place{code, where.c_str()};
    ::graft::detail::guarded<void>(code ? code : reinterpret_cast<void*>(&events::host), [&body] { body(); });
}

std::size_t hub::queued() const {
    const std::lock_guard held{queue_lock_};
    return queue_.size();
}

std::size_t hub::subscribers(std::string_view name) const {
    const auto found = topics_.find(name);
    if (found == topics_.end()) {
        return 0;
    }
    return static_cast<std::size_t>(std::ranges::count_if(found->second->subs, [](const auto& s) { return !s->dead; }));
}

namespace {
hub* g_active = nullptr;
}

hub& host() {
    static hub* const the = new hub; // не разрушается: объекты движка умирают и после статиков
    return *the;
}

hub& active() {
    return g_active ? *g_active : host();
}

scoped::scoped(hub& h) : was_(g_active) {
    g_active = &h;
}

scoped::~scoped() {
    g_active = was_;
}

clock::duration frame_budget() {
    static const clock::duration budget =
        std::chrono::milliseconds{plugins::frame_budget_ms(GetCommandLineA())};
    return budget;
}

void frame(float dt, std::uint64_t number) {
    hub& h = active();
    h.bind_here();
    const graft::tick t{dt, number};
    h.publish(graft::tick::name, sizeof t, &t, false);
    h.frame(frame_budget());
}

} // namespace graft::events

// ── Мост events.hpp на стороне хоста: хост сам себе хост ───────────────────────
namespace graft::detail {

bool events_ready() {
    return true;
}

bool on_script_thread() {
    return events::active().on_thread();
}

bool script_thread_known() {
    return events::active().bound();
}

bool post(void (*run)(void*), void (*drop)(void*), void* ctx, const char* where) {
    return events::active().post(run, drop, ctx, where ? where : "");
}

std::uint64_t subscribe(const char* topic, std::uint32_t size, void (*handler)(void*, const void*), void (*drop)(void*), void* ctx, const char* where) {
    return events::active().subscribe(topic ? topic : "", size, handler, drop, ctx, where ? where : "");
}

void unsubscribe(std::uint64_t id) {
    events::active().unsubscribe(id);
}

bool publish(const char* topic, std::uint32_t size, const void* event, bool state) {
    return events::active().publish(topic ? topic : "", size, event, state);
}

std::uint64_t add_timer(std::uint32_t first_ms, std::uint32_t every_ms, void (*run)(void*), void (*drop)(void*), void* ctx, const char* where) {
    return events::active().timer(std::chrono::milliseconds{first_ms}, std::chrono::milliseconds{every_ms}, run, drop, ctx, where ? where : "");
}

} // namespace graft::detail
