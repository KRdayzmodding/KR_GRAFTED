// Хранилище для скриптового шаблона CppHashMap<K,V>.
//
// В C++ это ОБЫЧНЫЙ шаблон обычными типами: ни фасадов, ни спецтипов. Библиотека
// разворачивает его по всем поддерживаемым типам и на каждом вызове выбирает нужную
// инстанциацию по имени скриптового класса объекта ("CppHashMap<string,int>").
// Объявление для PBO печатает генератор — в моде для этого класса нет ни строчки.
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>

#include "graft/native.hpp"

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
    // ponytail: std::next вместо параллельного вектора порядка — таблицы тут небольшие.
    K KeyAt(int index) const {
        if (index < 0 || std::cmp_greater_equal(index, data.size())) {
            return K{};
        }
        return std::next(data.begin(), index)->first;
    }
};

GRAFT_BINDINGS("1_Core") {
    bind.template_class<Table>("CppHashMap<Class K, Class V>")
        .method(GRAFT_METHOD(Set))
        .method(GRAFT_METHOD(Get))
        .method(GRAFT_METHOD(Contains))
        .method(GRAFT_METHOD(Remove))
        .method(GRAFT_METHOD(Count))
        .method(GRAFT_METHOD(Clear))
        .method(GRAFT_METHOD(KeyAt));
}

}  // namespace
