// Кодировщик строчек ассемблера: INCLUDE/graft/asm.hpp.
//
// Здесь проверяется МЕХАНИКА кодирования — места, где кодировщик легко соврал бы, а сверить
// было бы нечем: префикс REX, направление операндов, SIB, ширина смещения, дырка. Байты
// снимаются с дизассемблера (`python RESEARCH/scripts/pe.py`) либо из SDM.
//
// Сверка идёт на КОМПИЛЯЦИИ, поэтому почти весь файл — static_assert. Кейсы gtest ниже нужны
// для другого: чтобы прогон сьюты показал, что файл вообще собран и посчитан.
#include <gtest/gtest.h>

#include <cstdint>

#include "graft/asm.hpp"
#include "graft/scan.hpp"

namespace {

using graft::scan::code;
using graft::scan::sig;

// ── Механика кодирования ─────────────────────────────────────────────────────────────
// Кейсы ниже не из движка: это места, где кодировщик легко соврал бы, а сверить было бы
// нечем. Байты снимаются с дизассемблера (`python RESEARCH/scripts/pe.py`) либо из SDM.

// REX появляется ровно когда надо: 64-битный операнд, регистр r8..r15, младший байт
// rsp/rbp/rsi/rdi (без префикса те же коды значат ah/ch/dh/bh).
static_assert(code<"mov eax,ecx"> == sig<"8B C1">);    // REX не нужен вовсе
static_assert(code<"mov rax,rcx"> == sig<"48 8B C1">); // только W
static_assert(code<"mov eax,r8d"> == sig<"41 8B C0">); // только B
static_assert(code<"mov r8d,eax"> == sig<"44 8B C0">); // только R
static_assert(code<"mov r9,r10"> == sig<"4D 8B CA">);  // W, R и B сразу
static_assert(code<"xor sil,sil"> == sig<"40 32 F6">); // пустой REX ради sil
static_assert(code<"xor bl,bl"> == sig<"32 DB">);      // у bl он не нужен

// Направление: приёмник-регистр читает (8B), приёмник-память пишет (89). Компилятор игры
// пишет так же, и перепутать их — значит получить чужую инструкцию с тем же текстом.
static_assert(code<"mov rbx,rcx"> == sig<"48 8B D9">);
static_assert(code<"mov [rcx+8],rbx"> == sig<"48 89 59 08">);

// rsp и r12 адресуются только через SIB — байт 24 после modrm.
static_assert(code<"mov [rsp+8],rbx"> == sig<"48 89 5C 24 08">);
static_assert(code<"mov [r12+8],rbx"> == sig<"49 89 5C 24 08">);

// [rbp] без смещения не кодируется: этот код занят формой rip+disp32, поэтому пишется
// disp8 = 0. Ровно так же поступает компилятор.
static_assert(code<"mov rax,[rbp]"> == sig<"48 8B 45 00">);
static_assert(code<"mov rax,[rcx]"> == sig<"48 8B 01">);

// Смещение берётся коротким, пока влезает, — и это не вкусовщина: компилятор игры делает
// то же, а сигнатура обязана совпасть с ним байт в байт.
static_assert(code<"mov rax,[rcx+7Fh]"> == sig<"48 8B 41 7F">);
static_assert(code<"mov rax,[rcx+80h]"> == sig<"48 8B 81 80 00 00 00">);

// Непосредственное — тоже короткое, пока влезает; у аккумулятора сверх того своя форма
// без modrm, и компилятор её использует.
static_assert(code<"sub rsp,30h"> == sig<"48 83 EC 30">);
static_assert(code<"cmp eax,5"> == sig<"83 F8 05">);
static_assert(code<"cmp eax,200h"> == sig<"3D 00 02 00 00">);
static_assert(code<"cmp ecx,200h"> == sig<"81 F9 00 02 00 00">);

// Запись в память непосредственным: ширину из операндов не вывести, её пишут явно.
static_assert(code<"mov dword [rcx+38h],5"> == sig<"C7 41 38 05 00 00 00">);
static_assert(code<"mov dword ptr [rcx+38h],5"> == sig<"C7 41 38 05 00 00 00">);

// Разбор текста: формы числа, комментарии, пустые строки, отступы. Мнемоники и регистры
// пишутся строчными — так их печатает отладчик, и второй формы кодировщик не знает.
static_assert(code<"mov rax,[rcx+0x2B8]"> == sig<"48 8B 81 B8 02 00 00">);
static_assert(code<"mov rax,[rcx+696]"> == sig<"48 8B 81 B8 02 00 00">);
static_assert(code<R"(
    ; целиком комментарий

    push rbp    ; и хвостом тоже
)"> == sig<"55">);

// Отрицательное смещение — нормальный x86: так адресуются локали от rbp.
static_assert(code<"mov rax,[rbp-8]"> == sig<"48 8B 45 F8">);

// jcc: в сигнатуре важен байт смещения, а не куда он ведёт.
static_assert(code<"jne +5"> == sig<"75 05">);
static_assert(code<"jmp -2"> == sig<"EB FE">);

// db берёт и джокер: «здесь любой байт» — то же, что ?? в байтовой записи.
static_assert(code<"db 48 ?? 81"> == sig<"48 ?? 81">);

// ── reg64: любой регистр-приёмник ────────────────────────────────────────────────────
// Отпускаются поле reg в modrm и бит R в префиксе REX — и только они: форма адресации
// (mod и rm) сверяется по-прежнему, иначе под сигнатуру подойдёт другая инструкция.
static_assert(code<"lea reg64,[rip+disp32]"> == sig<"48&FB 8D 05&C7 [disp32]">);
static_assert(code<"mov reg64,[rcx+disp8]"> == sig<"48&FB 8B 41&C7 [disp8]">);
static_assert(code<"mov [rip+disp32],reg64"> == sig<"48&FB 89 05&C7 [disp32]">);

// И это действительно находит ВСЕ шестнадцать регистров, а не младшие восемь. Ровно на
// этом спотыкался рукописный скан в defines.cpp: REX он сверял целиком.
TEST(Asm, AnyRegisterFindsHighRegistersToo) {
    constexpr auto lea_any = code<"lea reg64,[rip+disp32]">;
    // lea rax,[rip+..] | lea rdx,[rip+..] | lea r14,[rip+..] — REX 48, 48, 4C
    static const std::uint8_t rax[] = {0x48, 0x8D, 0x05, 0, 0, 0, 0};
    static const std::uint8_t rdx[] = {0x48, 0x8D, 0x15, 0, 0, 0, 0};
    static const std::uint8_t r14[] = {0x4C, 0x8D, 0x35, 0, 0, 0, 0};
    // А это уже ДРУГАЯ форма адресации: lea rax,[rcx+8]. Под `??` она бы подошла.
    static const std::uint8_t mem[] = {0x48, 0x8D, 0x41, 0x08, 0, 0, 0};

    EXPECT_TRUE(graft::scan::matches(reinterpret_cast<std::uintptr_t>(rax), lea_any));
    EXPECT_TRUE(graft::scan::matches(reinterpret_cast<std::uintptr_t>(rdx), lea_any));
    EXPECT_TRUE(graft::scan::matches(reinterpret_cast<std::uintptr_t>(r14), lea_any));
    EXPECT_FALSE(graft::scan::matches(reinterpret_cast<std::uintptr_t>(mem), lea_any));
}

// Длина инструкции от регистра зависеть не должна, поэтому `reg32` и `reg8` — ошибка
// сборки: без REX.W у r8d..r15d префикс есть, у eax..edi нет. Кейса на это быть не может,
// файл бы не собрался; проверено руками.

// Константы самого сканера записаны байтами (graft/scan.hpp стоит НИЖЕ кодировщика) —
// сверяем, что мнемоника в их комментарии не разошлась с байтами.
static_assert(code<"lea rdx,[rip+disp32]"> == graft::scan::lea_rdx);
static_assert(code<"lea r8,[rip+disp32]"> == graft::scan::lea_r8);

// Дырка в ассемблерной записи — та же дырка: её ширина и место известны.
static_assert(code<"mov rax,[rcx+disp32]">.hole == 3);
static_assert(code<"mov rax,[rcx+disp32]">.hole_size == 4);
static_assert(code<"mov eax,[rcx+disp8]">.hole_size == 1);
static_assert(code<"mov [rsp+8],rbx">.hole_size == 0);

// Неизвестная инструкция, неизвестная форма, две дырки, адресация с индексом — всё это
// ОШИБКИ СБОРКИ, поэтому кейсов на них здесь нет и быть не может: файл бы не собрался.
// Проверено руками по одному разу на каждую, текст ошибки ведёт в graft/asm.hpp.

} // namespace

// Прогон сьюты должен показывать, что файл собран и сверки посчитаны: static_assert в
// отчёте ctest не виден, а молчаливо выпавший из сборки файл — видная беда.
TEST(Asm, EncoderChecksAreCompiled) {
    // Если файл скомпилировался, все сверки выше уже сошлись. Кейс сторожит обратное: что он
    // вообще попал в сборку.
    EXPECT_EQ(code<"mov rax,[rcx+disp32]">.size(), 7u);
    EXPECT_EQ(code<"lock xadd [rcx+disp8],eax">.size(), 5u);
}

// Сигнатура из строчек ассемблера ищется ровно так же, как байтовая: тип у них один.
TEST(Asm, AssembledSignatureWorksWithFind) {
    // nop ; mov eax,[rcx+38h] — поле по короткому смещению, дырка в один байт.
    static const std::uint8_t body[] = {0x90, 0x8B, 0x41, 0x38};
    const auto                ea     = reinterpret_cast<std::uintptr_t>(body);

    const auto at = graft::scan::find(ea, sizeof body, code<"mov eax,[rcx+disp8]">);
    ASSERT_TRUE(at.has_value());
    EXPECT_EQ(at->site, ea + 1);
    EXPECT_EQ(at->value, 0x38);
}
