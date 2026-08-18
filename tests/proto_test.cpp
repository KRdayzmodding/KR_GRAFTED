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

GRAFT_BINDINGS("1_Core") {
    bind.global<&DemoPing>("DemoPing")
        .global<&DemoAll>("DemoEverything")
        .global<&DemoText>("DemoText");
    bind.class_<DemoClass>()
        .static_method<&DemoClass::Twice>("Twice")
        .method<&DemoClass::Scale>("Scale");
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

}  // namespace
