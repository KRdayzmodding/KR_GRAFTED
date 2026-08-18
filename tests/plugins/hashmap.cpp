// Второй плагин фикстуры: шаблонный скриптовый класс, всё хранилище которого живёт в
// C++. Он здесь не ради хеш-таблицы, а ради ДВУХ плагинов в одном процессе — сьюта
// зовёт нативы обоих, поэтому «два плагина уживаются», «состояние на объект своё у
// каждого» и «NativeDispose_<ПЛАГИН> не спорят» краснеют обычным прогоном.
//
// Пример examples/hashmap показывает то же самое пользователю и живёт своей жизнью:
// у него свой класс (CppHashMap) и своё имя плагина. Имена скриптовых классов у
// движка глобальны на всю установку — поэтому у теста класс СВОЙ, и мод примеров с
// модом тестов могут стоять в одной игре.
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>

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

}  // namespace
