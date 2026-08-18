// Разбор объявлений Enforce и зеркало движкового API на C++.
//
// Все строки ниже взяты из ванильных скриптов (E:\DayZ\PDrive\scripts) как есть — это и
// есть источник истины. Если разбор их не осилит, зеркало будет молча неполным, а узнать
// об этом на живом сервере дороже всего.
#include <gtest/gtest.h>

#include <string>
#include <algorithm>
#include <string_view>

#include "graft/enfparse.hpp"

namespace {

const graft::enf::function* find(const graft::enf::class_& k, std::string_view name) {
    for (const auto& f : k.functions) {
        if (f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

const graft::enf::class_* find(const graft::enf::unit& u, std::string_view name) {
    for (const auto& k : u.classes) {
        if (k.name == name) {
            return &k;
        }
    }
    return nullptr;
}

// ── Разбор ──────────────────────────────────────────────────────────────────

TEST(EnfParse, NativeMethodWithOutContainer) {
    // 3_Game/Global/Game.c:947
    const auto u = graft::enf::parse("class CGame\n{\n"
                                     "\tproto native void\t\tGetPlayers( out array<Man> players );\n"
                                     "}\n");
    const graft::enf::class_* k = find(u, "CGame");
    ASSERT_NE(k, nullptr);
    const graft::enf::function* f = find(*k, "GetPlayers");
    ASSERT_NE(f, nullptr);
    EXPECT_TRUE(f->is_native);
    EXPECT_FALSE(f->is_static);
    EXPECT_EQ(f->ret, "void");
    ASSERT_EQ(f->params.size(), 1u);
    EXPECT_EQ(f->params[0].type, "array<Man>");
    EXPECT_EQ(f->params[0].name, "players");
    EXPECT_TRUE(f->params[0].is_out);
}

TEST(EnfParse, MarshalledIsNotNative) {
    // 3_Game/gameplay.c — второй этаж API объявлен без native
    const auto u = graft::enf::parse("class PlayerIdentityBase : Managed\n{\n"
                                     "\tproto string GetPlainId();\n"
                                     "\tproto int GetPlayerId();\n"
                                     "}\n");
    const graft::enf::class_* k = find(u, "PlayerIdentityBase");
    ASSERT_NE(k, nullptr);
    ASSERT_EQ(k->bases.size(), 1u);
    EXPECT_EQ(k->bases[0].name, "Managed");
    const graft::enf::function* f = find(*k, "GetPlainId");
    ASSERT_NE(f, nullptr);
    EXPECT_FALSE(f->is_native);
    EXPECT_EQ(f->ret, "string");
    EXPECT_TRUE(f->params.empty());
}

TEST(EnfParse, StaticAndModifiersAreStrippedFromReturnType) {
    // 1_Core/proto/EnScript.c:160 и :37
    const auto u = graft::enf::parse(
        "class ScriptModule\n{\n"
        "\tstatic proto native ScriptModule LoadScript(ScriptModule parentModule, string "
        "scriptFile, bool listing);\n"
        "\tproto native owned external string ClassName();\n"
        "\tproto volatile int CallFunction(Class inst, string function, out void returnVal, void "
        "parm);\n"
        "}\n");
    const graft::enf::class_* k = find(u, "ScriptModule");
    ASSERT_NE(k, nullptr);

    const graft::enf::function* load = find(*k, "LoadScript");
    ASSERT_NE(load, nullptr);
    EXPECT_TRUE(load->is_static);
    EXPECT_TRUE(load->is_native);
    EXPECT_EQ(load->ret, "ScriptModule");
    ASSERT_EQ(load->params.size(), 3u);
    EXPECT_EQ(load->params[2].type, "bool");

    const graft::enf::function* name = find(*k, "ClassName");
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->ret, "string");  // owned/external — модификаторы, а не часть типа

    const graft::enf::function* call = find(*k, "CallFunction");
    ASSERT_NE(call, nullptr);
    EXPECT_FALSE(call->is_native);
    ASSERT_EQ(call->params.size(), 4u);
    EXPECT_TRUE(call->params[2].is_out);
    EXPECT_EQ(call->params[2].type, "void");
}

TEST(EnfParse, DefaultValueIsKeptSeparateFromName) {
    const auto u = graft::enf::parse("class array\n{\n"
                                     "\tproto native void Sort(bool reverse = false);\n"
                                     "}\n");
    const graft::enf::function* f = find(*find(u, "array"), "Sort");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(f->params.size(), 1u);
    EXPECT_EQ(f->params[0].name, "reverse");
    EXPECT_EQ(f->params[0].default_value, "false");
}

TEST(EnfParse, GlobalFunctions) {
    const auto u = graft::enf::parse("proto native int KillThread(Class owner, string name);\n");
    ASSERT_EQ(u.globals.size(), 1u);
    EXPECT_EQ(u.globals[0].name, "KillThread");
    EXPECT_EQ(u.globals[0].ret, "int");
}

TEST(EnfParse, CommentsAndBodiesAreIgnored) {
    const auto u = graft::enf::parse(R"(
/**
 * \brief proto native void NotAFunction();
 */
class CGame
{
    //! proto native void AlsoNot();
    void CGame()
    {
        m_ParamCache = new array<ref Param>;   // { ; } внутри тела
        if (x) { y(); }
    }
    proto native int GetTime();
}
)");
    const graft::enf::class_* k = find(u, "CGame");
    ASSERT_NE(k, nullptr);
    ASSERT_EQ(k->functions.size(), 1u);
    EXPECT_EQ(k->functions[0].name, "GetTime");
}

TEST(EnfParse, PreprocessorLinesDoNotBreakScope) {
    const auto u = graft::enf::parse("class PlayerIdentityBase\n{\n"
                                     "#ifdef FEATURE_NETWORK_RECONCILIATION\n"
                                     "\tproto native void Possess(Pawn pawn);\n"
                                     "#endif\n"
                                     "\tproto string GetName();\n"
                                     "}\n"
                                     "class After { proto native int X(); }\n");
    ASSERT_NE(find(u, "PlayerIdentityBase"), nullptr);
    // Главное — что класс After не утонул внутри предыдущего.
    ASSERT_NE(find(u, "After"), nullptr);
}

TEST(EnfParse, ModdedAndRepeatedClassesMerge) {
    graft::enf::unit all = graft::enf::parse("class Man extends Person { proto native int A(); }");
    graft::enf::merge(all, graft::enf::parse("modded class Man { proto native int B(); }"));
    const graft::enf::class_* k = find(all, "Man");
    ASSERT_NE(k, nullptr);
    ASSERT_EQ(k->bases.size(), 1u);
    EXPECT_EQ(k->bases[0].name, "Person");
    EXPECT_EQ(k->functions.size(), 2u);
}

TEST(EnfParse, DuplicateDeclarationIsKeptOnce) {
    graft::enf::unit all = graft::enf::parse("class Man { proto native int A(); }");
    graft::enf::merge(all, graft::enf::parse("class Man { proto native int A(); }"));
    EXPECT_EQ(find(all, "Man")->functions.size(), 1u);
}


TEST(EnfParse, ConditionalClassHeadKeepsEveryBranch) {
    // 3_Game/Entities/Man.c: у класса две ветки, и склеить их в один заголовок нельзя —
    // базой стала бы Person из ветки, которой в сборке нет.
    const auto u = graft::enf::parse(R"(
#ifdef FEATURE_NETWORK_RECONCILIATION
class Man extends Person
#else
class Man extends EntityAI
#endif
{
    proto native PlayerIdentity GetIdentity();
}
)");
    const graft::enf::class_* k = find(u, "Man");
    ASSERT_NE(k, nullptr);
    // Ветку НЕ выбираем — сохраняем обе вместе с их условиями. Какая из них настоящая,
    // знает только сборка игры, и решать это будет её компилятор.
    ASSERT_EQ(k->bases.size(), 2u);
    EXPECT_EQ(k->bases[0].name, "Person");
    ASSERT_EQ(k->bases[0].when.size(), 1u);
    EXPECT_EQ(k->bases[0].when[0].name, "FEATURE_NETWORK_RECONCILIATION");
    EXPECT_FALSE(k->bases[0].when[0].negated);
    EXPECT_EQ(k->bases[1].name, "EntityAI");
    EXPECT_TRUE(k->bases[1].when[0].negated);
}

TEST(EnfParse, ColonWithoutSpaceIsStillABase) {
    // 1_Core/proto/EnEntity.c:164 — `class IEntity: Managed`. Двоеточие прилипает к
    // имени, и раньше такой класс отбрасывался ЦЕЛИКОМ: вместе с IEntity терялись
    // GetOrigin/GetID/GetName у каждой сущности мира.
    const auto u = graft::enf::parse(R"(
class IEntity: Managed
{
    proto native external vector GetOrigin();
}
)");
    const graft::enf::class_* k = find(u, "IEntity");
    ASSERT_NE(k, nullptr);
    ASSERT_EQ(k->bases.size(), 1u);
    EXPECT_EQ(k->bases[0].name, "Managed");
    ASSERT_NE(find(*k, "GetOrigin"), nullptr);
}

// ── Зеркало ─────────────────────────────────────────────────────────────────

TEST(EnfMirror, NativeMethodBecomesDirectCall) {
    graft::enf::unit u = graft::enf::parse(
        "class Man { proto native PlayerIdentity GetIdentity(); }\n"
        "class PlayerIdentity { proto string GetPlainId(); }\n");
    const std::string out = graft::enf::mirror(u, "3_Game");
    // Прямой путь для native, маршалируемый — для proto. Разницу решает генератор, а не
    // тот, кто потом это зовёт.
    EXPECT_NE(out.find("call<PlayerIdentity, \"GetIdentity\">()"), std::string::npos);
    // Возврат строки маршалируемым путём забираем ЗНАЧЕНИЕМ: буфер там движковый, и
    // сколько он живёт — его дело, а не наше.
    EXPECT_NE(out.find("proto<std::string, \"GetPlainId\">()"), std::string::npos);
}

TEST(EnfMirror, LookupHappensOnTheConcreteClass) {
    graft::enf::unit u = graft::enf::parse("class Person { proto native bool IsAlive(); }\n"
                                           "class Man extends Person { proto native int Slot(); }\n");
    const std::string out = graft::enf::mirror(u, "3_Game");
    // Методы базового класса обязаны искаться на РЕАЛЬНОМ классе объекта, иначе
    // find_method("Person", ...) не найдёт то, что объявлено ниже по цепочке. Отсюда
    // примесь, параметризованная именем: Man_<"Man"> -> Person_<"Man"> -> ref<"Man">.
    EXPECT_NE(out.find("struct Person_ :\n    graft::ref<N>\n"), std::string::npos);
    EXPECT_NE(out.find("struct Man_ :\n    Person_<N>\n"), std::string::npos);
    EXPECT_NE(out.find("struct Man : Man_<\"Man\">"), std::string::npos);
}

TEST(EnfMirror, TypesAreTranslated) {
    graft::enf::unit u = graft::enf::parse(
        "class CGame {\n"
        "  proto native void GetPlayers(out array<Man> players);\n"
        "  proto native vector GetPos(int i, float f, bool b, string s);\n"
        "}\n"
        "class Man { proto native int X(); }\n");
    const std::string out = graft::enf::mirror(u, "3_Game");
    EXPECT_NE(out.find("graft::array<Man>"), std::string::npos);
    EXPECT_NE(out.find("graft::i32"), std::string::npos);
    EXPECT_NE(out.find("graft::f32"), std::string::npos);
    EXPECT_NE(out.find("graft::vector"), std::string::npos);
    EXPECT_NE(out.find("graft::str"), std::string::npos);
}

TEST(EnfMirror, UnknownTypeDropsTheMethodNotTheClass) {
    graft::enf::unit u = graft::enf::parse("class CGame {\n"
                                           "  proto native void Ok();\n"
                                           "  proto native void Bad(SomeTypeNobodyDeclared p);\n"
                                           "}\n");
    const std::string out = graft::enf::mirror(u, "3_Game");
    EXPECT_NE(out.find("Ok()"), std::string::npos);
    // Лучше не выпустить метод, чем выпустить с неверной сигнатурой: движок зовётся по
    // ней напрямую, и расхождение — это падение, а не ошибка компиляции.
    EXPECT_EQ(out.find("Bad("), std::string::npos);
}

TEST(EnfMirror, ReservedNamesAreSkipped) {
    graft::enf::unit u = graft::enf::parse("class typename { proto volatile Class Spawn(); }\n"
                                           "class Man { proto native int X(); }\n");
    const std::string out = graft::enf::mirror(u, "1_Core");
    EXPECT_EQ(out.find("struct typename"), std::string::npos);
}

TEST(EnfMirror, EmitsForRequestedModuleOnly) {
    graft::enf::unit u = graft::enf::parse("class Only1Core { proto native int X(); }");
    u.classes[0].module = "1_Core";
    EXPECT_NE(graft::enf::mirror(u, "1_Core").find("Only1Core"), std::string::npos);
    EXPECT_EQ(graft::enf::mirror(u, "3_Game").find("struct Only1Core"), std::string::npos);
}

TEST(EnfMirror, StaticMethodsAreNotEmittedAsMembers) {
    graft::enf::unit u = graft::enf::parse(
        "class Widget { static proto bool CastTo(out Class to, Class from);\n"
        "               proto native int Id(); }");
    const std::string out = graft::enf::mirror(u, "1_Core");
    ASSERT_NE(out.find("struct Widget"), std::string::npos);
    EXPECT_NE(out.find("Id()"), std::string::npos);
    // Статический `proto` зовётся на дескрипторе класса, а не на объекте — методом
    // экземпляра его выпускать нельзя.
    EXPECT_EQ(out.find("CastTo("), std::string::npos);
}

TEST(EnfMirror, BanIsInheritedByDerivedClasses) {
    // PlayerIdentity наследует PlayerIdentityBase, у которого приватные конструктор и
    // деструктор. Наследник от этого не становится обычным Managed — запрет наследуется.
    graft::enf::unit u = graft::enf::parse(R"(
class Managed {}
class PlayerIdentityBase : Managed {
    proto string GetPlainId();
    private void PlayerIdentityBase();
    private void ~PlayerIdentityBase();
}
class PlayerIdentity : PlayerIdentityBase { proto int GetPlayerId(); }
)");
    const std::string out = graft::enf::mirror(u, "3_Game");
    const std::size_t at = out.find("struct PlayerIdentity : PlayerIdentity_<");
    ASSERT_NE(at, std::string::npos);
    const std::string tail = out.substr(at, 200);
    EXPECT_NE(tail.find("graft::lifetime::engine"), std::string::npos);
    EXPECT_NE(tail.find("script_spawnable = false"), std::string::npos);
}

}  // namespace

// ── Модель памяти и поля ────────────────────────────────────────────────────
// Всё ниже — со слов автора движка: Managed даёт поведение shared_ptr, сущности живут
// до удаления из мира, приватный деструктор запрещает удержание, приватный конструктор
// запрещает создание. Зеркало обязано донести это до C++ так, чтобы нарушение не
// собиралось, а не падало.
namespace {

TEST(EnfParse, FieldsOfClass) {
    const auto u = graft::enf::parse("class CGame\n{\n"
                                     "\tint m_DebugMonitorEnabled;\n"
                                     "\tScriptModule GameScript;\n"
                                     "\tprivate ref array<ref Param> m_ParamCache;\n"
                                     "\tproto native int GetTime();\n"
                                     "}\n");
    const graft::enf::class_* k = find(u, "CGame");
    ASSERT_NE(k, nullptr);
    ASSERT_EQ(k->fields.size(), 3u);
    EXPECT_EQ(k->fields[0].type, "int");
    EXPECT_EQ(k->fields[0].name, "m_DebugMonitorEnabled");
    EXPECT_EQ(k->fields[1].name, "GameScript");
    EXPECT_EQ(k->fields[2].name, "m_ParamCache");
}

TEST(EnfParse, PrivateConstructorAndDestructorAreNoticed) {
    // 1_Core/proto/EnScript.c: «This is a C++ managed class, so script has no business
    // managing the lifetime».
    const auto u = graft::enf::parse("class PlayerIdentityBase : Managed\n{\n"
                                     "\tproto string GetName();\n"
                                     "\tprivate void PlayerIdentityBase();\n"
                                     "\tprivate void ~PlayerIdentityBase();\n"
                                     "}\n");
    const graft::enf::class_* k = find(u, "PlayerIdentityBase");
    ASSERT_NE(k, nullptr);
    EXPECT_TRUE(k->private_ctor);
    EXPECT_TRUE(k->private_dtor);
}

TEST(EnfParse, ConstructorWithBodyIsNotAField) {
    const auto u = graft::enf::parse("class CGame\n{\n"
                                     "\tint m_Value;\n"
                                     "\tvoid CGame()\n\t{\n\t\tm_Value = 1;\n\t}\n"
                                     "\tproto native int GetTime();\n"
                                     "}\n");
    const graft::enf::class_* k = find(u, "CGame");
    ASSERT_EQ(k->fields.size(), 1u);
    EXPECT_EQ(k->fields[0].name, "m_Value");
}

TEST(EnfMirror, LifetimeFollowsTheBaseChain) {
    graft::enf::unit u = graft::enf::parse(
        "class Managed {}\n"
        "class Object {}\n"
        "class EntityAI extends Object { proto native int GetID(); }\n"
        "class Man extends EntityAI { proto native int Slot(); }\n"
        "class Widget : Managed { proto native int Id(); }\n"
        "class Plain { proto native int Id(); }\n");
    const std::string out = graft::enf::mirror(u, "3_Game");
    EXPECT_NE(out.find("struct Man : Man_<\"Man\"> {\n    static constexpr graft::lifetime "
                       "script_lifetime = graft::lifetime::entity;"),
              std::string::npos);
    EXPECT_NE(out.find("struct Widget : Widget_<\"Widget\"> {\n    static constexpr "
                       "graft::lifetime script_lifetime = graft::lifetime::managed;"),
              std::string::npos);
    EXPECT_NE(out.find("struct Plain : Plain_<\"Plain\"> {\n    static constexpr "
                       "graft::lifetime script_lifetime = graft::lifetime::plain;"),
              std::string::npos);
}

TEST(EnfMirror, PrivateDestructorMeansEngineOwned) {
    graft::enf::unit u = graft::enf::parse(
        "class PlayerIdentity : Managed {\n"
        "  proto string GetPlainId();\n"
        "  private void PlayerIdentity();\n"
        "  private void ~PlayerIdentity();\n"
        "}\n");
    const std::string out = graft::enf::mirror(u, "3_Game");
    EXPECT_NE(out.find("script_lifetime = graft::lifetime::engine;"), std::string::npos);
    EXPECT_NE(out.find("script_spawnable = false;"), std::string::npos);
}

TEST(EnfMirror, FieldsBecomeProxyAccessors) {
    graft::enf::unit u = graft::enf::parse("class CGame {\n"
                                           "  int m_DebugMonitorEnabled;\n"
                                           "  proto native int GetTime();\n"
                                           "}\n");
    const std::string out = graft::enf::mirror(u, "3_Game");
    EXPECT_NE(out.find("graft::field_proxy<CGame_<N>, \"m_DebugMonitorEnabled\"> "
                       "m_DebugMonitorEnabled() const"),
              std::string::npos);
}

TEST(EnfMirror, FieldLosesToMethodOfTheSameName) {
    // В C++ нет перегрузки по возвращаемому типу — кто-то должен уступить, и уступает
    // поле: метод несёт сигнатуру, а поле выражается через obj["имя"_f].
    graft::enf::unit u = graft::enf::parse("class Node {\n"
                                           "  int Id;\n"
                                           "  proto native int Id();\n"
                                           "}\n");
    const std::string out = graft::enf::mirror(u, "3_Game");
    EXPECT_EQ(out.find("field_proxy<Node_<N>, \"Id\">"), std::string::npos);
}

TEST(EnfMirror, EveryBranchIsBakedInAsUnion) {
    // Дефайны ЗАПУЩЕННОЙ игры узнать неоткуда, поэтому ветка не выбирается: запекается
    // объединение. Первую цепочку наследуем, методы остальных дописываем прямо в примесь,
    // а чего в этой сборке нет — промахнётся в рантайме.
    graft::enf::unit u = graft::enf::parse(R"(
class Managed {}
class Person : Managed { proto native int OnlyInPerson(); }
class EntityAI : Managed { proto native int OnlyInEntity(); }
#ifdef FEATURE_NETWORK_RECONCILIATION
class Man extends Person
#else
class Man extends EntityAI
#endif
{
    proto native int Slot();
#ifdef FEATURE_NETWORK_RECONCILIATION
    proto native void Possess(int pawn);
#endif
}
)");
    const std::string out = graft::enf::mirror(u, "3_Game");
    // Ни одного условия в выпущенном коде.
    EXPECT_EQ(out.find("\n#if"), std::string::npos);
    // Метод из ветки, которой может не быть, всё равно объявлен.
    EXPECT_NE(out.find("void Possess("), std::string::npos);
    // Наследуется первая цепочка...
    EXPECT_NE(out.find("struct Man_ :\n    Person_<N>\n"), std::string::npos);
    // ...а метод второй дописан в саму примесь.
    EXPECT_NE(out.find("graft::i32 OnlyInEntity() const;"), std::string::npos);
}

TEST(EnfMirror, LifetimeTakesTheStrictestBranch) {
    // У Man одна ветка даёт сущность мира, другая — обычный класс. Берём строгую: она
    // только ЗАПРЕЩАЕТ лишнее (держать между тиками), разрешить лишнего не может.
    graft::enf::unit u = graft::enf::parse(R"(
class Managed {}
class Object {}
class Person : Managed { proto native int A(); }
class EntityAI extends Object { proto native int B(); }
#ifdef FEATURE_NETWORK_RECONCILIATION
class Man extends Person
#else
class Man extends EntityAI
#endif
{ proto native int Slot(); }
)");
    const std::string out = graft::enf::mirror(u, "3_Game");
    // Ищем НАСТОЯЩЕЕ объявление, а не пример в шапке файла.
    const std::size_t at = out.find("\nstruct Man : Man_<");
    ASSERT_NE(at, std::string::npos);
    EXPECT_NE(out.substr(at, 200).find("graft::lifetime::entity"), std::string::npos)
        << out.substr(at, 200);
}

}  // namespace
