// Тесты генерации скриптовой стороны: из C++ сигнатуры должно получаться ровно то
// объявление, которое компилятор Enforce ждёт увидеть в PBO.
#include <gtest/gtest.h>

#include <string>

#include "graft/native.hpp"

namespace {

graft::i32 DemoPing(graft::i32 token) {
    return token;
}

bool DemoAll(graft::f32 a, graft::str s, graft::vec3 v, graft::obj o) {
    return a > 0 && !s.empty() && v.x == 0 && static_cast<bool>(o);
}

graft::owned DemoText() {
    return {"x"};
}

// Скриптовый класс отражён классом C++: статический метод — обычная статическая функция,
// обычный — обычный метод, и объект в объявление скрипта не попадает.
struct DemoClass : graft::script_object<"DemoClass"> {
    static graft::i32 Twice(graft::i32 a) { return a; }
    void Scale(graft::f32 k) const { (void)k; }
};

// Класс с ПОЛЕМ: экземпляр живёт вместе со скриптовым объектом. В объявлении от этого
// не появляется ничего — о смерти объекта библиотека узнаёт от движка, а не от скрипта.
struct DemoState : graft::script_object<"DemoState"> {
    graft::i32 hits = 0;
    void Bump() { ++hits; }
};

// graft::value в сигнатуре означает «любой тип» и печатается как void — так же, как
// у ванильных proto void Print(void var) и Serializer.Write(void value_in).
// Смешивать с обычными типами можно: маршалируемый вызов везёт ВСЕ аргументы блоком
// с тегами, поэтому объявленный тип на форму вызова не влияет.
bool DemoAny(graft::value v) {
    return !v.empty();
}

// Строка рядом с value: маршалируемый путь обязан принимать graft::str, иначе
// «любой тип» нельзя смешать с обычной строкой.
bool DemoText2(graft::str name, graft::value payload) {
    return !name.empty() && !payload.empty();
}

bool DemoMixed(graft::i32 n, graft::obj o, graft::vec3 v, graft::value payload) {
    return n > 0 && static_cast<bool>(o) && v.x == 0 && !payload.empty();
}

struct DemoStatics : graft::script_object<"DemoStatics"> {
    static bool Any(graft::value v) { return !v.empty(); }
};

GRAFT_BINDINGS("1_Core") {
    bind.global<&DemoAny>("DemoAny").global<&DemoMixed>("DemoMixed").global<&DemoText2>("DemoText2");
    bind.class_<DemoStatics>().static_method<&DemoStatics::Any>("Any");
    bind.global<&DemoPing>("DemoPing")
        .global<&DemoAll>("DemoEverything")
        .global<&DemoText>("DemoText");
    bind.class_<DemoClass>()
        .static_method<&DemoClass::Twice>("Twice")
        .method<&DemoClass::Scale>("Scale");
    bind.class_<DemoState>().method<&DemoState::Bump>("Bump");
}

const graft::native& find(const char* name) {
    for (const graft::native* n = graft::natives(); n; n = n->next) {
        if (std::string(n->name) == name) {
            return *n;
        }
    }
    ADD_FAILURE() << "натив не зарегистрирован: " << name;
    static const graft::native missing{};
    return missing;
}

TEST(Proto, MapsScalarTypes) {
    EXPECT_EQ(graft::proto_decl(find("DemoPing")), "proto native int DemoPing(int p0);");
}

TEST(Proto, MapsEverySupportedType) {
    EXPECT_EQ(graft::proto_decl(find("DemoEverything")),
              "proto native bool DemoEverything(float p0, string p1, vector p2, Class p3);");
}

TEST(Proto, OwnedStringReturn) {
    EXPECT_EQ(graft::proto_decl(find("DemoText")), "proto native owned string DemoText();");
}

// Глобальный натив с graft::value обязан объявляться маршалируемым (`proto`, не
// `proto native`) — форма вызова у него другая, и перепутать их значит упасть.
TEST(Proto, GlobalWithValueIsMarshalled) {
    EXPECT_EQ(graft::proto_decl(find("DemoAny")), "proto bool DemoAny(void p0);");
    EXPECT_TRUE(find("DemoAny").marshalled);
    EXPECT_FALSE(find("DemoPing").marshalled);
}

// Точная типизация там, где она есть, и «любой» только там, где нужен.
TEST(Proto, MarshalledArgsCanBeMixed) {
    EXPECT_EQ(graft::proto_decl(find("DemoMixed")),
              "proto bool DemoMixed(int p0, Class p1, vector p2, void p3);");
    EXPECT_TRUE(find("DemoMixed").marshalled);
}

TEST(Proto, MarshalledAcceptsEngineString) {
    EXPECT_EQ(graft::proto_decl(find("DemoText2")), "proto bool DemoText2(string p0, void p1);");
    EXPECT_TRUE(find("DemoText2").marshalled);
}

TEST(Proto, StaticMethodWithValueIsMarshalled) {
    EXPECT_EQ(graft::proto_decl(find("Any")), "static proto bool Any(void p0);");
    EXPECT_TRUE(find("Any").marshalled);
}

TEST(Proto, StaticMethodKeepsAllArgs) {
    EXPECT_EQ(graft::proto_decl(find("Twice")), "static proto native int Twice(int p0);");
}

TEST(Proto, MemberMethodDropsSelf) {
    EXPECT_EQ(graft::proto_decl(find("Scale")), "proto native void Scale(float p0);");
}

TEST(Proto, FileGroupsMethodsIntoModdedClass) {
    const std::string file = graft::proto_file();
    EXPECT_NE(file.find("proto native int DemoPing(int p0);"), std::string::npos);
    const std::size_t cls = file.find("modded class DemoClass\n{\n");
    ASSERT_NE(cls, std::string::npos);
    EXPECT_LT(file.find("proto native int DemoPing"), cls);  // глобальные — до классов
    EXPECT_NE(file.find("    static proto native int Twice(int p0);"), std::string::npos);
    EXPECT_NE(file.find("    proto native void Scale(float p0);"), std::string::npos);
}


// У класса с состоянием в объявлении — только его методы: ни служебного натива, ни
// деструктора. Освобождение живёт целиком на стороне C++.
TEST(Proto, ClassWithStateDeclaresOnlyItsMethods) {
    const std::string file = graft::proto_file();
    const std::size_t cls = file.find("modded class DemoState\n{\n");
    ASSERT_NE(cls, std::string::npos);
    EXPECT_NE(file.find("    proto native void Bump();"), std::string::npos);
    EXPECT_EQ(file.find("NativeDispose"), std::string::npos);
    EXPECT_EQ(file.find("~DemoState"), std::string::npos);
}

}  // namespace
