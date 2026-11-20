// Второй плагин фикстуры: шаблонный скриптовый класс, всё хранилище которого живёт в
// C++. Он здесь не ради хеш-таблицы, а ради ДВУХ плагинов в одном процессе — сьюта
// зовёт нативы обоих, поэтому «два плагина уживаются», «состояние на объект своё у
// каждого» краснеют обычным прогоном.
//
// Пример examples/hashmap показывает то же самое пользователю и живёт своей жизнью:
// у него свой класс (CppHashMap) и своё имя плагина. Имена скриптовых классов у
// движка глобальны на всю установку — поэтому у теста класс СВОЙ, и мод примеров с
// модом тестов могут стоять в одной игре.
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>

#include "graft/events.hpp"
#include "graft/native.hpp"

GRAFT_PLUGIN("SIXW_HASHMAP", 1);

namespace {

template <class K, class V>
struct Table {
    std::unordered_map<K, V> data;

    void Set(K key, V value) { data.insert_or_assign(std::move(key), std::move(value)); }
    V Get(K key) const {
        const auto it = data.find(key);
        return it == data.end() ? V{} : it->second;
    }
    bool Contains(K key) const { return data.contains(key); }
    bool Remove(K key) { return data.erase(key) != 0; }
    int Count() const { return static_cast<int>(data.size()); }
    void Clear() { data.clear(); }

    // Ключи отдаются по одному: заполнять array<K> из C++ нельзя — за строками и
    // ссылками движок считает ссылки. Порядок у хеш-таблицы не определён, но между
    // вызовами не меняется, поэтому обход по индексу согласован сам с собой.
    K KeyAt(int index) const {
        if (index < 0 || std::cmp_greater_equal(index, data.size())) {
            return K{};
        }
        return std::next(data.begin(), index)->first;
    }
};

GRAFT_BINDINGS("1_Core") {
    bind.template_class<Table>("SeraphHashMap<Class K, Class V>")
        .method(GRAFT_METHOD(Set))
        .method(GRAFT_METHOD(Get))
        .method(GRAFT_METHOD(Contains))
        .method(GRAFT_METHOD(Remove))
        .method(GRAFT_METHOD(Count))
        .method(GRAFT_METHOD(Clear))
        .method(GRAFT_METHOD(KeyAt));
}

// Сторож GRAFT_ON_LOAD: plugin_entry_test ждёт эту строку в журнале хоста — она доходит
// туда, только если обработчик позван ПОСЛЕ знакомства с хостом.
GRAFT_ON_LOAD() {
    graft::log("fixture: on_load");
}

}  // namespace

// Сторож совместимости 0.6: плагин с событиями на хосте без них не падает и не течёт —
// post отвечает «нет», а замыкание отпущено. 1 — события есть, 0 — нет и отпущено, -1 — утекло.
namespace {
int g_dropped = 0;

struct probe_mark {
    bool armed = true;
    probe_mark() = default;
    probe_mark(probe_mark&& other) noexcept : armed(std::exchange(other.armed, false)) {}
    probe_mark(const probe_mark&) = delete;
    ~probe_mark() {
        if (armed) {
            ++g_dropped;
        }
    }
};
}  // namespace

extern "C" __declspec(dllexport) int fixture_events_probe() {
    if (graft::events_available()) {
        return 1;
    }
    const int before  = g_dropped;
    const bool posted = graft::post([mark = probe_mark{}] {});
    return !posted && g_dropped == before + 1 ? 0 : -1;
}
