// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "graft/name.hpp"

// Поиск движковых точек регистрации нативов в загруженном образе — без единого
// зашитого адреса. Якоря — имена ванильных нативов и скриптовых классов: они часть
// публичного script-API Enforce, поэтому переживают патчи игры (в отличие от RVA).
//
// Найденный код (см. re/out/*/RegisterCoreNatives.c):
//   RegisterGlobal(ctx, "MemoryValidation", impl, 0)      -> lea rdx, [rip+имя]; ... call
//   RegisterMethod(ctx, cls, "GetNumberOfSetBits", ...)   -> lea r8,  [rip+имя]; ... call
//   FindClass(ctx, "Math")                                -> lea rdx, [rip+имя]; ... call
//
// Алгоритм 1:1 повторён в re/scripts/discover.py — сухой прогон по exe в IDA
// сверяет результат с частотным анализом natives.py (обе цели, 1.29: совпало).
namespace graft::scan {

// ── Сверка найденного: сигнатура так, как её показывает отладчик ──────────────
// Ход по call-графу даёт КАНДИДАТА; кандидат становится ответом только когда его байты
// совпали с сигнатурой. Так «нашлось не то» превращается в отказ, а не в прыжок в середину
// чужого кода. Из тех же байт берутся и смещения полей движка: они запечены в инструкциях,
// которые нас к функции и привели.
//
// Сигнатура пишется ОДНИМ способом — строкой, скопированной из отладчика:
//
//   constexpr auto prologue = scan::sig<"48 89 5C 24 ?? 57">;   // mov [rsp+?],rbx; push rdi
//   constexpr auto field    = scan::sig<"48 8B 81 [disp32]">;   // mov rax,[rcx+disp32]
//
// Четыре вида токена, и больше никаких:
//
//   48          байт как есть
//   ??   ?      любой байт: там регистр или смещение, которое сверять нечем
//   05&C7       ЧАСТЬ байта: сверяются биты маски, остальные любые. Так пишется «поле reg
//               в modrm какое угодно, а mod и rm вот такие» — целиком байт здесь сверять
//               нельзя, а целиком отпускать нельзя тем более
//   [disp32]    ДЫРКА: столько бит любых — и это ОТВЕТ. Слово в скобках для читателя,
//               парсер берёт из него только число — ширину в битах: [disp32], [imm32]
//               и [32] — одно и то же, [disp8] — дырка в байт
//
// ЗАЧЕМ ДЫРКА, А НЕ ВТОРОЙ ВЫЗОВ. Раньше ширину смещения держало имя функции — `disp8_of`
// против `disp32_of`, — а какая нужна, было написано в комментарии рядом с опкодом. Два
// места, которые обязаны совпасть, и расхождение не заметит ни компилятор, ни тест: код
// прочитает четыре байта там, где лежит один, и получит начало следующей инструкции в
// старших разрядах. Теперь ширина живёт в сигнатуре, ровно там, где отладчик её и печатает.
//
// Строка разбирается на КОМПИЛЯЦИИ, поэтому опечатка («4» вместо «48», `???`, дырка без
// ширины, посторонний знак) — ошибка сборки, а не сигнатура, которая молча ничего не
// находит и отлаживается уже на живом сервере. Лишние пробелы законны: строку копируют из
// отладчика, а он выравнивает столбцы.

// Байт сверяется ПО МАСКЕ: совпасть обязаны биты, поднятые в mask. Маска 0xFF — байт как
// есть, 0 — любой, что-то между — часть байта (поле reg в modrm, бит R в префиксе REX).
//
// Третьего представления у «любого байта» нет намеренно: пока джокер был отдельным знаком
// (-1), маскированный байт выразить было нечем, и каждое место, которому он нужен, писало
// свой цикл по байтам.
template <std::size_t N>
struct pattern {
    std::array<std::uint8_t, N> value{};
    std::array<std::uint8_t, N> mask{};
    std::uint8_t                hole      = 0; // смещение дырки от начала, в байтах
    std::uint8_t                hole_size = 0; // её ширина в байтах; 0 — дырки нет

    static constexpr std::size_t size() { return N; }

    // Сверяется ли байт хоть в чём-нибудь. Нужно тестам и человеку, а не поиску.
    constexpr bool checked(std::size_t i) const { return mask[i] != 0; }
};

// Две записи ОДНОГО И ТОГО ЖЕ сличаются целиком: строчки ассемблера (`scan::code`, см.
// graft/asm.hpp) против золотых байт, снятых с бинаря. Разная длина — не ошибка сборки, а
// честное «не равно»: иначе у static_assert не осталось бы текста, который можно прочесть.
template <std::size_t A, std::size_t B>
constexpr bool operator==(const pattern<A>& a, const pattern<B>& b) {
    if constexpr (A != B) {
        return false;
    } else {
        return a.value == b.value && a.mask == b.mask && a.hole == b.hole &&
               a.hole_size == b.hole_size;
    }
}

// Та же сигнатура без длины в типе — так её принимают функции ниже. Неявно: на месте
// вызова пишется `scan::sig<"...">`, и лишнего слова там быть не должно.
struct pattern_view {
    std::span<const std::uint8_t> value;
    std::span<const std::uint8_t> mask;
    std::uint8_t                  hole      = 0;
    std::uint8_t                  hole_size = 0;

    template <std::size_t N>
    constexpr pattern_view(const pattern<N>& p)
        : value{p.value}, mask{p.mask}, hole{p.hole}, hole_size{p.hole_size} {}
};

namespace detail {

consteval int sig_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    // consteval бросать не умеет — и это ровно то, что нужно: вызов перестаёт быть
    // константным выражением, и компилятор показывает эту строку в цепочке ошибки.
    throw "в сигнатуре бывают только шестнадцатеричные байты, ?? и пробелы";
}

// Байт сигнатуры: значение и маска сверяемых бит.
struct sig_cell {
    std::uint8_t value = 0;
    std::uint8_t mask  = 0; // 0 — байт любой
};

consteval std::uint8_t sig_hex2(std::string_view token) {
    if (token.size() != 2) {
        throw "байт сигнатуры — две шестнадцатеричные цифры, ?? либо дырка [disp32]";
    }
    return static_cast<std::uint8_t>((sig_nibble(token[0]) * 16) + sig_nibble(token[1]));
}

consteval sig_cell sig_byte(std::string_view token) {
    if (token == "??" || token == "?") {
        return {}; // джокер: там смещение поля или регистр, выбранный компилятором игры
    }
    // `05&C7` — сверить только биты маски. Так пишется «поле reg в modrm любое».
    if (const std::size_t amp = token.find('&'); amp != std::string_view::npos) {
        const std::uint8_t value = sig_hex2(token.substr(0, amp));
        const std::uint8_t mask  = sig_hex2(token.substr(amp + 1));
        if (mask == 0) {
            throw "маска 00 ничего не сверяет — это просто ??";
        }
        if ((value & ~mask) != 0) {
            throw "в значении подняты биты вне маски: сверять их всё равно нечем";
        }
        return {value, mask};
    }
    return {sig_hex2(token), 0xFF};
}

// Границы следующего токена: пробелы пропущены, `[...]` взят целиком. Пустой — конец строки.
struct sig_token {
    std::string_view text;
    std::size_t      next = 0;
};

consteval sig_token sig_next(std::string_view s, std::size_t i) {
    while (i < s.size() && s[i] == ' ') {
        ++i;
    }
    if (i >= s.size()) {
        return {{}, i};
    }
    std::size_t j = i;
    if (s[i] == '[') {
        while (j < s.size() && s[j] != ']') {
            ++j;
        }
        if (j == s.size()) {
            throw "дырка без закрывающей скобки";
        }
        ++j;
    } else {
        while (j < s.size() && s[j] != ' ') {
            ++j;
        }
    }
    return {s.substr(i, j - i), j};
}

// Ширина дырки в байтах. Читается ЧИСЛО внутри скобок, остальное там — слово для читателя,
// поэтому [disp32], [imm32] и [32] значат одно и то же.
consteval std::size_t sig_hole(std::string_view token) {
    unsigned bits = 0;
    bool     seen = false;
    for (const char c : token) {
        if (c >= '0' && c <= '9') {
            bits = (bits * 10) + static_cast<unsigned>(c - '0');
            seen = true;
        }
    }
    if (!seen) {
        throw "в дырке нужна ширина: [disp32], [imm8], [64]";
    }
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64) {
        throw "ширина дырки — 8, 16, 32 или 64 бита";
    }
    return bits / 8;
}

consteval std::size_t sig_size(std::string_view text) {
    std::size_t n = 0;
    for (std::size_t i = 0;;) {
        const sig_token t = sig_next(text, i);
        if (t.text.empty()) {
            return n;
        }
        n += t.text.front() == '[' ? sig_hole(t.text) : 1;
        i = t.next;
    }
}

template <name_t S>
consteval auto parse_sig() {
    constexpr std::string_view text{S.value};
    constexpr std::size_t      n = sig_size(text);
    static_assert(n > 0, "пустая сигнатура");
    // Смещение дырки хранится байтом; сигнатуры длиннее этого не бывает, но молча обрезать
    // его нельзя.
    static_assert(n <= 255, "сигнатура длиннее 255 байт");
    pattern<n>  out;
    std::size_t k = 0;
    for (std::size_t i = 0;;) {
        const sig_token t = sig_next(text, i);
        if (t.text.empty()) {
            return out;
        }
        i = t.next;
        if (t.text.front() != '[') {
            const sig_cell cell = sig_byte(t.text);
            out.value[k]        = cell.value;
            out.mask[k]         = cell.mask;
            ++k;
            continue;
        }
        if (out.hole_size != 0) {
            throw "дырка в сигнатуре бывает одна: искомое смещение в инструкции одно";
        }
        out.hole      = static_cast<std::uint8_t>(k);
        out.hole_size = static_cast<std::uint8_t>(sig_hole(t.text));
        k += out.hole_size; // байты дырки не сверяются: маска у них и осталась нулевой
    }
}

} // namespace detail

template <name_t S>
inline constexpr auto sig = detail::parse_sig<S>();

// На этом адресе лежит ровно это? Нечитаемый адрес — false, а не падение: кандидат мог
// приехать из мусорного смещения, и сверка обязана это выдержать.
bool matches(std::uintptr_t ea, pattern_view sig);

// Где нашлось и что лежало в дырке.
struct found {
    std::uintptr_t site  = 0; // адрес самой инструкции
    std::int64_t   value = 0; // содержимое дырки; 0, если дырки в сигнатуре не было
};

// `nth`-е вхождение сигнатуры в [ea, ea+span). Пусто — не нашлось, и это отказ, а не повод
// идти дальше по догадке.
//
// Дырка читается СО ЗНАКОМ, как её понимает процессор: disp8 0x80 — это -128, а не +128.
// Поля движка, которыми мы пользуемся, все положительные и короткие, но читать их
// неправильно нельзя и в этом случае.
std::optional<found> find(std::uintptr_t ea, std::size_t span, pattern_view sig, unsigned nth = 1);

// Секция отображённого образа: байты + адрес, по которому они лежат в памяти.
struct view {
    std::span<const std::uint8_t> bytes;        // сама секция: указатель и длина вместе
    std::uintptr_t                base = 0;     // адрес bytes[0]
    bool                          exec = false; // исполняемая: в таких ищем инструкции

    // Адрес C-строки ровно text (не подстроки), начиная с ea. 0 — нет.
    std::uintptr_t find_cstr(const char* text, std::uintptr_t from = 0) const;
    // `nth`-е вхождение сигнатуры в [ea, ea+span) ЭТОЙ секции. То же, что свободная
    // scan::find, но по своим байтам: у секции они уже есть, и спрашивать у системы,
    // отображена ли страница, незачем. Окно обрезается концом секции.
    //
    // Это не удобство: `base` у секции бывает и синтетическим (так собраны фикстуры
    // сьюты), и тогда свободная find честно отвечает «адрес не читается».
    std::optional<struct found> find(std::uintptr_t ea, std::size_t span, pattern_view sig, unsigned nth = 1) const;

    // Адрес инструкции, которая через rip ссылается на target: `lea reg,[rip+d]`,
    // `mov [rip+d],reg` — любая. Сигнатура берётся ЦЕЛИКОМ, вместе с дыркой под смещение:
    // rip смотрит за конец инструкции, поэтому нужна и её длина, и само смещение.
    // Единственная реализация этого поиска на весь проект.
    std::uintptr_t find_rip(pattern_view insn, std::uintptr_t target, std::uintptr_t from = 0) const;
    // Цели всех rel32-переходов с таким опкодом после/до ea в пределах span байт, по
    // порядку адресов: 0xE8 — call, 0xE9 — хвостовой jmp. Их несколько, потому что байт
    // опкода может встретиться и внутри чужого смещения — отсеивает уже вызывающий код
    // (цель обязана лежать в исполняемой секции, см. rel32_targets).
    std::vector<std::uintptr_t> rel32_after(std::uintptr_t ea, std::size_t span, std::uint8_t opcode) const;
    std::vector<std::uintptr_t> rel32_before(std::uintptr_t ea, std::size_t span, std::uint8_t opcode) const;

    std::vector<std::uintptr_t> calls_after(std::uintptr_t ea, std::size_t span = 0x40) const {
        return rel32_after(ea, span, 0xE8);
    }

    std::vector<std::uintptr_t> calls_before(std::uintptr_t ea, std::size_t span = 0x40) const {
        return rel32_before(ea, span, 0xE8);
    }

    bool contains(std::uintptr_t ea) const { return ea >= base && ea - base < bytes.size(); }
};

// Чем движок передаёт имя в регистрацию: 2-й аргумент fastcall — rdx, 3-й — r8. Записаны
// байтами, а не строчками ассемблера, только из-за слоёв: кодировщик (graft/asm.hpp) стоит
// НАД этим заголовком и включить его здесь нельзя. Что мнемоника в комментарии не разошлась
// с байтами, сторожит static_assert в tests/asm_test.cpp.
inline constexpr auto lea_rdx = sig<"48 8D 15 [disp32]">; // lea rdx,[rip+disp32]
inline constexpr auto lea_r8  = sig<"4C 8D 05 [disp32]">; // lea r8,[rip+disp32]

// Точки движка. Сигнатуры выведены из декомпиляции (re/out/server/reg_*.c):
// последний числовой аргумент — размер буфера возврата, для `proto native` он 0.
using reg_global_fn = void*(__fastcall*)(void* ctx, const char* name, void* impl, unsigned ret_buf);
using reg_method_fn = void*(__fastcall*)(void* ctx, void* cls, const char* name, void* impl, unsigned ret_buf, char create);
using find_class_fn = void*(__fastcall*)(void* ctx, const char* name);

struct api {
    reg_global_fn register_global = nullptr;
    reg_method_fn register_method = nullptr;
    find_class_fn find_class      = nullptr;

    explicit operator bool() const { return register_global && register_method && find_class; }
};

// Голосование нескольких якорей: случайный байт 0xE8 в чужом смещении может дать
// ложный call, но совпасть у трёх разных якорей он не может.
std::uintptr_t vote(const std::vector<view>& sections, pattern_view insn, const char* const* anchors, bool before = false, const std::uintptr_t* reject = nullptr, std::size_t reject_n = 0);

api discover(const std::vector<view>& sections);

// ── Точка входа кадра ────────────────────────────────────────────────────────
// Движок раз в кадр зовёт скриптовый `DayZGame.OnUpdate(bool doSim, float timeslice)`.
// Делает он это не напрямую: сначала ищет ИНДЕКС метода по имени и кэширует его в
// глобали, потом собирает кадр вызова по этому индексу и исполняет.
//
//     lea  rdx, "OnUpdate"          <- якорь, как и у всего остального в этом файле
//     call найти_индекс_по_имени
//     mov  cs:кэш, eax              <- индекс лежит здесь и дальше только читается
//     cvtss2sd xmm0, xmm?           <- timeslice уезжает во фрейм: ЭТОТ OnUpdate с float
//     mov  r8d, eax
//     call собрать_вызов            <- вот сюда и вешаемся
//     call исполнить
//
// Зачем так, а не хук на саму функцию кадра: её НАЧАЛО в рантайме взять неоткуда —
// границ функций у нас нет, только байты. А цель `call` берётся из смещения, и это
// ровно то, чем уже найдены RegisterGlobal и FindClass.
//
// Строк "OnUpdate" в образе одна на всех (это имя события), и ссылок на неё несколько:
// виджеты, техника, игра. Нужную отличает `cvtss2sd` между поиском индекса и сборкой
// вызова — float-аргумент есть только у кадрового OnUpdate.
struct frame_entry {
    // Собрать кадр вызова: (объект, буфер кадра, индекс метода, doSim, timeslice).
    //
    // ВНИМАНИЕ: эта сигнатура — сигнатура ОДНОГО МЕСТА ВЫЗОВА, а не функции. Сама функция
    // общая, её зовут из сотен мест, и у каждого свои аргументы: где-то float в xmm2,
    // где-то нет. Хук на ФУНКЦИЮ поэтому смертелен — детур на C++ затирает xmm, и чужой
    // вызов с вещественным аргументом уезжает в движок испорченным. Это стоило трёх
    // падений сервера, прежде чем стало понятно.
    //
    // Поэтому перенаправляется не функция, а `call` в CGame::Update: тогда к нам приходит
    // ровно то, что кладёт это место, и только оно.
    using prepare_fn            = void(__fastcall*)(void* self, void* frame, std::uint32_t index, std::uint32_t sim, double dt);
    std::uintptr_t      site    = 0;       // адрес самой инструкции `call` в CGame::Update
    prepare_fn          prepare = nullptr; // куда она ведёт сейчас
    const std::int32_t* index   = nullptr; // движковый кэш индекса OnUpdate

    explicit operator bool() const { return site && prepare && index; }
};

frame_entry find_frame_entry(const std::vector<view>& sections);

// Цель первого `call rel32` внутри функции — так добираемся до внутренностей
// движка, у которых нет своих строк-маяков (линковщик, поиск функции по имени).
std::uintptr_t first_call(const std::vector<view>& sections, std::uintptr_t fn, std::size_t span = 0x60);

// Все места, где инструкция вида insn ссылается через rip на C-строку ровно text.
std::vector<std::uintptr_t> rip_refs(const std::vector<view>& sections, const char* text, pattern_view insn);

// Начало функции, в которой лежит ea, — по таблицам раскрутки загруженного образа (.pdata),
// через сцепленные записи: у куска, вынесенного компилятором, запись своя. Ноль — у адреса
// записи нет (не код или лист без кадра).
std::uintptr_t function_start(std::uintptr_t ea);

// Начало функции, которая берёт адрес C-строки ровно text (`lea reg,[rip+d]`). Для
// движковых функций, у которых есть строка, но нет натива. Ноль — не нашлось или
// ссылаются РАЗНЫЕ функции: угадывать из двух нельзя.
std::uintptr_t function_referencing(const std::vector<view>& sections, const char* text);

// Секции загруженного образа. Одна на всех: и хосту для поиска регистрации, и
// плагину для поиска внутренностей движка.
std::vector<view> sections_of(void* module);

// Цели всех rel32-переходов с этим опкодом в [ea, ea+span). Мусорные отброшены: 0xE8/0xE9
// встречается и внутри чужого смещения, а настоящая цель обязана лежать в исполняемой
// секции образа. Так разбирается тело найденной функции — куда она зовёт и чем кончается.
std::vector<std::uintptr_t> rel32_targets(const std::vector<view>& sections, std::uintptr_t ea, std::size_t span, std::uint8_t opcode = 0xE8);

// ── RTTI: от имени класса к его таблице ──────────────────────────────────────
// Чистая механика MSVC, про DayZ она не знает ничего. Нужна там, где у метода нет своей
// строки-маяка: невиртуальный метод берётся по коду класса, а к коду ведёт таблица.
//
// Раскладка (x64, RVA считаются от базы образа):
//   _TypeDescriptor  { void* vfptr; void* spare; char name[]; }   имя лежит на +16
//   CompleteObjectLocator { u32 signature; u32 offset; u32 cd_offset;
//                           u32 type_rva; u32 class_rva; u32 self_rva }
//   перед самой таблицей лежит указатель на локатор, поэтому таблица — это `ptr + 8`
//
// mangled — имя как его пишет компилятор. Буква после `.?A` — это ВИД типа, и перепутать
// её легко: `V` у class, `U` у struct, `W` у enum. ".?AVAIBehaviourGoToTarget@@".
//
// Ноль — не нашли; это отказ, а не повод идти дальше по догадке.
std::uintptr_t rtti_vtable(const std::vector<view>& sections, std::uintptr_t image_base, const char* mangled);
// То же от модуля: базу и секции берёт сам. Столько же работы, на одну ошибку меньше —
// перепутать базу с базой первой секции нечем.
std::uintptr_t rtti_vtable(void* module, const char* mangled);

// Отображены ли n байт по адресу. Спрашивается у системы, а не пробным чтением под SEH:
// движок ставит свой фильтр исключений и рапортует о падении раньше, чем сработал бы
// наш __except (проверено — вылет 0xC0000005 с адресом внутри hid.dll).
//
// ВЫРАВНИВАНИЯ НЕ ТРЕБУЕТ, и это не мелочь: рядом живёт script::detail::readable, который
// сперва спрашивает «похоже ли это вообще на указатель» (в том числе кратность восьми).
// Там это правильно — он проверяет ЗНАЧЕНИЕ ПОЛЯ; здесь неправильно — адрес внутри
// функции ничем не выровнен. Отсюда две функции при одной проверке страницы.
bool readable(const void* p, std::size_t n);

// Исполняемая ли страница по адресу. Спрашивается у системы, а не у таблицы секций:
// так узнаются и трамплины, и чужие модули, а не только образ игры.
bool is_code(const void* p);

// Длина C++ таблицы: подряд идущие адреса машинного кода. Длину её никто не хранит,
// поэтому считаем сами; потолок — не от жадности, а чтобы не уехать в соседние данные,
// если .rdata продолжается такими же указателями.
std::size_t vtable_slots(void* const* vt);

// ── Позвать НАСТОЯЩИЙ метод C++ движка ───────────────────────────────────────
// Это НЕ натив: натив зарегистрирован через RegisterMethod, за ним стоит обёртка движка,
// и зовут его через ref::call. Здесь — метод, найденный по RTTI и таблице (rtti_vtable),
// то есть обычная функция-член, как её собрал MSVC.
//
// Разница ровно одна, и она смертельна. Возврат больше 8 байт (скриптовый vector — это
// 12) в RAX не влезает: под него заводится буфер, адрес которого вызывающая сторона
// передаёт скрытым аргументом. У ФУНКЦИИ-ЧЛЕНА этот адрес идёт ВТОРЫМ, после this; у
// свободной — первым. Снято с clang-cl:
//
//   vec3 Ent::GetOrigin()      ->  mov rcx, this  ; mov rdx, буфер ; call
//   vec3 __fastcall f(void*)   ->  mov rcx, буфер ; mov rdx, объект; call
//
// Поэтому `reinterpret_cast<vector(__fastcall*)(void*)>(метод)(self)` кладёт буфер туда,
// где метод ждёт this, а объект — туда, куда он пишет результат: чтение по мусору и
// запись поверх чужого объекта. Это и есть 0xC0000005 на ровном месте.
//
// Обратное тоже верно и тоже проверено на живом движке: у ЗАРЕГИСТРИРОВАННОГО натива
// буфер идёт первым — см. graft::position в world.hpp, там разобран импл IEntity.GetOrigin.
template <class R>
concept returned_in_register =
    std::is_void_v<R> || ((sizeof(R) == 1 || sizeof(R) == 2 || sizeof(R) == 4 ||
                           sizeof(R) == 8) &&
                          std::is_trivially_copyable_v<R>);

template <class R, class... A>
R member_call(void* fn, void* self, A... args) {
    if constexpr (returned_in_register<R>) {
        return reinterpret_cast<R(__fastcall*)(void*, A...)>(fn)(self, args...);
    } else {
        R out{};
        reinterpret_cast<void(__fastcall*)(void*, R*, A...)>(fn)(self, &out, args...);
        return out;
    }
}

} // namespace graft::scan
