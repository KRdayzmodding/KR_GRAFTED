// Тесты параллели типов C++ <-> Enforce: имена собираются на этапе компиляции, а
// доступ к контейнерам считается по снятой зондом раскладке. Раскладку держим здесь
// на синтетическом объекте — если константы в native.hpp поедут, тест покраснеет
// не дожидаясь запуска игры (в игре её же проверяет сьюта seraph::graft).
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

#include "graft/native.hpp"

namespace {

TEST(Types, ScalarNames) {
    EXPECT_STREQ(graft::enf_type<void>::name, "void");
    EXPECT_STREQ(graft::enf_type<bool>::name, "bool");
    EXPECT_STREQ(graft::enf_type<graft::i32>::name, "int");
    EXPECT_STREQ(graft::enf_type<graft::f32>::name, "float");
    EXPECT_STREQ(graft::enf_type<graft::str>::name, "string");
    EXPECT_STREQ(graft::enf_type<graft::vec3>::name, "vector");
    EXPECT_STREQ(graft::enf_type<graft::owned>::name, "owned string");
    EXPECT_STREQ(graft::enf_type<graft::type>::name, "typename");
}

TEST(Types, ReferenceCarriesClassName) {
    EXPECT_STREQ(graft::enf_type<graft::obj>::name, "Class");
    EXPECT_STREQ(graft::enf_type<graft::ref<"PlayerBase">>::name, "PlayerBase");
}

TEST(Types, ContainerNamesBuiltAtCompileTime) {
    static_assert(graft::enf_type<graft::array<graft::i32>>::id.value[0] == 'a');
    EXPECT_STREQ(graft::enf_type<graft::array<graft::i32>>::name, "array<int>");
    EXPECT_STREQ(graft::enf_type<graft::array<graft::str>>::name, "array<string>");
    EXPECT_STREQ(graft::enf_type<graft::set<graft::f32>>::name, "set<float>");
    EXPECT_STREQ((graft::enf_type<graft::map<graft::str, graft::i32>>::name), "map<string,int>");
    EXPECT_STREQ(graft::enf_type<graft::array<graft::ref<"Man">>>::name, "array<Man>");
}

// Скриптовый array/set: данные по +40, ёмкость по +48, размер по +52.
class FakeArray {
public:
    explicit FakeArray(std::vector<graft::i32> items, graft::i32 capacity = -1)
        : storage_(std::move(items)), object_(72, 0) {
        const graft::i32 count = static_cast<graft::i32>(storage_.size());
        const graft::i32 cap = (capacity < 0) ? count : capacity;
        storage_.resize(static_cast<std::size_t>(cap > count ? cap : count));
        auto* data = storage_.data();
        std::memcpy(object_.data() + graft::layout::array_data, &data, sizeof data);
        std::memcpy(object_.data() + graft::layout::array_capacity, &cap, sizeof cap);
        std::memcpy(object_.data() + graft::layout::array_count, &count, sizeof count);
    }
    void* ptr() { return object_.data(); }
    const std::vector<graft::i32>& items() const { return storage_; }

private:
    std::vector<graft::i32> storage_;
    std::vector<std::uint8_t> object_;
};

TEST(ArrayView, ReadsSizeAndElements) {
    FakeArray fake({1, 2, 39});
    const graft::array<graft::i32> a{fake.ptr()};

    ASSERT_TRUE(static_cast<bool>(a));
    EXPECT_EQ(a.size(), 3);
    EXPECT_GE(a.capacity(), 3);
    EXPECT_EQ(a[0], 1);
    EXPECT_EQ(a[2], 39);

    graft::i32 sum = 0;
    for (graft::i32 v : a) {
        sum += v;
    }
    EXPECT_EQ(sum, 42);
}

TEST(ArrayView, WritesExistingElementsOnly) {
    FakeArray fake({1, 2, 3});
    const graft::array<graft::i32> a{fake.ptr()};

    a.set(0, 100);
    a.set(2, 300);
    a.set(7, 999);  // за пределами размера — игнорируем, память чужая
    EXPECT_EQ(fake.items()[0], 100);
    EXPECT_EQ(fake.items()[2], 300);
}

TEST(ArrayView, NullIsEmpty) {
    const graft::array<graft::i32> a{};
    EXPECT_FALSE(static_cast<bool>(a));
    EXPECT_EQ(a.size(), 0);
    EXPECT_FALSE(a.begin() != a.end());
}

// Размер меняет движковый Resize — вне игры его нет, и вьюха обязана честно отказать,
// а не трогать объект. Рабочий путь проверяет сьюта seraph::graft (Array_Grown*).
TEST(ArrayView, ResizeWithoutEngineIsRefused) {
    FakeArray fake({1, 2}, /*capacity=*/8);
    const graft::array<graft::i32> a{fake.ptr()};

    ASSERT_EQ(a.size(), 2);
    EXPECT_FALSE(a.resize(5));
    EXPECT_EQ(a.size(), 2);
    EXPECT_FALSE(a.resize(-1));
    EXPECT_FALSE(a.clear());
}

TEST(ArrayView, ResizeOnNullIsRefused) {
    const graft::array<graft::i32> a{};
    EXPECT_FALSE(a.resize(1));
}

// Размер map берётся движковым Count(); вне игры остаётся запасной путь по раскладке.
TEST(MapView, ReadsSizeViaLayoutFallback) {
    std::vector<std::uint8_t> object(72, 0);
    const graft::i32 count = 3;
    std::memcpy(object.data() + graft::layout::map_count, &count, sizeof count);

    const graft::map<graft::str, graft::i32> m{object.data()};
    ASSERT_TRUE(static_cast<bool>(m));
    EXPECT_EQ(m.size(), 3);
    EXPECT_EQ((graft::map<graft::str, graft::i32>{}.size()), 0);
}

// ── Современные обёртки: строка, текст, вектор, ссылка ──────────────────────
TEST(Str, BehavesLikeString) {
    const graft::str s{"alpha-beta"};
    EXPECT_EQ(s.size(), 10u);
    EXPECT_TRUE(s.starts_with("alpha"));
    EXPECT_TRUE(s.contains("-be"));
    EXPECT_EQ(s.view(), "alpha-beta");
    EXPECT_EQ(s, graft::str{"alpha-beta"});
    EXPECT_TRUE(static_cast<bool>(s));

    const graft::str empty{};
    EXPECT_TRUE(empty.empty());
    EXPECT_FALSE(static_cast<bool>(empty));
    EXPECT_STREQ(empty.c_str(), "");  // nullptr наружу не протекает
}

TEST(Text, FormatsWithoutManualBuffers) {
    const graft::text t = graft::text::of("{}:{}", "hp", 42);
    EXPECT_EQ(t.view(), "hp:42");

    // Длинная строка и кольцо буферов: несколько живых возвратов подряд не мешают друг другу.
    const std::string big(4096, 'x');
    const graft::text a = graft::text::from(big);
    const graft::text b = graft::text::of("{}", 1);
    EXPECT_EQ(a.view().size(), 4096u);
    EXPECT_EQ(b.view(), "1");
    EXPECT_EQ(a.view().front(), 'x');
}

TEST(Vec3, HasArithmetic) {
    constexpr graft::vec3 a{1, 2, 3};
    constexpr graft::vec3 b{4, 5, 6};
    EXPECT_EQ(a + b, (graft::vec3{5, 7, 9}));
    EXPECT_EQ(b - a, (graft::vec3{3, 3, 3}));
    EXPECT_EQ(a * 2.0f, (graft::vec3{2, 4, 6}));
    EXPECT_FLOAT_EQ(a.dot(b), 32.0f);
    EXPECT_FLOAT_EQ((graft::vec3{3, 4, 0}).length(), 5.0f);
}

TEST(Ref, WorksAsMapKey) {
    // ключом служит сама ссылка — указатель наружу доставать не нужно
    int first = 0;
    int second = 0;
    const graft::obj a{&first};
    const graft::obj b{&second};

    std::unordered_map<graft::obj, int> seen;
    seen[a] = 1;
    seen[b] = 2;
    EXPECT_EQ(seen.at(a), 1);
    EXPECT_EQ(seen.at(b), 2);
    EXPECT_TRUE(a == a);
    EXPECT_FALSE(a == b);
}

// Контейнерные вьюхи — полноценные ranges, значит работают std::ranges-алгоритмы.
static_assert(std::input_iterator<graft::array<graft::i32>::iterator>);
// ...а ещё СТАРЫЕ шаблоны стандартной библиотеки: им нужен iterator_category, без него
// std::iterator_traits пуст и vector(begin, end) не компилируется (диагностика при этом
// про что угодно, кроме настоящей причины).
static_assert(std::is_same_v<std::iterator_traits<graft::array<graft::i32>::iterator>::iterator_category,
                             std::input_iterator_tag>);
static_assert(std::ranges::input_range<graft::array<graft::i32>>);
static_assert(std::ranges::input_range<graft::set<graft::f32>>);
static_assert(std::ranges::input_range<graft::map<graft::i32, graft::i32>>);

TEST(ArrayView, CopiesThroughIteratorPair) {
    FakeArray fake({3, 17, 8});
    const graft::array<graft::i32> a{fake.ptr()};

    const std::vector<graft::i32> copy(a.begin(), a.end());
    EXPECT_EQ(copy, (std::vector<graft::i32>{3, 17, 8}));
    EXPECT_EQ(std::ranges::to<std::vector<graft::i32>>(a), copy);
}

TEST(ArrayView, AssignNeedsEngineResize) {
    FakeArray fake({1, 2, 3});
    const graft::array<graft::i32> a{fake.ptr()};

    // Размер меняет только движковый Resize, а вне игры его нет: assign обязан честно
    // отказаться, а не записать мимо памяти.
    EXPECT_FALSE(a.assign(std::vector<graft::i32>{9, 8}));
    EXPECT_EQ(a.size(), 3);
}

TEST(ArrayView, WorksWithRangesAlgorithms) {
    FakeArray fake({4, 8, 15, 16, 23, 42});
    const graft::array<graft::i32> a{fake.ptr()};

    EXPECT_EQ(std::ranges::count_if(a, [](graft::i32 v) { return v % 2 == 0; }), 4);
    EXPECT_EQ(std::ranges::max(a), 42);
    EXPECT_TRUE(std::ranges::any_of(a, [](graft::i32 v) { return v == 15; }));

    std::vector<graft::i32> doubled;
    for (graft::i32 v : a | std::views::filter([](graft::i32 x) { return x > 15; })) {
        doubled.push_back(v * 2);
    }
    EXPECT_EQ(doubled, (std::vector<graft::i32>{32, 46, 84}));
}

// Раскладка узла map зависит от пары типов — снята с дизассемблера движка и
// закреплена здесь, чтобы правка констант не проходила молча.
TEST(MapView, NodeLayoutPerTypePair) {
    using II = graft::map<graft::i32, graft::i32>;
    using SI = graft::map<graft::str, graft::i32>;
    using IS = graft::map<graft::i32, graft::str>;
    using SS = graft::map<graft::str, graft::str>;

    EXPECT_EQ(II::node_stride, 12u);
    EXPECT_EQ(II::key_offset, 4u);
    EXPECT_EQ(II::value_offset, 8u);

    EXPECT_EQ(SI::node_stride, 24u);
    EXPECT_EQ(SI::key_offset, 8u);
    EXPECT_EQ(SI::value_offset, 16u);

    EXPECT_EQ(IS::node_stride, 24u);
    EXPECT_EQ(IS::value_offset, 16u);

    EXPECT_EQ(SS::node_stride, 24u);
    EXPECT_EQ(SS::key_offset, 8u);
}

// Система типов открыта: свой примитив описывается специализацией enf_type.
enum class Faction : graft::i32 { none, alpha, beta };

}  // namespace

template <>
struct graft::enf_type<Faction> {
    static constexpr graft::name_t id{"int"};
    static constexpr const char* name = id.value;
};
template <>
inline constexpr bool graft::is_element<Faction> = true;

namespace {

TEST(Types, UserDefinedPrimitive) {
    EXPECT_STREQ(graft::enf_type<Faction>::name, "int");
    EXPECT_STREQ(graft::enf_type<graft::array<Faction>>::name, "array<int>");

    // и он читается из слота как обычное значение
    const Faction stored = Faction::beta;
    EXPECT_EQ(graft::element_access<Faction>::load(&stored), Faction::beta);
}

// typename: имя класса лежит указателем по +16.
TEST(TypeView, ReadsName) {
    std::vector<std::uint8_t> object(32, 0);
    const char* name = "SeraphGraft";
    std::memcpy(object.data() + graft::layout::type_name, &name, sizeof name);

    const graft::type t{object.data()};
    ASSERT_TRUE(static_cast<bool>(t));
    EXPECT_STREQ(t.name(), "SeraphGraft");
    EXPECT_EQ(graft::type{}.name(), nullptr);
}

}  // namespace

namespace {

// ── Поля класса и ABI обёртки ────────────────────────────────────────────────
// Класс с полями живёт столько же, сколько скриптовый объект: библиотека держит по
// одному экземпляру на объект. Голая обёртка полей не имеет и обязана остаться
// 8-байтным агрегатом — иначе MS x64 перейдёт на скрытый указатель и ABI разъедется.
struct Plain : graft::script_object<"TestRef"> {
    graft::i32 Tag() const { return 7; }
};
struct Counter : graft::script_object<"TestBox"> {
    graft::i32 value = 0;  // обычное поле
    graft::i32 Bump(graft::i32 by) { return value += by; }
    graft::i32 Peek() const { return value; }
};

// Обёртка без полей — по-прежнему годится и аргументом натива, и элементом контейнера.
static_assert(sizeof(Plain) == sizeof(void*));
static_assert(std::is_aggregate_v<Plain> && std::is_trivially_copyable_v<Plain>);
static_assert(graft::detail::abi_value<Plain>);
static_assert(graft::is_element<Plain>);
static_assert(!graft::detail::has_fields<Plain>);
// Класс с полями — нет: он не влезает в регистр, и сторож ABI это ловит на компиляции.
static_assert(graft::detail::has_fields<Counter>);
static_assert(!graft::detail::abi_value<Counter>);
static_assert(!graft::is_element<Counter>);
// Тип трамплина обязан быть ровно тем, что зовёт движок: объект в первом аргументе и
// никакого скрытого указателя возврата.
static_assert(std::is_same_v<decltype(&graft::detail::member_thunk<&Counter::Bump>::call),
                             graft::i32(__fastcall*)(void*, graft::i32)>);
static_assert(std::is_same_v<decltype(&graft::detail::member_thunk<&Plain::Tag>::call),
                             graft::i32(__fastcall*)(void*)>);
// Имя скриптового класса видно через базу — по нему bind.class_<C>() и работает.
static_assert(std::string_view{Counter::script_class.value} == "TestBox");
static_assert(std::string_view{graft::enf_type<Plain>::name} == "TestRef");

// Зовём ровно так, как это делает движок: через трамплин, с объектом в первом аргументе.
template <auto Mfn, class... A>
auto call_native(void* self, A... args) {
    return graft::detail::member_thunk<Mfn>::call(self, args...);
}

TEST(ObjectFields, SurviveBetweenNativeCalls) {
    int a = 0, b = 0;

    EXPECT_EQ(call_native<&Counter::Bump>(&a, 5), 5);
    EXPECT_EQ(call_native<&Counter::Bump>(&a, 7), 12);  // поле пережило вызов
    EXPECT_EQ(call_native<&Counter::Bump>(&b, 100), 100);
    EXPECT_EQ(call_native<&Counter::Peek>(&a), 12);     // и у каждого объекта своё

    graft::detail::forget_instance<Counter>(&a);
    EXPECT_EQ(call_native<&Counter::Peek>(&a), 0);      // объект умер — поля тоже
    EXPECT_EQ(call_native<&Counter::Peek>(&b), 100);    // соседа не задело
    graft::detail::forget_instance<Counter>(&b);
}

TEST(ObjectFields, WrapperWithoutFieldsNeedsNoTable) {
    int object = 0;
    EXPECT_EQ(call_native<&Plain::Tag>(&object), 7);
    EXPECT_TRUE(graft::detail::instances<Plain>().empty());
}

}  // namespace

namespace {

// ── Привязка обычного C++ ────────────────────────────────────────────────────
// Класс ничего не знает ни про скрипты, ни про graft и написан в обычных типах.
// Границу переводит трамплин: string <-> std::string/std::string_view,
// owned string <- std::string возвратом.
struct Plainest {
    std::string last;

    void Remember(std::string value) { last = std::move(value); }
    std::string Echo(std::string_view suffix) const { return last + std::string{suffix}; }
    int Size() const { return static_cast<int>(last.size()); }
};

// Свободная функция тоже привязывается методом: объект приходит первым параметром.
int TwiceSize(const Plainest& self) {
    return self.Size() * 2;
}

template <auto F>
using thunk = graft::detail::method_thunk<Plainest, F>;

// Движок должен увидеть ровно ABI-типы, а не std::string.
static_assert(std::is_same_v<decltype(&thunk<&Plainest::Remember>::call),
                             void(__fastcall*)(void*, graft::str)>);
static_assert(std::is_same_v<decltype(&thunk<&Plainest::Echo>::call),
                             graft::text(__fastcall*)(void*, graft::str)>);
static_assert(std::is_same_v<decltype(&thunk<&TwiceSize>::call), int(__fastcall*)(void*)>);
// И объявить их в скрипте как обычные string / owned string / int.
static_assert(std::string_view{thunk<&Plainest::Echo>::ret} == "owned string");
static_assert(std::string_view{thunk<&Plainest::Size>::ret} == "int");

TEST(PlainBinding, DeclaresOrdinaryEnforceTypes) {
    EXPECT_STREQ(thunk<&Plainest::Remember>::args[0], "string");
    EXPECT_EQ(thunk<&Plainest::Remember>::args[1], nullptr);
    EXPECT_STREQ(thunk<&Plainest::Echo>::args[0], "string");
    EXPECT_EQ(thunk<&TwiceSize>::args[0], nullptr);  // объект в объявление не идёт
}

TEST(PlainBinding, TranslatesOrdinaryCppTypes) {
    int object = 0;

    thunk<&Plainest::Remember>::call(&object, graft::str{"alpha"});
    EXPECT_EQ(thunk<&Plainest::Size>::call(&object), 5);
    EXPECT_STREQ(thunk<&Plainest::Echo>::call(&object, graft::str{"-beta"}).raw, "alpha-beta");

    // свободная функция, привязанная методом, видит тот же объект
    EXPECT_EQ(thunk<&TwiceSize>::call(&object), 10);

    graft::detail::forget_instance<Plainest>(&object);
    EXPECT_EQ(thunk<&Plainest::Size>::call(&object), 0);
    graft::detail::forget_instance<Plainest>(&object);
}

}  // namespace

// ── Нативные типы C++ в сигнатуре ────────────────────────────────────────────
// Писать нативы в graft::i32 / graft::str / graft::out не нужно: трамплин переводит
// обычные типы сам. Эти кейсы держат перевод — если он отвалится, объявление для
// скрипта разъедется с реализацией молча.
namespace native_types {

int Add(int a, int b) {
    return a + b;
}
std::string Greet(std::string_view who) {
    return std::string{"hi "} + std::string{who};
}
int SumVector(std::vector<int> values) {
    int sum = 0;
    for (int v : values) {
        sum += v;
    }
    return sum;
}
int TotalLength(std::vector<std::string> words) {
    int sum = 0;
    for (const std::string& w : words) {
        sum += static_cast<int>(w.size());
    }
    return sum;
}
std::array<float, 3> Doubled(std::array<float, 3> v) {
    return {v[0] * 2, v[1] * 2, v[2] * 2};
}
// Неконстантная ссылка = out-аргумент, как у самого движка.
void Fill(graft::array<int>& dst) {
    dst.resize(1);
}

using Add_t = graft::detail::free_thunk<&Add>;
using Greet_t = graft::detail::free_thunk<&Greet>;
using SumVector_t = graft::detail::free_thunk<&SumVector>;
using TotalLength_t = graft::detail::free_thunk<&TotalLength>;
using Doubled_t = graft::detail::free_thunk<&Doubled>;
using Fill_t = graft::detail::free_thunk<&Fill>;

// int — это и есть Enforce int: graft::i32 всего лишь его псевдоним.
TEST(NativeTypes, PlainIntIsEnforceInt) {
    EXPECT_STREQ(Add_t::ret, "int");
    EXPECT_STREQ(Add_t::args[0], "int");
    EXPECT_STREQ(Add_t::args[1], "int");
    EXPECT_EQ(Add_t::args[2], nullptr);
}

// std::string_view на входе — это string, std::string на возврате — owned string.
TEST(NativeTypes, StdStringCrossesAsOwnedString) {
    EXPECT_STREQ(Greet_t::ret, "owned string");
    EXPECT_STREQ(Greet_t::args[0], "string");
}

TEST(NativeTypes, StdVectorArrivesAsScriptArray) {
    EXPECT_STREQ(SumVector_t::args[0], "array<int>");
    EXPECT_STREQ(TotalLength_t::args[0], "array<string>");
}

TEST(NativeTypes, StdArrayOfThreeFloatsIsVector) {
    EXPECT_STREQ(Doubled_t::ret, "vector");
    EXPECT_STREQ(Doubled_t::args[0], "vector");
}

// Главное про ссылку: в объявлении она обязана стать `out`, иначе движок отдаст копию
// и запись потеряется.
TEST(NativeTypes, NonConstReferenceDeclaresOut) {
    EXPECT_STREQ(Fill_t::args[0], "out array<int>");
    EXPECT_STREQ(Fill_t::ret, "void");
}

}  // namespace native_types

// ── graft::vector рядом со std::array<float, 3> ──────────────────────────────
// Скриптовый тип называется vector, и в сигнатуре натива он читается ровно так же.
// std::array<float,3> остаётся равноправным вариантом: раскладка та же, поэтому выбор
// между ними — вопрос вкуса, а не возможностей.
namespace vector_type {

graft::vector Scale(graft::vector v, float k) {
    return v * k;
}
std::array<float, 3> ScaleArr(std::array<float, 3> v, float k) {
    return {v[0] * k, v[1] * k, v[2] * k};
}

using Scale_t = graft::detail::free_thunk<&Scale>;
using ScaleArr_t = graft::detail::free_thunk<&ScaleArr>;

TEST(VectorType, BothSpellingsDeclareTheSameThing) {
    EXPECT_STREQ(Scale_t::ret, "vector");
    EXPECT_STREQ(Scale_t::args[0], "vector");
    EXPECT_STREQ(ScaleArr_t::ret, "vector");
    EXPECT_STREQ(ScaleArr_t::args[0], "vector");
}

// Раскладка обязана совпадать: на этом держится и ABI, и переход между формами.
TEST(VectorType, SameLayoutAsStdArray) {
    static_assert(sizeof(graft::vector) == sizeof(std::array<float, 3>));
    const graft::vector v{1, 2, 3};
    const auto a = static_cast<std::array<float, 3>>(v);
    EXPECT_EQ(a[0], 1.0f);
    EXPECT_EQ(a[2], 3.0f);
    EXPECT_EQ(graft::vector::from(a), v);
}

TEST(VectorType, ArithmeticIsTheReasonToPreferIt) {
    const graft::vector a{1, 2, 3};
    const graft::vector b{4, 5, 6};
    EXPECT_EQ((a + b), (graft::vector{5, 7, 9}));
    EXPECT_FLOAT_EQ(a.dot(b), 32.0f);
    EXPECT_FLOAT_EQ(graft::vector(0, 3, 4).length(), 5.0f);
}

// Старое имя остаётся рабочим псевдонимом — им написан код, который уже есть.
TEST(VectorType, Vec3StillNamesTheSameType) {
    static_assert(std::is_same_v<graft::vec3, graft::vector>);
}

}  // namespace vector_type

// ── std::span: массив чисел без копии ────────────────────────────────────────
namespace span_arg {

int SumSpan(std::span<const int> values) {
    return std::ranges::fold_left(values, 0, std::plus{});
}
float MaxSpan(std::span<const float> values) {
    return values.empty() ? 0.0f : std::ranges::max(values);
}

using SumSpan_t = graft::detail::free_thunk<&SumSpan>;
using MaxSpan_t = graft::detail::free_thunk<&MaxSpan>;

// В объявлении это тот же array<T>, что и std::vector — разница только в том, что
// копии не происходит.
TEST(SpanArg, DeclaresTheSameArrayAsVector) {
    EXPECT_STREQ(SumSpan_t::args[0], "array<int>");
    EXPECT_STREQ(MaxSpan_t::args[0], "array<float>");
}

// Только для типов, лежащих в движке как есть. У строк и ссылок в слоте не значение,
// а указатель или обёртка — span поверх них смотрел бы не на те байты, поэтому
// специализация на них не срабатывает и такой натив просто не соберётся.
TEST(SpanArg, OnlyForPlainElements) {
    static_assert(
        std::is_same_v<graft::detail::arg_of<std::span<const int>>::abi, graft::array<int>>);
    static_assert(std::is_same_v<graft::detail::arg_of<std::span<const graft::str>>::abi,
                                 std::span<const graft::str>>);
}

}  // namespace span_arg
