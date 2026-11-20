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
#include "graft/frame.hpp"
#include "graft/guard.hpp"
#include "graft/loader.hpp"
#include "graft/plugins.hpp"
#include "graft/profile.hpp"

// Хаб событий хоста. Что обещано плагину — graft/events.hpp; устройство — graft/hub.hpp.
namespace graft::events {
namespace {

std::atomic<std::uint64_t> g_serials{1};

// Счётчик подписчиков читают издатели с любого потока; пишет только поток скрипта.
std::uint32_t listeners_of(const graft_topic& t) {
    return std::atomic_ref<std::uint32_t>(const_cast<std::uint32_t&>(t.listeners)).load(std::memory_order_acquire);
}

void set_listeners(graft_topic& t, std::uint32_t n) {
    std::atomic_ref<std::uint32_t>(t.listeners).store(n, std::memory_order_release);
}

} // namespace

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

hub::hub(now_fn now) : now_(now), serial_(g_serials.fetch_add(1, std::memory_order_relaxed)) {
    tick_ = find_or_add(graft::tick::name, sizeof(graft::tick), "graft");
}

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
    waiting_.fetch_add(1, std::memory_order_release);
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

// ── Темы ─────────────────────────────────────────────────────────────────────

// Отказ — сразу и громко: обоих по месту, иначе непонятно, кого из двух пересобирать.
hub::topic_rec* hub::find_or_add(std::string_view name, std::uint32_t size, std::string_view where) {
    const std::lock_guard held{topics_lock_};
    if (const auto found = topics_.find(name); found != topics_.end()) {
        topic_rec& t = *found->second;
        if (t.size == size) {
            return &t;
        }
        graft::log(std::format("! событие «{}»: размер {} у {} — тема уже {} ({}); отказано", name, size, where, t.size, t.where));
        return nullptr;
    }
    auto t       = std::make_unique<topic_rec>();
    t->size      = size;
    t->listeners = 0;
    t->name      = std::string(name);
    t->where     = std::string(where);
    topic_rec* p = t.get();
    topics_.emplace(std::string(name), std::move(t));
    return p;
}

const graft_topic* hub::topic(std::string_view name, std::uint32_t size, std::string_view where) {
    return find_or_add(name, size, where);
}

// Подписчиков стало ноль или перестало быть ноль — источникам темы скажут на ближайшем кадре: не
// внутри подписки (подписчик часто — поле объекта, который ещё строится) и не по разу на каждую.
void hub::count(topic_rec& t, int delta) {
    const std::uint32_t was = listeners_of(t);
    const std::uint32_t now = static_cast<std::uint32_t>(static_cast<std::int64_t>(was) + delta);
    set_listeners(t, now);
    if ((was == 0) != (now == 0) && !t.watchers.empty()) {
        tell_later(t);
    }
}

void hub::tell_later(topic_rec& t) {
    if (std::exchange(t.telling, true)) {
        return;
    }
    post_own([this, &t] {
        t.telling = false;
        tell(t);
    },
             "graft: источнику темы");
}

// Каждому — только перемена: подписались и тут же ушли — источник не дёргается вовсе.
void hub::tell(topic_rec& t) {
    const bool listened = listeners_of(t) != 0;
    ++t.delivering;
    const std::size_t count = t.watchers.size();
    for (std::size_t i = 0; i < count; ++i) {
        watcher* w = t.watchers[i].get();
        if (w->dead || w->told == listened) {
            continue;
        }
        w->told = listened;
        call(w->code, w->where, [w, listened] { w->changed(listened); });
    }
    sweep(t);
}

void hub::sweep(topic_rec& t) {
    if (--t.delivering == 0 && t.dirty) {
        std::erase_if(t.subs, [](const auto& s) { return s->dead; });
        std::erase_if(t.watchers, [](const auto& w) { return w->dead; });
        t.dirty = false;
    }
}

// ── Подписка ─────────────────────────────────────────────────────────────────

std::uint64_t hub::subscribe(std::string_view topic, std::uint32_t size, void (*handler)(void*, const void*), void (*drop)(void*), void* ctx, std::string_view where) {
    closure    box{drop, ctx};
    topic_rec* t = handler ? find_or_add(topic, size, where) : nullptr;
    if (!t) {
        return 0; // box отпустит замыкание здесь же — плагин его уже не держит
    }
    const std::uint64_t id = next_.fetch_add(1, std::memory_order_relaxed);
    if (on_thread()) {
        add(*t, id, handler, std::move(box), std::string(where));
    } else {
        post_own([this, t, id, handler, box = std::move(box), w = std::string(where)]() mutable { add(*t, id, handler, std::move(box), std::move(w)); },
                 "graft: подписка");
    }
    return id;
}

void hub::add(topic_rec& t, std::uint64_t id, void (*handler)(void*, const void*), closure box, std::string where) {
    t.subs.push_back(std::make_unique<sub>(sub{id, handler, std::move(box), std::move(where), false}));
    links_[id] = link{&t, false};
    count(t, +1);
    if (t.has_last) {
        // Не внутри подписки: подписчик часто — поле объекта, который ещё строится.
        post_own([this, id] { deliver_last(id); }, "graft: состояние опоздавшему");
    }
}

std::uint64_t hub::watch(std::string_view topic, std::uint32_t size, void (*changed)(void*, std::uint8_t), void (*drop)(void*), void* ctx, std::string_view where) {
    closure    box{drop, ctx};
    topic_rec* t = changed ? find_or_add(topic, size, where) : nullptr;
    if (!t) {
        return 0;
    }
    return watch_on(*t, [changed, box = std::move(box)](bool listened) { changed(box.ctx(), listened ? 1u : 0u); }, reinterpret_cast<void*>(changed), where);
}

std::uint64_t hub::watch_own(std::string_view topic, std::uint32_t size, std::move_only_function<void(bool)> changed, std::string_view where) {
    topic_rec* t = changed ? find_or_add(topic, size, where) : nullptr;
    return t ? watch_on(*t, std::move(changed), nullptr, where) : 0;
}

std::uint64_t hub::watch_on(topic_rec& t, std::move_only_function<void(bool)> changed, void* code, std::string_view where) {
    const std::uint64_t id = next_.fetch_add(1, std::memory_order_relaxed);
    auto                w  = std::make_unique<watcher>(watcher{id, std::move(changed), code, std::string(where)});
    if (on_thread()) {
        add_watcher(t, std::move(w));
    } else {
        post_own([this, &t, w = std::move(w)]() mutable { add_watcher(t, std::move(w)); }, "graft: источник темы");
    }
    return id;
}

void hub::add_watcher(topic_rec& t, std::unique_ptr<watcher> w) {
    links_[w->id] = link{&t, true};
    t.watchers.push_back(std::move(w));
    if (listeners_of(t) != 0) {
        tell_later(t);
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
    if (const auto found = links_.find(id); found != links_.end()) {
        const link l = found->second;
        links_.erase(found);
        topic_rec& t = *l.topic;
        // Идёт раздача: запись помечается, замыкание отпустим после неё.
        const auto drop = [&t](auto& list, std::uint64_t which) {
            const auto at = std::ranges::find(list, which, [](const auto& e) { return e->id; });
            if (at == list.end() || (*at)->dead) {
                return false;
            }
            if (t.delivering > 0) {
                (*at)->dead = true;
                t.dirty     = true;
            } else {
                list.erase(at);
            }
            return true;
        };
        if (l.watcher) {
            drop(t.watchers, id);
        } else if (drop(t.subs, id)) {
            count(t, -1);
        }
        return;
    }
    if (running_timer_ == id) {
        running_cancelled_ = true;
        return;
    }
    timers_.erase(id);
}

// ── Публикация ───────────────────────────────────────────────────────────────

bool hub::publish(std::string_view topic, std::uint32_t size, const void* event, bool state) {
    const topic_rec* t = event ? find_or_add(topic, size, "publish") : nullptr;
    return t && publish(static_cast<const graft_topic*>(t), event, state);
}

bool hub::publish(const graft_topic* topic, const void* event, bool state) {
    if (!topic || !event) {
        return false;
    }
    auto& t = *const_cast<topic_rec*>(static_cast<const topic_rec*>(topic));
    if (!state && listeners_of(t) == 0) {
        return true; // некому: ни копии, ни задания
    }
    if (on_thread()) {
        publish_here(t, event, state);
        return true;
    }
    std::vector<std::byte> copy(t.size);
    std::memcpy(copy.data(), event, t.size);
    return post_own([this, &t, copy = std::move(copy), state] { publish_here(t, copy.data(), state); }, "graft: событие с чужого потока");
}

void hub::publish_here(topic_rec& t, const void* event, bool state) {
    if (state) {
        t.last.resize(t.size);
        std::memcpy(t.last.data(), event, t.size);
        t.has_last = true;
    }
    deliver(t, event);
}

void hub::deliver(topic_rec& t, const void* event) {
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
    sweep(t);
}

void hub::deliver_last(std::uint64_t id) {
    const auto found = links_.find(id);
    if (found == links_.end() || found->second.watcher || !found->second.topic->has_last) {
        return;
    }
    topic_rec& t  = *found->second.topic;
    const auto at = std::ranges::find(t.subs, id, [](const auto& s) { return s->id; });
    if (at == t.subs.end() || (*at)->dead) {
        return;
    }
    ++t.delivering;
    const auto  handler = (*at)->handler;
    void*       ctx     = (*at)->box.ctx();
    const void* event   = t.last.data();
    call(reinterpret_cast<void*>(handler), (*at)->where, [handler, ctx, event] { handler(ctx, event); });
    sweep(t);
}

// ── Таймеры ──────────────────────────────────────────────────────────────────

std::uint64_t hub::timer(std::chrono::milliseconds first, std::chrono::milliseconds every, void (*run)(void*), void (*drop)(void*), void* ctx, std::string_view where) {
    closure box{drop, ctx};
    if (!run) {
        return 0;
    }
    auto e   = std::make_unique<timer_entry>();
    e->every = every;
    e->fn    = [run, box = std::move(box)] { run(box.ctx()); };
    e->code  = reinterpret_cast<void*>(run);
    e->where = std::string(where);
    return start_timer(first, std::move(e));
}

std::uint64_t hub::timer_own(std::chrono::milliseconds first, std::chrono::milliseconds every, std::move_only_function<void()> fn, std::string_view where) {
    if (!fn) {
        return 0;
    }
    auto e   = std::make_unique<timer_entry>();
    e->every = every;
    e->fn    = std::move(fn);
    e->where = std::string(where);
    return start_timer(first, std::move(e));
}

std::uint64_t hub::start_timer(clock::duration first, std::unique_ptr<timer_entry> entry) {
    const std::uint64_t id = next_.fetch_add(1, std::memory_order_relaxed);
    if (on_thread()) {
        add_timer(id, first, std::move(entry));
    } else {
        post_own([this, id, first, e = std::move(entry)]() mutable { add_timer(id, first, std::move(e)); }, "graft: таймер");
    }
    return id;
}

void hub::add_timer(std::uint64_t id, clock::duration first, std::unique_ptr<timer_entry> entry) {
    timers_.emplace(id, std::move(entry));
    clock_at_           = now_();
    since_clock_        = {};
    frames_since_clock_ = 0;
    due_.emplace(clock_at_ + first, id);
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
        call(t.code, t.where, [&t] { t.fn(); });
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

// ── Кадр ─────────────────────────────────────────────────────────────────────

void hub::drain(clock::duration budget) {
    if (waiting_.load(std::memory_order_acquire) == 0) {
        return; // пустой кадр — без замка
    }
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
            waiting_.fetch_sub(1, std::memory_order_release);
        }
        call(t.code, t.where, [&t] { t.fn(); });
        if (now_() - started >= budget) {
            return;
        }
    }
}

void hub::tick(float dt, std::uint64_t number) {
    if (listeners_of(*tick_) != 0) {
        const graft::tick t{dt, number};
        publish_here(*tick_, &t, false);
    }
}

void hub::frame(clock::duration budget) {
    if (!due_.empty()) {
        clock_at_           = now_();
        since_clock_        = {};
        frames_since_clock_ = 0;
        run_timers(clock_at_);
    }
    drain(budget);
}

void hub::frame(clock::duration budget, clock::duration dt) {
    if (!due_.empty()) {
        since_clock_ += dt;
        if (++frames_since_clock_ >= 64 || clock_at_ + since_clock_ >= due_.top().first) {
            frame(budget);
            return;
        }
    }
    drain(budget);
}

// Чужой код — под ловушкой; при сбое журнал называет DLL по адресу кода и место по where.
// С -graftProfile — ещё и цена вызова по месту; без него — одно чтение числа.
template <class F>
void hub::call(void* code, const std::string& where, F body) const {
    const loader::fault_place place{code, where.c_str()};
    void* const               guard = code ? code : reinterpret_cast<void*>(&events::host);
    if (profile::on()) {
        const std::uint64_t t0 = profile::now();
        ::graft::detail::guarded<void>(guard, [&body] { body(); });
        profile::call(code, where, profile::now() - t0);
        return;
    }
    ::graft::detail::guarded<void>(guard, [&body] { body(); });
}

std::size_t hub::queued() const {
    const std::lock_guard held{queue_lock_};
    return queue_.size();
}

std::size_t hub::subscribers(std::string_view name) const {
    const std::lock_guard held{topics_lock_};
    const auto            found = topics_.find(name);
    if (found == topics_.end()) {
        return 0;
    }
    return static_cast<std::size_t>(std::ranges::count_if(found->second->subs, [](const auto& s) { return !s->dead; }));
}

std::size_t hub::timers() const {
    return timers_.size();
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
    h.tick(dt, number);
    h.frame(frame_budget(), std::chrono::duration_cast<clock::duration>(std::chrono::duration<float>(dt)));
}

} // namespace graft::events

// ── Мост events.hpp на стороне хоста: хост сам себе хост ───────────────────────
namespace graft::detail {
namespace {

bool settle(slot& s, std::uint64_t id) {
    if (id != 0) {
        s.id.store(id, std::memory_order_release);
    } else {
        s.alive.store(false, std::memory_order_release);
    }
    return id != 0;
}

} // namespace

bool events_ready() {
    return true;
}

bool host_bound() {
    return true;
}

bool on_script_thread() {
    return events::active().on_thread();
}

bool script_thread_known() {
    return events::active().bound();
}

std::uint64_t epoch() {
    return events::active().serial();
}

std::uint64_t frames() {
    return frame::frames();
}

const graft_topic* topic(const char* name, std::uint32_t size, const char* where) {
    return events::active().topic(name ? name : "", size, where ? where : "");
}

bool post(void (*run)(void*), void (*drop)(void*), void* ctx, const char* where) {
    return events::active().post(run, drop, ctx, where ? where : "");
}

bool subscribe(const char* topic, std::uint32_t size, void (*handler)(void*, const void*), void (*drop)(void*), void* ctx, const char* where, const std::shared_ptr<slot>& s) {
    return settle(*s, events::active().subscribe(topic ? topic : "", size, handler, drop, ctx, where ? where : ""));
}

bool watch(const char* topic, std::uint32_t size, void (*changed)(void*, std::uint8_t), void (*drop)(void*), void* ctx, const char* where, const std::shared_ptr<slot>& s) {
    return settle(*s, events::active().watch(topic ? topic : "", size, changed, drop, ctx, where ? where : ""));
}

void unsubscribe(std::uint64_t id) {
    events::active().unsubscribe(id);
}

bool publish(const graft_topic* topic, const void* event, bool state) {
    return events::active().publish(topic, event, state);
}

bool add_timer(std::uint32_t first_ms, std::uint32_t every_ms, void (*run)(void*), void (*drop)(void*), void* ctx, const char* where, const std::shared_ptr<slot>& s) {
    return settle(*s, events::active().timer(std::chrono::milliseconds{first_ms}, std::chrono::milliseconds{every_ms}, run, drop, ctx, where ? where : ""));
}

} // namespace graft::detail
