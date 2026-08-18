#pragma once
#include <cstring>

#include "graft/enf_type.hpp"

// Скриптовые контейнеры: array<T>, set<T>, map<K,V> и модификатор out.
//
// Это ВЬЮХИ на память движка, а не контейнеры: данные принадлежат ему, копии не
// происходит. Размер меняется его же методами — так работает и для array<string>, и на
// рост за пределы ёмкости, потому что аллокатор и подсчёт ссылок остаются движковыми.
namespace graft {

// Типизированная вьюха на скриптовый array<T>/set<T> — сам объект остаётся в скрипте.
// Размер менять нельзя (память принадлежит аллокатору движка): Resize/Insert делай
// Размер меняется движковыми методами (resize/clear/remove/sort) — память выделяет и
// освобождает сам движок, поэтому работает и для array<string>, и на рост за пределы
// ёмкости. Прямая запись элементов (set) — только int/float.
template <class T, name_t Kind>
struct container_view {
    static_assert(is_element<T>, "такой тип элемента не поддерживается: размер в скрипте неизвестен");
    void* ptr = nullptr;

    explicit operator bool() const { return ptr != nullptr; }
    T* data() const {
        return ptr ? *reinterpret_cast<T* const*>(at(layout::array_data)) : nullptr;
    }
    i32 size() const { return ptr ? *reinterpret_cast<const i32*>(at(layout::array_count)) : 0; }
    i32 capacity() const {
        return ptr ? *reinterpret_cast<const i32*>(at(layout::array_capacity)) : 0;
    }
    T operator[](i32 i) const {
        return element_access<T>::load(reinterpret_cast<const char*>(data()) +
                                       static_cast<std::size_t>(i) * sizeof(T));
    }

    // Писать можно только то, чем не владеет движок. За строками и ссылками он считает
    // ссылки: положив туда свой указатель, мы заставим его освобождать чужую память —
    // это порча кучи, проверено падением 0xC0000374. Такие элементы отдаём наружу
    // возвратом `owned string` (движок копирует сам) или заполняем в скрипте.
    void set(i32 i, T value) const
        requires plain_element<T>
    {
        if (i >= 0 && i < size()) {
            data()[i] = value;
        }
    }
    // Итератор отдаёт элемент по значению: ссылочные надо разворачивать из обёртки,
    // поэтому сырым указателем обойтись нельзя. Требования std::input_iterator
    // выполнены, поэтому работают std::ranges-алгоритмы и views.
    class iterator {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = T;

        iterator() = default;
        iterator(const container_view* owner, i32 index) : owner_(owner), index_(index) {}

        T operator*() const { return (*owner_)[index_]; }
        iterator& operator++() {
            ++index_;
            return *this;
        }
        void operator++(int) { ++index_; }
        friend bool operator==(const iterator& a, const iterator& b) {
            return a.index_ == b.index_;
        }

    private:
        const container_view* owner_ = nullptr;
        i32 index_ = 0;
    };
    iterator begin() const { return {this, 0}; }
    iterator end() const { return {this, size()}; }

    // Меняет размер настоящим движковым Resize: он же выделяет память и следит за
    // временем жизни элементов, поэтому работает и для строк со ссылками, и на рост.
    // `Resize` — не шаблонный метод (аргумент int), поэтому его импл зовётся напрямую.
    bool resize(i32 n) const {
        if (!ptr || n < 0) {
            return false;
        }
        const script::method fn = detail::engine_method<Kind, "Resize">();
        if (!fn.callable()) {
            return false;
        }
        reinterpret_cast<void(__fastcall*)(void*, i32)>(fn.impl)(ptr, n);
        return size() == n;
    }

    bool clear() const {
        const script::method fn = detail::engine_method<Kind, "Clear">();
        if (!ptr || !fn.callable()) {
            return false;
        }
        reinterpret_cast<void(__fastcall*)(void*)>(fn.impl)(ptr);
        return true;
    }

    // Внимание: Reserve/Swap/Copy/Init у контейнеров и map.Remove — НЕ обычные нативы,
    // а маршалируемые (`proto`): у них другое соглашение вызова (rcx=this, rdx=блок
    // аргументов, r8=переменная возврата). Вызов их как C-функции роняет процесс —
    // проверено падением 0xC0000005 на array.Reserve. Поэтому их здесь нет.
    bool remove(i32 index) const {
        const script::method fn = detail::engine_method<Kind, "Remove">();
        if (!ptr || !fn.callable() || index < 0 || index >= size()) {
            return false;
        }
        reinterpret_cast<void(__fastcall*)(void*, i32)>(fn.impl)(ptr, index);
        return true;
    }
    bool sort(bool reverse = false) const {
        const script::method fn = detail::engine_method<Kind, "Sort">();
        if (!ptr || !fn.callable()) {
            return false;
        }
        reinterpret_cast<void(__fastcall*)(void*, bool)>(fn.impl)(ptr, reverse);
        return true;
    }
private:
    const char* at(std::size_t off) const { return static_cast<const char*>(ptr) + off; }
};

template <class T>
using array = container_view<T, "array">;
template <class T>
using set = container_view<T, "set">;

template <class T, name_t Kind>
struct enf_type<container_view<T, Kind>> {
    static constexpr auto id = Kind + name_t{"<"} + enf_type<T>::id + name_t{">"};
    static constexpr const char* name = id.value;
};

// map<K,V>. Узлы лежат по хешу ключа (слот = hash(key) & (capacity-1)), а пустые слоты
// не помечены никак — в них просто мусор от аллокатора (проверено зондом на коллизии:
// ключ 9 при ёмкости 8 ушёл не в слот 1). Значит, обойти карту из C++ нельзя, не
// повторив ровно ту же хеш-функцию движка, — а она поедет на первом же патче молча.
// Поэтому: размер читаем, содержимое берём через скриптовый мост (GetKey/GetElement
// раскладывают карту в два массива, их graft читает полностью). См. README.
template <class T>
inline constexpr std::size_t script_size = 8;  // строка и любая ссылка — указатель
template <>
inline constexpr std::size_t script_size<i32> = 4;
template <>
inline constexpr std::size_t script_size<f32> = 4;

template <class K, class V>
struct map {
    static_assert(is_element<K> && is_element<V>, "такой тип ключа/значения не поддерживается");
    void* ptr = nullptr;
    explicit operator bool() const { return ptr != nullptr; }

    // Count/Clear — не шаблонные методы, поэтому зовём движковые. Запасной путь по
    // раскладке нужен, если поиск по имени вдруг не заработает.
    i32 size() const {
        if (!ptr) {
            return 0;
        }
        const script::method fn = detail::engine_method<"map", "Count">();
        if (fn.callable()) {
            return reinterpret_cast<i32(__fastcall*)(void*)>(fn.impl)(ptr);
        }
        return *reinterpret_cast<const i32*>(static_cast<const char*>(ptr) + layout::map_count);
    }
    bool clear() const {
        const script::method fn = detail::engine_method<"map", "Clear">();
        if (!ptr || !fn.callable()) {
            return false;
        }
        reinterpret_cast<void(__fastcall*)(void*)>(fn.impl)(ptr);
        return true;
    }

    // ── Обход ────────────────────────────────────────────────────────────────
    // Узлы адресуются хешем ключа, но повторять хеш не нужно: `Begin/End/Next` —
    // нативные и нешаблонные, движок сам пропускает пустые слоты, а итератор у него
    // и есть номер слота (проверено: при ёмкости 8 обход дал 1 -> 2 -> 4, End = 8).
    //
    // Раскладка узла {used, key, value} снята с дизассемблера Begin/GetIteratorKey/
    // GetIteratorElement для каждой комбинации типов (re/README.md):
    //   <int,int>              шаг 12, used +0, key +4,  value +8
    //   одинаковые 4-байтные разных типов (int,float) шаг 16, used +4, key +8, value +12
    //   есть 8-байтное поле    шаг 24, used +0 (типы совпадают) или +4, key +8, value +16
    static constexpr bool same_type =
        std::string_view{enf_type<K>::name} == std::string_view{enf_type<V>::name};
    static constexpr bool wide = script_size<K> > 4 || script_size<V> > 4;

    static constexpr std::size_t node_stride = wide ? 24 : (same_type ? 12 : 16);
    static constexpr std::size_t key_offset = wide ? 8 : (same_type ? 4 : 8);
    static constexpr std::size_t value_offset = wide ? 16 : (same_type ? 8 : 12);

    struct pair {
        K key;
        V value;
    };

    class iterator {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = pair;

        iterator() = default;
        iterator(const map* owner, std::int64_t slot) : owner_(owner), slot_(slot) {}

        pair operator*() const { return owner_->at_slot(slot_); }
        iterator& operator++() {
            slot_ = owner_->next_slot(slot_);
            return *this;
        }
        void operator++(int) { slot_ = owner_->next_slot(slot_); }
        friend bool operator==(const iterator& a, const iterator& b) { return a.slot_ == b.slot_; }

    private:
        const map* owner_ = nullptr;
        std::int64_t slot_ = 0;
    };

    iterator begin() const { return {this, call_iter<"Begin">()}; }
    iterator end() const { return {this, call_iter<"End">()}; }

    pair at_slot(std::int64_t slot) const {
        const auto* node =
            static_cast<const char*>(nodes()) + static_cast<std::size_t>(slot) * node_stride;
        return {element_access<K>::load(node + key_offset),
                element_access<V>::load(node + value_offset)};
    }

private:
    const void* nodes() const {
        return *reinterpret_cast<void* const*>(static_cast<const char*>(ptr) + layout::map_nodes);
    }
    template <name_t Name>
    std::int64_t call_iter() const {
        const script::method fn = detail::engine_method<"map", Name>();
        if (!ptr || !fn.callable()) {
            return 0;
        }
        return reinterpret_cast<std::int64_t(__fastcall*)(void*)>(fn.impl)(ptr);
    }
    std::int64_t next_slot(std::int64_t slot) const {
        const script::method fn = detail::engine_method<"map", "Next">();
        if (!ptr || !fn.callable()) {
            return slot;
        }
        return reinterpret_cast<std::int64_t(__fastcall*)(void*, std::int64_t)>(fn.impl)(ptr, slot);
    }
};
template <class K, class V>
struct enf_type<map<K, V>> {
    static constexpr auto id =
        name_t{"map<"} + enf_type<K>::id + name_t{","} + enf_type<V>::id + name_t{">"};
    static constexpr const char* name = id.value;
};

// Контейнер, лежащий полем объекта или элементом другого контейнера, — такая же
// ссылка: разворачиваем его тем же способом.
template <class T, name_t Kind>
struct element_access<container_view<T, Kind>> {
    static container_view<T, Kind> load(const void* slot) {
        void* stored = nullptr;
        std::memcpy(&stored, slot, sizeof stored);
        return {script::deref_object(stored)};
    }
};
template <class K, class V>
struct element_access<map<K, V>> {
    static map<K, V> load(const void* slot) {
        void* stored = nullptr;
        std::memcpy(&stored, slot, sizeof stored);
        return {script::deref_object(stored)};
    }
};

// ── Модификатор объявления: out ─────────────────────────────────────────────
// `out` у `proto native` работает со ссылочными типами — именно так отдаёт коллекции
// сам движок (`proto native void GetPlayers(out array<Man> players)`): ссылка и так
// приходит указателем, маршалинг не нужен. Значения (int/float/string) через out
// умеет только немаршалируемая форма, там компилятор Enforce нативы отвергает.
//
// Наследуемся от T, чтобы вьюха осталась той же по раскладке и по API:
//   void Fill(graft::out<graft::array<graft::str>> dst) { dst.resize(2); dst.set(0, "a"); }
template <class T>
struct out : T {};

template <class T>
struct enf_type<out<T>> {
    static constexpr auto id = name_t{"out "} + enf_type<T>::id;
    static constexpr const char* name = id.value;
};

}  // namespace graft
