// Вызов В ДРУГУЮ СТОРОНУ: как graft зовёт маршалируемые `proto` самого движка.
//
// Здесь стоит ФАЛЬШИВЫЙ движок: дескриптор функции и дескрипторы её параметров,
// разложенные ровно по graft::layout, плюс импл — обычная C-функция. Так вся сборка
// блока аргументов проверяется без игры: если смещения дескриптора поедут или мы
// перепутаем порядок указателей, тест покраснеет здесь, а не падением на сервере.
//
// Разложено по re/README.md («Вызов В ДРУГУЮ СТОРОНУ»): у дескриптора +32 — массив
// дескрипторов переменных, +44 — сколько их всего, +88 — сколько из них параметров.
// Скриптовая переменная — 40 байт, и движок её КОПИРУЕТ из дескриптора, а не строит.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "graft/native.hpp"

namespace {

using graft::layout::var_size;

// Дескриптор одной переменной: те же 40 байт, что и в рантайме. Движок держит в нём
// шаблон — тег типа, флаги и размерности, — и льёт его целиком в кадр вызова.
std::array<unsigned char, var_size> var_desc(std::uint32_t tag, std::uint32_t flags = 0) {
    std::array<unsigned char, var_size> out{};
    // Метка, по которой видно, что шаблон СКОПИРОВАН, а не собран нами с нуля: байты,
    // до которых мы не должны додуматься сами.
    out[8] = 0xAB;
    out[9] = 0xCD;
    std::memcpy(out.data() + graft::layout::var_type, &tag, sizeof tag);
    std::memcpy(out.data() + graft::layout::var_flags, &flags, sizeof flags);
    return out;
}

// Фальшивая движковая функция: дескриптор + параметры + импл.
class FakeProto {
public:
    FakeProto(std::vector<std::uint32_t> tags, void* impl, std::uint32_t ret_tag = 0)
        : ret_(var_desc(ret_tag)) {
        for (std::uint32_t t : tags) {
            params_.push_back(var_desc(t));
        }
        for (auto& p : params_) {
            table_.push_back(p.data());
        }
        desc_.assign(128, 0);
        write(graft::layout::desc_impl, impl);
        write(graft::layout::desc_params, static_cast<void*>(table_.data()));
        const std::uint32_t count = static_cast<std::uint32_t>(table_.size());
        std::memcpy(desc_.data() + graft::layout::desc_var_count, &count, sizeof count);
        desc_[graft::layout::desc_param_count] = static_cast<unsigned char>(table_.size());
        const std::uint32_t flags = graft::layout::flag_marshalled;
        std::memcpy(desc_.data() + graft::layout::desc_flags, &flags, sizeof flags);
    }

    graft::script::method method() const {
        graft::script::method m;
        m.desc = const_cast<unsigned char*>(desc_.data());
        std::memcpy(&m.impl, desc_.data() + graft::layout::desc_impl, sizeof m.impl);
        std::memcpy(&m.flags, desc_.data() + graft::layout::desc_flags, sizeof m.flags);
        m.executable = true;  // импл — настоящая функция, страница исполняемая
        return m;
    }
    void* ret_var() { return ret_.data(); }

    // Флаги шаблона i-го параметра — как их выставил компилятор Enforce (0x40 у vector).
    void set_param_flags(std::size_t i, std::uint32_t flags) {
        std::memcpy(params_[i].data() + graft::layout::var_flags, &flags, sizeof flags);
    }

    // Контекст по +24 — через него движок находит дескриптор типа переменной.
    void set_param_context(std::size_t i, void* ctx) {
        std::memcpy(params_[i].data() + graft::layout::var_context, &ctx, sizeof ctx);
    }

private:
    void write(std::size_t at, void* p) { std::memcpy(desc_.data() + at, &p, sizeof p); }

    std::vector<std::array<unsigned char, var_size>> params_;
    std::array<unsigned char, var_size> ret_;
    std::vector<void*> table_;
    std::vector<unsigned char> desc_;
};

// ── Импл-«движок»: то, что мы обязаны ему подать ─────────────────────────────
// Сигнатура ровно та, которой движок зовёт маршалируемый метод и которую уже принимают
// наши собственные трамплины (marshal.hpp): rcx = объект, rdx = указатель на массив
// переменных, r8 = указатель на переменную возврата.

void* g_seen_self = nullptr;
std::int64_t g_seen_a = 0;
std::string g_seen_b;

std::int64_t __fastcall sum_impl(void* self, void*** args, void** ret) {
    g_seen_self = self;
    void** block = *args;
    g_seen_a = *reinterpret_cast<const std::int32_t*>(block[0]);
    g_seen_b = *reinterpret_cast<const char* const*>(block[1]);
    if (ret && *ret) {
        const std::int32_t out = static_cast<std::int32_t>(g_seen_a) * 2;
        std::memcpy(*ret, &out, sizeof out);
    }
    return 0;
}

// Пишет в аргумент — так работает `out` у маршалируемого `proto`.
std::int64_t __fastcall out_impl(void*, void*** args, void**) {
    void** block = *args;
    const std::int32_t written = 77;
    std::memcpy(block[1], &written, sizeof written);
    return 0;
}

std::int64_t __fastcall noop_impl(void*, void***, void**) {
    return 0;
}

// Вторая форма: у статических и у методов типов-значений объекта нет, поэтому rcx у них
// не занят — первый аргумент это уже массив переменных. Ошибиться тут нельзя: движок
// прочитал бы объект как массив (проверено падением).
void* g_two_arg_first = nullptr;
std::int64_t __fastcall static_impl(void*** args, void** ret) {
    void** block = *args;
    g_two_arg_first = block[0];
    if (ret && *ret) {
        const std::int32_t out = *reinterpret_cast<const std::int32_t*>(block[0]) + 1;
        std::memcpy(*ret, &out, sizeof out);
    }
    return 0;
}

// ── Сама переменная ─────────────────────────────────────────────────────────

TEST(CallOut, SynthesizedVarCarriesTagAndScalarExtent) {
    const graft::script::var v = graft::script::var::of_tag(graft::script::type_int);
    EXPECT_EQ(v.tag(), graft::script::type_int);
    // Подготовка кадра в движке пишет сюда 0x10000: младшее слово — размерность
    // статического массива (0), старшее — счётчик, который у скаляра равен 1.
    std::uint32_t extent = 0;
    std::memcpy(&extent, v.raw + 32, sizeof extent);
    EXPECT_EQ(extent, 0x10000u);
}

TEST(CallOut, TemplateIsCopiedWholeNotRebuilt) {
    const auto desc = var_desc(graft::script::type_string, 0x80);
    const graft::script::var v = graft::script::var::from_template(desc.data());
    EXPECT_EQ(v.tag(), graft::script::type_string);
    // Байты, которых мы не знаем, обязаны доехать как есть — в этом весь смысл: шаблон
    // несёт вызываемая функция, а мы его не сочиняем.
    EXPECT_EQ(v.raw[8], 0xAB);
    EXPECT_EQ(v.raw[9], 0xCD);
    std::uint32_t flags = 0;
    std::memcpy(&flags, v.raw + graft::layout::var_flags, sizeof flags);
    EXPECT_EQ(flags, 0x80u);
}

// ── Чтение дескриптора ──────────────────────────────────────────────────────

TEST(CallOut, ReadsParamCountAndTemplatesFromDescriptor) {
    FakeProto fake({graft::script::type_int, graft::script::type_string},
                   reinterpret_cast<void*>(&noop_impl));
    const graft::script::method fn = fake.method();
    EXPECT_EQ(graft::script::param_count(fn), 2u);
    ASSERT_NE(graft::script::param_template(fn, 0), nullptr);
    std::uint32_t tag = 0;
    std::memcpy(&tag,
                static_cast<const char*>(graft::script::param_template(fn, 1)) +
                    graft::layout::var_type,
                sizeof tag);
    EXPECT_EQ(tag, graft::script::type_string);
}

TEST(CallOut, ParamTemplateOutOfRangeIsNull) {
    FakeProto fake({graft::script::type_int}, reinterpret_cast<void*>(&noop_impl));
    EXPECT_EQ(graft::script::param_template(fake.method(), 1), nullptr);
    EXPECT_EQ(graft::script::param_template(fake.method(), 99), nullptr);
}

// ── Вызов целиком ───────────────────────────────────────────────────────────

TEST(CallOut, PassesArgumentsAndSelfThenReadsReturn) {
    FakeProto fake({graft::script::type_int, graft::script::type_string},
                   reinterpret_cast<void*>(&sum_impl), graft::script::type_int);
    int self_marker = 0;
    const graft::value args[] = {graft::value{graft::i32{21}}, graft::value{std::string{"hello"}}};
    const auto r = graft::call_proto(fake.method(), &self_marker, args, graft::script::tag_int);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(g_seen_self, &self_marker);
    EXPECT_EQ(g_seen_a, 21);
    EXPECT_EQ(g_seen_b, "hello");
    EXPECT_EQ(r->ret.as<graft::i32>(), 42);
}

TEST(CallOut, ReadsOutArgumentBack) {
    FakeProto fake({graft::script::type_int, graft::script::type_int},
                   reinterpret_cast<void*>(&out_impl));
    int self_marker = 0;
    const graft::value args[] = {graft::value{graft::i32{1}}, graft::value{graft::i32{0}}};
    const auto r = graft::call_proto(fake.method(), &self_marker, args);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->arg(1).as<graft::i32>(), 77);
}

TEST(CallOut, RefusesWhenArityDisagreesWithDescriptor) {
    FakeProto fake({graft::script::type_int, graft::script::type_int},
                   reinterpret_cast<void*>(&noop_impl));
    int self_marker = 0;
    const graft::value one[] = {graft::value{graft::i32{1}}};
    const auto r = graft::call_proto(fake.method(), &self_marker, one);
    // Лишний/недостающий аргумент — это несобранный блок и почти наверняка падение.
    // Отказ виден в типе, а не превращается в ноль.
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), graft::miss::wrong_arity);
}

TEST(CallOut, NullSelfSwitchesToTwoArgumentForm) {
    FakeProto fake({graft::script::type_int}, reinterpret_cast<void*>(&static_impl));
    const graft::value args[] = {graft::value{graft::i32{41}}};
    g_two_arg_first = nullptr;
    const auto r = graft::call_proto(fake.method(), nullptr, args, graft::script::tag_int);
    ASSERT_TRUE(r.has_value());
    EXPECT_NE(g_two_arg_first, nullptr);
    EXPECT_EQ(r->ret.as<graft::i32>(), 42);
}

// Дескриптор — последнее слово: статическому методу rcx не передаётся, что бы ни
// передала вызывающая сторона.
TEST(CallOut, StaticFlagWinsOverGivenSelf) {
    FakeProto fake({graft::script::type_int}, reinterpret_cast<void*>(&static_impl));
    graft::script::method fn = fake.method();
    fn.flags |= graft::layout::flag_static;
    const graft::value args[] = {graft::value{graft::i32{7}}};
    int self_marker = 0;
    const auto r = graft::call_proto(fn, &self_marker, args, graft::script::tag_int);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->ret.as<graft::i32>(), 8);
}

TEST(CallOut, RefusesNonExecutableImpl) {
    FakeProto fake({}, reinterpret_cast<void*>(&noop_impl));
    graft::script::method fn = fake.method();
    fn.executable = false;  // в impl байткод: звать как C-функцию нельзя
    int self_marker = 0;
    const auto r = graft::call_proto(fn, &self_marker, {});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), graft::miss::not_native);
}

TEST(CallOut, RefusesMissingImpl) {
    graft::script::method fn;  // метода нет вовсе
    int self_marker = 0;
    const auto r = graft::call_proto(fn, &self_marker, {});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), graft::miss::not_found);
}

TEST(CallOut, RefusesMoreArgsThanBlockHolds) {
    std::vector<std::uint32_t> many(graft::script::max_proto_args + 1, graft::script::type_int);
    FakeProto fake(many, reinterpret_cast<void*>(&noop_impl));
    std::vector<graft::value> args(many.size(), graft::value{graft::i32{0}});
    int self_marker = 0;
    const auto r = graft::call_proto(fake.method(), &self_marker, args);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), graft::miss::too_many_args);
}

// Возврат без переменной: у `proto void` движок r8 не даёт, и импл обязан это пережить.
TEST(CallOut, VoidReturnLeavesResultEmpty) {
    FakeProto fake({graft::script::type_int, graft::script::type_int},
                   reinterpret_cast<void*>(&out_impl));
    int self_marker = 0;
    const graft::value args[] = {graft::value{graft::i32{1}}, graft::value{graft::i32{0}}};
    const auto r = graft::call_proto(fake.method(), &self_marker, args);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->ret.empty());
}

// ── `proto external` у метода объекта ───────────────────────────────────────
// Снято с живого движка репортом: IEntity.GetLocalPosition объявлен без параметров, а
// в дескрипторе у него один — сам объект. В rcx его нет: форма двухаргументная, объект
// первым в блоке, ровно как у методов типов-значений.
void*        g_ext_self = nullptr;
std::int32_t g_ext_a    = 0;

std::int64_t __fastcall external_impl(void*** args, void** ret) {
    void** block = *args;
    std::memcpy(&g_ext_self, block[0], sizeof g_ext_self);
    std::memcpy(&g_ext_a, block[1], sizeof g_ext_a);
    const std::int32_t written = 77;
    std::memcpy(block[2], &written, sizeof written); // out-параметр
    if (ret && *ret) {
        const std::int32_t out = g_ext_a + 1;
        std::memcpy(*ret, &out, sizeof out);
    }
    return 0;
}

TEST(CallOut, ExternalMethodTakesTheObjectFirstInTheBlock) {
    FakeProto             fake({graft::script::tag_class, graft::script::type_int, graft::script::type_int},
                   reinterpret_cast<void*>(&external_impl));
    graft::script::method fn = fake.method();
    fn.flags |= graft::layout::flag_external;
    int                self_marker = 0;
    const graft::value args[]      = {graft::value{graft::i32{5}}, graft::value{graft::i32{0}}};
    g_ext_self                     = nullptr;
    const auto r                   = graft::call_proto(fn, &self_marker, args, graft::script::tag_int);
    ASSERT_TRUE(r.has_value()) << static_cast<int>(r.error());
    EXPECT_EQ(g_ext_self, &self_marker);
    EXPECT_EQ(g_ext_a, 5);
    EXPECT_EQ(r->ret.as<graft::i32>(), 6);
    // Номера out-аргументов — те, что видит вызывающий, без объекта впереди.
    ASSERT_EQ(r->count, 2u);
    EXPECT_EQ(r->arg(1).as<graft::i32>(), 77);
}

// Как переменная держит объект, решает её тип (sub_1403673C0): у класса с флагом 0x10 —
// обёрткой {vtable, счётчик, объект}, и импл достаёт `this` по +16. Голый указатель там
// импл читает как обёртку — так IEntity.GetLocalPosition возвращал мусор, а GetBounds
// падал внутри движка.
struct fake_types {
    std::array<unsigned char, 96>  type{};  // дескриптор типа: +16 имя, +80 флаги
    std::array<void*, 8>           table{}; // ctx+104: типы по номеру из тега
    std::array<unsigned char, 112> ctx{};

    explicit fake_types(const char* name) {
        std::memcpy(type.data() + graft::layout::class_name, &name, sizeof name);
        type[graft::layout::type_flags] = 0x10;
        table[5]                        = type.data();
        void* t                         = table.data();
        std::memcpy(ctx.data() + graft::layout::context_types, &t, sizeof t);
    }

    static constexpr std::uint32_t tag = graft::script::type_class | 5;
};

struct alignas(8) fake_entity {
    void*         vtable     = nullptr;
    void*         class_desc = nullptr; // +8: указатель — значит объект, а не обёртка
    std::uint64_t pad[2]{};
    void*         back_ref = nullptr; // +32: своя обёртка
};

struct alignas(8) fake_ref {
    void*         vtable = nullptr;
    std::uint64_t count  = 1;       // +8: счётчик, маленькое число
    void*         object = nullptr; // +16
};

void* g_receiver_value = nullptr;

std::int64_t __fastcall receiver_impl(void*** args, void**) {
    std::memcpy(&g_receiver_value, (*args)[0], sizeof g_receiver_value);
    return 0;
}

std::expected<graft::proto_result, graft::miss> call_external_on(fake_types&   types,
                                                                 fake_entity&  entity,
                                                                 std::uint32_t flags = 0x1000) {
    // int-параметр — донор переменной возврата: без движка другого взять неоткуда.
    static FakeProto fake({fake_types::tag, graft::script::type_int},
                          reinterpret_cast<void*>(&receiver_impl));
    fake.set_param_context(0, types.ctx.data());
    fake.set_param_flags(0, flags); // 0x1000 — как у приёмника IEntity в игре
    graft::script::method fn = fake.method();
    fn.flags |= graft::layout::flag_external;
    g_receiver_value          = nullptr;
    const graft::value args[] = {graft::value{graft::i32{0}}};
    return graft::call_proto(fn, &entity, args);
}

TEST(CallOut, ObjectGoesAsWrapperWhereTheTypeHoldsWrappers) {
    fake_types  types{"IEntity"};
    fake_entity entity;
    entity.class_desc = &types;
    fake_ref wrapper;
    wrapper.object  = &entity;
    entity.back_ref = &wrapper;
    ASSERT_TRUE(call_external_on(types, entity).has_value());
    EXPECT_EQ(g_receiver_value, &wrapper);
}

// Параметры Class и Managed движок объявляет с 0x800 (снято в игре: флаги 0xc00), и такие
// переменные держат объект прямым указателем — даже если у типа флаг 0x10 есть (у Managed
// есть). Так и работает EnScript.GetClassVar(Class inst, ...).
TEST(CallOut, OwnedVariableTakesTheObjectItself) {
    fake_types  types{"Managed"};
    fake_entity entity;
    entity.class_desc = &types;
    fake_ref wrapper;
    wrapper.object  = &entity;
    entity.back_ref = &wrapper;
    ASSERT_TRUE(call_external_on(types, entity, 0xc00).has_value());
    EXPECT_EQ(g_receiver_value, &entity);
}

// Обёртку ждут, а её нет — отказ, а не мусор вместо `this`.
TEST(CallOut, ObjectWithoutWrapperIsRefusedWhereOneIsExpected) {
    fake_types  types{"IEntity"};
    fake_entity entity;
    entity.class_desc = &types;
    const auto r      = call_external_on(types, entity);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), graft::miss::unsafe_arg);
}

// ── vector-возврат маршалируемого `proto` ───────────────────────────────────
// Движок присваивает vector-переменной 12 байт через указатель из +0 только при флаге
// 0x40 (sub_140347CB0); без него значение до нас не доезжает. Импл ниже ведёт себя так
// же — так возвращал нули Object.ModelToWorld.
std::int64_t __fastcall vector_twice_impl(void*, void*** args, void** ret) {
    void**        block = *args;
    graft::vector in{};
    std::memcpy(&in, *static_cast<void**>(block[0]), sizeof in);
    if (ret && *ret) {
        std::uint32_t flags = 0;
        std::memcpy(&flags, static_cast<char*>(*ret) + graft::layout::var_flags, sizeof flags);
        if ((flags & 0x40) != 0) {
            const graft::vector out = in * 2.0f;
            std::memcpy(*static_cast<void**>(*ret), &out, sizeof out);
        }
    }
    return 0;
}

TEST(CallOut, VectorReturnArrivesThroughItsPointer) {
    FakeProto fake({graft::script::tag_vector}, reinterpret_cast<void*>(&vector_twice_impl));
    fake.set_param_flags(0, 0x40); // так vector-параметр размечает компилятор Enforce
    int                self_marker = 0;
    const graft::value args[]      = {graft::value{graft::vector{1, 2, 3}}};
    const auto         r           = graft::call_proto(fake.method(), &self_marker, args, graft::script::tag_vector);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->ret.as<graft::vector>(), (graft::vector{2, 4, 6}));
}

// Шаблон без 0x40 (донор, чужая сборка) — флаг всё равно ставим: у vector он означает
// хранилище по указателю, а не «не писать».
TEST(CallOut, VectorReturnGetsIndirectFlagEvenFromPlainTemplate) {
    FakeProto          fake({graft::script::tag_vector}, reinterpret_cast<void*>(&vector_twice_impl));
    int                self_marker = 0;
    const graft::value args[]      = {graft::value{graft::vector{1, 0, 0}}};
    const auto         r           = graft::call_proto(fake.method(), &self_marker, args, graft::script::tag_vector);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->ret.as<graft::vector>(), (graft::vector{2, 0, 0}));
}

// Донор берётся по ТЕГУ параметра, а не по номеру из объявления: у `proto external`
// нулевым в дескрипторе едет объект (Physics.SetMass — объект, потом float).
TEST(CallOut, TemplateIsFoundByTagNotByDeclaredIndex) {
    FakeProto                   fake({graft::script::tag_class, graft::script::tag_float},
                   reinterpret_cast<void*>(&noop_impl));
    const graft::script::method fn = fake.method();
    EXPECT_EQ(graft::detail::param_template_of_tag(fn, graft::script::tag_float),
              graft::script::param_template(fn, 1));
    EXPECT_EQ(graft::detail::param_template_of_tag(fn, graft::script::tag_vector), nullptr);
}

// Out-аргумент типизированной обёртки — std::ref: после вызова в нём то, что движок
// записал в переменную блока. Остальные аргументы не трогаются.
TEST(CallOut, OutArgumentsAreWrittenBackThroughStdRef) {
    graft::proto_result r;
    r.count   = 3;
    r.args[0] = graft::value{graft::i32{1}};
    r.args[1] = graft::value{graft::vector{7, 8, 9}};
    r.args[2] = graft::value{graft::f32{0.5f}};
    graft::vector    where{};
    graft::f32       fraction  = 0;
    const graft::i32 untouched = 0;
    graft::detail::write_back(r, untouched, std::ref(where), std::ref(fraction));
    EXPECT_EQ(where, (graft::vector{7, 8, 9}));
    EXPECT_EQ(fraction, 0.5f);
}

// Статический `proto` и `proto native` без движка: промах, а не падение.
TEST(CallOut, StaticCallsWithoutEngineAreAMiss) {
    EXPECT_EQ((graft::ref<"Math">::proto_static<graft::f32, "Sqrt">(16.0f)), 0.0f);
    EXPECT_EQ((graft::ref<"Math">::call_static<graft::i32, "AbsInt">(-3)), 0);
    const auto tried = graft::ref<"Math">::try_proto_static<graft::f32, "Sqrt">(16.0f);
    ASSERT_FALSE(tried.has_value());
    EXPECT_EQ(tried.error(), graft::miss::not_found);
}

// Статический `proto native` собран свободной функцией: vector-возврат компилятор сам
// кладёт в скрытый буфер первым — ровно как у external.
graft::vector __fastcall static_vector_native(float by) {
    return graft::vector{by, by * 2, by * 3};
}

TEST(CallOut, StaticNativeReturnsVectorLikeAFreeFunction) {
    graft::script::method fn;
    fn.impl       = reinterpret_cast<void*>(&static_vector_native);
    fn.flags      = graft::layout::flag_static | graft::layout::flag_native;
    fn.executable = true;
    EXPECT_EQ(graft::detail::call_static_native<graft::vector>(fn, 2.0f),
              (graft::vector{2, 4, 6}));
}

// ── Владение чужим объектом ─────────────────────────────────────────────────
// Синтетический объект: у скриптового по +8 лежит дескриптор класса, по нему borrowed и
// отличает «тот же объект» от «движок переиспользовал адрес».

struct FakeObject {
    std::array<void*, 8> words{};
    explicit FakeObject(void* class_desc) { words[graft::layout::object_class / sizeof(void*)] = class_desc; }
    void* ptr() { return words.data(); }
    void set_class(void* class_desc) { words[graft::layout::object_class / sizeof(void*)] = class_desc; }
};

TEST(World, CastKeepsPointer) {
    int marker = 0;
    graft::ref<"Man"> man{&marker};
    const auto entity = graft::cast<graft::ref<"EntityAI">>(man);
    EXPECT_EQ(entity.ptr, &marker);
    EXPECT_STREQ(graft::enf_type<decltype(entity)>::name, "EntityAI");
}

TEST(World, BorrowedSurvivesWhileClassIsSame) {
    int class_name = 0;
    FakeObject o{&class_name};
    const graft::borrowed<graft::ref<"Man">> kept{graft::ref<"Man">{o.ptr()}};
    ASSERT_TRUE(kept.get().has_value());
    EXPECT_EQ(kept.get()->ptr, o.ptr());
}

TEST(World, BorrowedNoticesRecycledAddress) {
    int class_name = 0;
    int other = 0;
    FakeObject o{&class_name};
    const graft::borrowed<graft::ref<"Man">> kept{graft::ref<"Man">{o.ptr()}};
    // Движок удалил игрока и положил по тому же адресу что-то другое.
    o.set_class(&other);
    EXPECT_FALSE(kept.get().has_value());
    EXPECT_FALSE(static_cast<bool>(kept));
}

TEST(World, BorrowedFromNullIsEmpty) {
    const graft::borrowed<graft::ref<"Man">> kept{graft::ref<"Man">{}};
    EXPECT_FALSE(kept.get().has_value());
}

}  // namespace

// ── Прокси поля: синтаксис sol2 с именем в типе ──────────────────────────────
// Тот же синтетический объект, что и у borrowed: дескриптор класса по +8, таблица
// переменных в дескрипторе, поля по слотам. Если раскладка поедет, покраснеет здесь.
namespace {

using namespace graft::literals;

// Класс с одним полем m_id (слот 0) и базой полей 0.
class FakeClass {
public:
    FakeClass() {
        std::memcpy(entry_.data() + graft::layout::var_entry_name, &kName, sizeof kName);
        const std::uint16_t slot = 0;
        std::memcpy(entry_.data() + graft::layout::var_entry_slot, &slot, sizeof slot);
        table_[0] = entry_.data();
        void* table = table_.data();
        std::memcpy(desc_.data() + graft::layout::class_var_table, &table, sizeof table);
        const std::uint32_t count = 1;
        std::memcpy(desc_.data() + graft::layout::class_var_count, &count, sizeof count);
        const std::uint32_t base = 16;  // поля начинаются после заголовка объекта
        std::memcpy(desc_.data() + graft::layout::class_var_base, &base, sizeof base);
    }
    void* desc() { return desc_.data(); }

private:
    static constexpr const char* kName = "m_id";
    std::array<unsigned char, 64> entry_{};
    std::array<void*, 1> table_{};
    std::array<unsigned char, 256> desc_{};
};

class FakeInstance {
public:
    explicit FakeInstance(void* class_desc) {
        std::memcpy(bytes_.data() + graft::layout::object_class, &class_desc, sizeof class_desc);
    }
    void* ptr() { return bytes_.data(); }
    graft::i32 raw_field() const {
        graft::i32 v = 0;
        std::memcpy(&v, bytes_.data() + 16, sizeof v);
        return v;
    }

private:
    std::array<unsigned char, 64> bytes_{};
};

TEST(FieldProxy, ReadsAndWritesThroughSameSlot) {
    FakeClass klass;
    FakeInstance obj{klass.desc()};
    const graft::ref<"Node"> node{obj.ptr()};

    node["m_id"_f] = graft::i32{4242};
    EXPECT_EQ(obj.raw_field(), 4242);

    const graft::i32 read = node["m_id"_f];
    EXPECT_EQ(read, 4242);
}

TEST(FieldProxy, AgreesWithTheExplicitForm) {
    FakeClass klass;
    FakeInstance obj{klass.desc()};
    const graft::ref<"Node"> node{obj.ptr()};
    node.set_field<"m_id">(graft::i32{7});
    // Два синтаксиса обязаны быть одним и тем же обращением, иначе один из них лишний.
    const graft::i32 viaProxy = node["m_id"_f];
    const graft::i32 viaField = node.field<graft::i32, "m_id">();
    EXPECT_EQ(viaProxy, viaField);
    EXPECT_EQ(viaProxy, 7);
}

TEST(FieldProxy, MissingFieldIsEmptyNotGarbage) {
    FakeClass klass;
    FakeInstance obj{klass.desc()};
    const graft::ref<"Node"> node{obj.ptr()};
    EXPECT_FALSE(node["m_nope"_f].exists());
    EXPECT_FALSE(node["m_nope"_f].get<graft::i32>().has_value());
    EXPECT_EQ(static_cast<graft::i32>(node["m_nope"_f]), 0);
}

TEST(FieldProxy, NameLivesInTheTypeNotInTheCall) {
    // Смысл всей затеи: имя — параметр шаблона, поэтому прокси у разных полей это
    // РАЗНЫЕ типы, и кэш слота у каждого свой.
    FakeClass klass;
    FakeInstance obj{klass.desc()};
    const graft::ref<"Node"> node{obj.ptr()};
    static_assert(!std::is_same_v<decltype(node["m_id"_f]), decltype(node["m_other"_f])>);
    SUCCEED();
}

}  // namespace
