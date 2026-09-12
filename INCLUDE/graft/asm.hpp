// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once

// СИГНАТУРА СТРОЧКАМИ АССЕМБЛЕРА — теми самыми, что показывает отладчик.
//
//   constexpr auto prologue = scan::code<R"(
//       mov  [rsp+8],rbx
//       push rdi
//       sub  rsp,20h
//       mov  rbx,rcx
//   )">;
//
// То же, что scan::sig<"48 89 5C 24 08 57 48 83 EC 20 48 8B D9">, и тип у них один —
// `pattern`. Разница в том, ЧТО ДЕЛАЕТ ЧЕЛОВЕК, когда игру пропатчили: он
// открывает функцию в IDA и сравнивает её листинг с этим исходником строка в строку.
// Шестнадцатеричная простыня такого сравнения не выдерживает — в ней не видно даже, где
// кончается одна инструкция и начинается другая.
//
// Кодирование идёт на КОМПИЛЯЦИИ, байт в байт. Сигнатура с опечаткой — ошибка сборки.
//
// ── ДЫРКА ────────────────────────────────────────────────────────────────────
// Там, где сверять нечего, а ответ нужен, в операнде пишется `disp8`, `disp32`, `imm8` или
// `imm32` вместо числа — это ДЫРКА, её содержимое и отдаёт `scan::find`:
//
//   mov rax,[rcx+disp32]     смещение поля: любое, и оно же ответ
//   cmp eax,imm32            предел, зашитый в сравнение
//
// Дырка в сигнатуре одна: искомое смещение в инструкции одно.
//
// ── reg64: ЛЮБОЙ РЕГИСТР ─────────────────────────────────────────────────────
// Регистр-приёмник выбирает компилятор игры, а форма адресации сверяться обязана. На месте
// регистра тогда пишется `reg64`:
//
//   lea reg64,[rip+disp32]      48&FB 8D 05&C7 + дырка
//
// Сверяются mod и rm в modrm, а поле reg отпускается маской; в префиксе REX так же
// отпускается бит R, поэтому находятся и rax..rdi, и r8..r15. Целиком отпустить байт
// (`??`) здесь нельзя: под него подойдёт и другая форма адресации, а именно её мы и
// проверяем.
//
// Только 64-битный и только на месте приёмника или источника, не в адресе. Остальное —
// ошибка сборки, и не из лени: у `reg32` без REX.W префикс появляется у r8d..r15d и не
// появляется у eax..edi, то есть ДЛИНА инструкции разная, и одной сигнатурой оба случая
// не накрыть.
//
// ── db: СЫРЫЕ БАЙТЫ, И ПОЧЕМУ БЕЗ НИХ НЕЛЬЗЯ ─────────────────────────────────
// Строчка ассемблера НЕ ОПРЕДЕЛЯЕТ байты однозначно, и на коде движка это не теория:
//
//   * `push rbp` бывает `55`, а бывает `40 55` — с лишним префиксом REX, который ничего не
//     меняет и который MSVC в кадровых функциях пишет. Обе строчки читаются как `push rbp`;
//   * сигнатура может намеренно ОБРЫВАТЬСЯ посреди инструкции: в `mov rbx,[rcx+2B8h]`
//     сверяются первые четыре байта, а смещение 2B8h — как раз то, что патч игры и
//     сдвигает, поэтому в сигнатуру оно не берётся.
//
// Ни то, ни другое мнемоникой не выражается — и врать об этом нельзя. Поэтому есть `db`,
// директива самого ассемблера:
//
//   db 40          ; лишний REX: так MSVC пишет push rbp в кадровой функции
//   push rbp
//   db 48 8B 99 B8 ; обрывок mov rbx,[rcx+2B8h]: дальше смещение версии игры
//
// Строчка с `db` в листинге сразу видна как «здесь сверяются байты, а не инструкция».
//
// ── ЧТО КОДИРОВЩИК УМЕЕТ ─────────────────────────────────────────────────────
// Ровно то, что встречается в разобранных функциях движка, и ни грамма больше:
//
//   mov, lea, push, pop, xadd (с префиксом lock), imul r,r,imm
//   add or adc sbb and sub xor cmp, test, shl shr sar rol ror
//   jmp и jcc с коротким смещением, db
//
// Незнакомая мнемоника или форма — ОШИБКА СБОРКИ с текстом, а не молча неверный байт.
// Надо больше — допиши сюда; по одной инструкции это несколько строк таблицы.
//
// Умышленно НЕ УМЕЕТ: 16-битные операнды (префикс 66), адресацию с индексом
// (`[rax+rcx*4]`), дальние переходы, вещественное и векторное. В разобранных функциях
// такого нет, а догадка в кодировщике — это неверная сигнатура, то есть ровно та беда, от
// которой весь этот файл.
//
// ── ПРОВЕРКА ─────────────────────────────────────────────────────────────────
// `tests/asm_test.cpp` сверяет кодировщик с байтами, снятыми с дизассемблера и из SDM, — на
// компиляции. Ошибка в кодировщике краснеет сборкой, а не выездом на стенд.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "graft/name.hpp"
#include "graft/scan.hpp"

namespace graft::scan::detail {

// ── Таблицы ──────────────────────────────────────────────────────────────────

struct reg_def {
    std::string_view text;
    std::uint8_t     num;
    std::uint8_t     bits;
};

// ah/ch/dh/bh здесь нет намеренно: их коды 4..7 без REX, а С префиксом те же коды значат
// spl/bpl/sil/dil. Движковый код пользуется вторыми, и путать их нечем, когда первых нет.
constexpr reg_def k_regs[] = {
    {"rax", 0, 64},  {"rcx", 1, 64},  {"rdx", 2, 64},   {"rbx", 3, 64},
    {"rsp", 4, 64},  {"rbp", 5, 64},  {"rsi", 6, 64},   {"rdi", 7, 64},
    {"r8", 8, 64},   {"r9", 9, 64},   {"r10", 10, 64},  {"r11", 11, 64},
    {"r12", 12, 64}, {"r13", 13, 64}, {"r14", 14, 64},  {"r15", 15, 64},
    {"eax", 0, 32},  {"ecx", 1, 32},  {"edx", 2, 32},   {"ebx", 3, 32},
    {"esp", 4, 32},  {"ebp", 5, 32},  {"esi", 6, 32},   {"edi", 7, 32},
    {"r8d", 8, 32},  {"r9d", 9, 32},  {"r10d", 10, 32}, {"r11d", 11, 32},
    {"r12d", 12, 32},{"r13d", 13, 32},{"r14d", 14, 32}, {"r15d", 15, 32},
    {"al", 0, 8},    {"cl", 1, 8},    {"dl", 2, 8},     {"bl", 3, 8},
    {"spl", 4, 8},   {"bpl", 5, 8},   {"sil", 6, 8},    {"dil", 7, 8},
    {"r8b", 8, 8},   {"r9b", 9, 8},   {"r10b", 10, 8},  {"r11b", 11, 8},
    {"r12b", 12, 8}, {"r13b", 13, 8}, {"r14b", 14, 8},  {"r15b", 15, 8},
};

// Группа 1: у всех восьми общая форма, различает их только поле reg в modrm.
struct byte_def {
    std::string_view text;
    std::uint8_t     value;  // цифра в поле reg у групповых команд, опкод у переходов
};

constexpr byte_def k_alu[] = {
    {"add", 0}, {"or", 1}, {"adc", 2}, {"sbb", 3},
    {"and", 4}, {"sub", 5}, {"xor", 6}, {"cmp", 7},
};

constexpr byte_def k_shift[] = {{"rol", 0}, {"ror", 1}, {"shl", 4}, {"shr", 5}, {"sar", 7}};

constexpr byte_def k_jcc[] = {
    {"jo", 0x70},  {"jno", 0x71}, {"jb", 0x72},  {"jc", 0x72},  {"jae", 0x73}, {"jnb", 0x73},
    {"je", 0x74},  {"jz", 0x74},  {"jne", 0x75}, {"jnz", 0x75}, {"jbe", 0x76}, {"ja", 0x77},
    {"js", 0x78},  {"jns", 0x79}, {"jl", 0x7C},  {"jge", 0x7D}, {"jle", 0x7E}, {"jg", 0x7F},
    {"jmp", 0xEB},
};

consteval const byte_def* digit_of(const byte_def* table, std::size_t n, std::string_view t) {
    for (std::size_t i = 0; i < n; ++i) {
        if (table[i].text == t) {
            return &table[i];
        }
    }
    return nullptr;
}

// ── Разбор текста ────────────────────────────────────────────────────────────

consteval bool asm_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

consteval std::string_view asm_trim(std::string_view s) {
    while (!s.empty() && asm_space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && asm_space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

// Число: 0x2C8, 2B8h, 40, -8, +5. Регистр цифр не важен — строку копируют из отладчика.
consteval long long asm_number(std::string_view t) {
    if (t.empty()) {
        throw "ожидалось число";
    }
    bool negative = false;
    if (t.front() == '-' || t.front() == '+') {
        negative = t.front() == '-';
        t.remove_prefix(1);
    }
    unsigned base = 10;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
        base = 16;
        t.remove_prefix(2);
    } else if (!t.empty() && (t.back() == 'h' || t.back() == 'H')) {
        base = 16;
        t.remove_suffix(1);
    }
    if (t.empty()) {
        throw "число без цифр";
    }
    long long value = 0;
    for (const char c : t) {
        unsigned digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(c - 'a') + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned>(c - 'A') + 10;
        } else {
            throw "в числе посторонний знак";
        }
        if (digit >= base) {
            throw "цифра не из этой системы счисления (шестнадцатеричное пиши как 0x2C8 или 2B8h)";
        }
        value = static_cast<long long>(value * base) + digit;
    }
    return negative ? -value : value;
}

// Имя дырки: disp8, disp32, imm8, imm32 — и только они. Ноль — это не дырка.
consteval unsigned asm_hole_bits(std::string_view t) {
    if (t == "disp8" || t == "imm8") {
        return 8;
    }
    if (t == "disp32" || t == "imm32") {
        return 32;
    }
    return 0;
}

struct width_def {
    std::string_view text;
    std::uint8_t     bits;
};

enum class op_kind : std::uint8_t { none, reg, mem, imm };

struct operand {
    op_kind      kind      = op_kind::none;
    std::uint8_t bits      = 0;  // ширина операнда; 0 — не сказана
    std::uint8_t num       = 0;  // номер регистра, он же база для памяти
    bool         rip       = false;
    std::uint8_t disp_bits = 0;  // 0 — смещения нет, иначе 8 или 32
    long long    disp      = 0;
    bool         disp_hole = false;
    long long    imm       = 0;
    std::uint8_t imm_bits  = 0;  // ширина, если её задала дырка; иначе 0
    bool         imm_hole  = false;
    bool         any       = false;  // `reg64`: регистр любой, сверяются остальные биты
};

consteval operand asm_operand(std::string_view t) {
    operand out;
    t = asm_trim(t);
    // Необязательная ширина: строку копируют из отладчика, а тот её печатает.
    constexpr width_def widths[] = {{"qword", 64}, {"dword", 32}, {"word", 16}, {"byte", 8}};
    for (const width_def& w : widths) {
        if (!t.starts_with(w.text)) {
            continue;
        }
        out.bits = w.bits;
        t        = asm_trim(t.substr(w.text.size()));
        if (t.starts_with("ptr")) {
            t = asm_trim(t.substr(3));
        }
        break;
    }
    if (t.empty()) {
        throw "пустой операнд";
    }

    if (t.front() == '[') {
        if (t.back() != ']') {
            throw "операнд-память без закрывающей скобки";
        }
        out.kind              = op_kind::mem;
        std::string_view body = asm_trim(t.substr(1, t.size() - 2));
        // База и, через плюс, смещение. Индекса ([rax+rcx*4]) кодировщик не знает.
        if (body.find('*') != std::string_view::npos) {
            throw "адресацию с индексом кодировщик не знает — поставь db с байтами";
        }
        const std::size_t      sign = body.find_first_of("+-");
        const std::string_view base = asm_trim(sign == std::string_view::npos
                                                   ? body
                                                   : body.substr(0, sign));
        if (base == "rip") {
            out.rip       = true;
            out.disp_bits = 32;
        } else {
            bool known = false;
            for (const reg_def& r : k_regs) {
                if (r.text == base && r.bits == 64) {
                    out.num = r.num;
                    known   = true;
                    break;
                }
            }
            if (!known) {
                throw "базой адреса бывает только 64-битный регистр либо rip";
            }
        }
        if (sign != std::string_view::npos) {
            // Знак входит в текст: минус — обычная адресация локали от rbp, и число его
            // разберёт само. Дырка бывает только со плюсом: отрицательной ширины нет.
            const std::string_view disp = asm_trim(body.substr(sign));
            const unsigned         bits =
                disp.front() == '+' ? asm_hole_bits(asm_trim(disp.substr(1))) : 0;
            if (bits != 0) {
                out.disp_hole = true;
                out.disp_bits = static_cast<std::uint8_t>(bits);
            } else {
                out.disp = asm_number(disp);
                // Короче — значит так же, как пишет компилятор: он тоже берёт disp8, когда
                // смещение в него влезает.
                out.disp_bits = (out.disp >= -128 && out.disp <= 127) ? 8 : 32;
            }
        } else if (out.rip) {
            throw "у [rip+...] смещение обязательно";
        }
        return out;
    }

    // `reg64` — ЛЮБОЙ регистр на этом месте. Нужно там, где регистр-приёмник выбрал
    // компилятор игры, а форма адресации сверяться обязана: `lea reg64,[rip+disp32]`.
    constexpr width_def any_regs[] = {{"reg64", 64}, {"reg32", 32}, {"reg8", 8}};
    for (const width_def& w : any_regs) {
        if (t == w.text) {
            out.kind = op_kind::reg;
            out.bits = w.bits;
            out.any  = true;
            return out;
        }
    }
    if (t == "reg") {
        throw "у любого регистра нужна ширина: reg64";
    }

    for (const reg_def& r : k_regs) {
        if (r.text == t) {
            out.kind = op_kind::reg;
            out.num  = r.num;
            out.bits = r.bits;
            return out;
        }
    }

    out.kind = op_kind::imm;
    if (const unsigned bits = asm_hole_bits(t); bits != 0) {
        out.imm_hole = true;
        out.imm_bits = static_cast<std::uint8_t>(bits);
    } else {
        out.imm = asm_number(t);
    }
    return out;
}

// ── Выход ────────────────────────────────────────────────────────────────────

// Потолок на сигнатуру. 128 байт с запасом — живые сигнатуры в разы короче, — и он всё
// равно упирается в `pattern::hole`, который хранит смещение одним байтом.
constexpr std::size_t k_asm_max = 128;

struct asm_buf {
    std::array<std::uint8_t, k_asm_max> value{};
    std::array<std::uint8_t, k_asm_max> mask{};
    std::size_t                         n         = 0;
    std::uint8_t                        hole      = 0;
    std::uint8_t                        hole_size = 0;

    // mask — какие биты байта сверяются: 0xFF весь, 0 ни один, что-то между — часть байта
    // (поле reg в modrm, бит R в префиксе REX).
    consteval void put(unsigned v, unsigned m = 0xFF) {
        if (n >= k_asm_max) {
            throw "сигнатура длиннее 128 байт";
        }
        if ((v & ~m) != 0) {
            throw "в байте подняты биты вне маски: сверять их всё равно нечем";
        }
        value[n] = static_cast<std::uint8_t>(v);
        mask[n]  = static_cast<std::uint8_t>(m);
        ++n;
    }

    consteval void put_num(long long v, unsigned bits) {
        for (unsigned i = 0; i < bits / 8; ++i) {
            put(static_cast<unsigned>((v >> (8 * i)) & 0xFF));
        }
    }

    consteval void put_hole(unsigned bits) {
        if (hole_size != 0) {
            throw "дырка в сигнатуре бывает одна: искомое смещение в инструкции одно";
        }
        hole      = static_cast<std::uint8_t>(n);
        hole_size = static_cast<std::uint8_t>(bits / 8);
        for (unsigned i = 0; i < bits / 8; ++i) {
            put(0, 0);
        }
    }
};

// REX нужен при 64-битном операнде, при регистре r8..r15 и при младшем байте rsp/rbp/rsi/rdi
// (без префикса те же коды значат ah/ch/dh/bh).
consteval void put_rex(asm_buf& out, bool wide, unsigned reg_field, unsigned rm, bool always,
                       bool reg_any = false) {
    unsigned bits = (wide ? 8u : 0u) | (reg_field >= 8 ? 4u : 0u) | (rm >= 8 ? 1u : 0u);
    unsigned mask = 0xFF;
    if (reg_any) {
        // Регистр неизвестен — значит неизвестен и бит R, его отпускаем. А вот ДЛИНА
        // инструкции зависеть от этого не должна: без REX.W у r8d..r15d префикс появляется,
        // у eax..edi его нет, и одной сигнатурой оба случая не накрыть.
        if (!wide) {
            throw "любой 32- или 8-битный регистр сигнатурой не выразить: с r8..r15 "
                  "появляется префикс REX, и длина инструкции другая";
        }
        bits &= ~4u;
        mask = 0xFB;
    }
    if (bits != 0 || always) {
        out.put(0x40 | bits, mask);
    }
}

consteval bool needs_rex8(const operand& a) {
    return a.kind == op_kind::reg && a.bits == 8 && a.num >= 4 && a.num <= 7;
}

consteval void put_rm(asm_buf& out, const operand& rm, unsigned reg_field, bool reg_any = false) {
    if (rm.any) {
        throw "`reg64` ставится на место приёмника или источника, но не в адрес";
    }
    // Поле reg занимает биты 3..5; неизвестно оно — их и отпускаем, а mod с rm сверяем.
    const unsigned m = reg_any ? 0xC7u : 0xFFu;
    if (rm.kind == op_kind::reg) {
        out.put(0xC0 | ((reg_field & 7) << 3) | (rm.num & 7), m);
        return;
    }
    if (rm.rip) {
        out.put(((reg_field & 7) << 3) | 5, m);
        if (rm.disp_hole) {
            out.put_hole(32);
        } else {
            out.put_num(rm.disp, 32);
        }
        return;
    }
    // [rbp] и [r13] без смещения не кодируются — этот код занят формой rip+disp32, поэтому
    // компилятор пишет туда disp8 = 0. Делаем то же.
    unsigned mod = rm.disp_bits == 8 ? 1 : rm.disp_bits == 32 ? 2 : 0;
    if (mod == 0 && (rm.num & 7) == 5) {
        mod = 1;
    }
    out.put((mod << 6) | ((reg_field & 7) << 3) | (rm.num & 7), m);
    if ((rm.num & 7) == 4) {
        out.put(0x24);  // rsp и r12 адресуются только через SIB
    }
    if (mod != 0) {
        const unsigned bits = mod == 1 ? 8 : 32;
        if (rm.disp_hole) {
            out.put_hole(bits);
        } else {
            out.put_num(rm.disp, bits);
        }
    }
}

consteval void put_imm(asm_buf& out, const operand& imm, unsigned bits) {
    if (imm.imm_hole) {
        out.put_hole(bits);
    } else {
        out.put_num(imm.imm, bits);
    }
}

// Общий путь: префикс lock, REX, опкод (один или два байта), modrm с хвостом.
consteval void put_form(asm_buf& out,
                        bool     lock,
                        int      op0,
                        int      op1,
                        const operand& rm,
                        unsigned reg_field,
                        unsigned bits,
                        bool     rex_always,
                        bool     reg_any = false) {
    if (bits == 16) {
        throw "16-битные операнды кодировщик не знает — поставь db с байтами";
    }
    if (lock) {
        out.put(0xF0);
    }
    put_rex(out, bits == 64, reg_field, rm.num, rex_always, reg_any);
    out.put(static_cast<unsigned>(op0));
    if (op1 >= 0) {
        out.put(static_cast<unsigned>(op1));
    }
    put_rm(out, rm, reg_field, reg_any);
}

// ── Инструкции ───────────────────────────────────────────────────────────────

consteval void asm_line(asm_buf& out, std::string_view line) {
    // Комментарий до конца строки — как в любом листинге.
    if (const std::size_t note = line.find(';'); note != std::string_view::npos) {
        line = line.substr(0, note);
    }
    line = asm_trim(line);
    if (line.empty()) {
        return;
    }

    bool lock = false;
    if (line.starts_with("lock ") || line.starts_with("lock\t")) {
        lock = true;
        line = asm_trim(line.substr(4));
    }

    std::size_t            cut = 0;
    while (cut < line.size() && !asm_space(line[cut])) {
        ++cut;
    }
    const std::string_view mnemonic = line.substr(0, cut);
    std::string_view       rest     = asm_trim(line.substr(cut));

    // db — сырые байты. Единственная форма, которая не притворяется инструкцией.
    if (mnemonic == "db") {
        if (rest.empty()) {
            throw "db без байтов";
        }
        while (!rest.empty()) {
            std::size_t end = 0;
            while (end < rest.size() && !asm_space(rest[end]) && rest[end] != ',') {
                ++end;
            }
            // Байт в db ВСЕГДА шестнадцатеричный: `db 48` — это 0x48, а не сорок восемь.
            // Так его печатает и отладчик, и листинг. Разбор тот же, что у байтовой записи
            // сигнатуры (scan::sig), поэтому и правила те же: две цифры, `??` либо `05&C7`.
            const sig_cell cell = sig_byte(rest.substr(0, end));
            out.put(cell.value, cell.mask);
            rest = asm_trim(rest.substr(end));
            if (!rest.empty() && rest.front() == ',') {
                rest = asm_trim(rest.substr(1));
            }
        }
        return;
    }

    // Операнды: до трёх, через запятую.
    operand  ops[3];
    unsigned count = 0;
    while (!rest.empty()) {
        if (count == 3) {
            throw "больше трёх операндов кодировщик не знает";
        }
        const std::size_t comma = rest.find(',');
        ops[count++] = asm_operand(comma == std::string_view::npos ? rest : rest.substr(0, comma));
        rest = comma == std::string_view::npos ? std::string_view{} : asm_trim(rest.substr(comma + 1));
    }

    const operand& a     = ops[0];
    const operand& b     = ops[1];
    const bool     rex8  = needs_rex8(a) || needs_rex8(b);

    if (mnemonic == "push" || mnemonic == "pop") {
        if (count != 1 || a.kind != op_kind::reg || a.bits != 64) {
            throw "push и pop кодируются только с 64-битным регистром";
        }
        // REX здесь только ради r8..r15: у стековых команд операнд и так 64-битный.
        if (a.num >= 8) {
            out.put(0x41);
        }
        out.put(static_cast<int>((mnemonic == "push" ? 0x50 : 0x58) + (a.num & 7)));
        return;
    }

    if (const byte_def* jump = digit_of(k_jcc, std::size(k_jcc), mnemonic); jump != nullptr) {
        if (count != 1 || a.kind != op_kind::imm) {
            throw "у перехода один операнд — короткое смещение, например `jne +5`";
        }
        out.put(jump->value);
        put_imm(out, a, 8);
        return;
    }

    if (mnemonic == "mov") {
        if (count != 2) {
            throw "у mov два операнда";
        }
        if (a.kind == op_kind::reg && (b.kind == op_kind::reg || b.kind == op_kind::mem)) {
            if (b.kind == op_kind::reg && b.bits != a.bits) {
                throw "у mov из регистра в регистр ширины обязаны совпасть";
            }
            put_form(out, lock, a.bits == 8 ? 0x8A : 0x8B, -1, b, a.num, a.bits, rex8, a.any);
            return;
        }
        if (a.kind == op_kind::mem && b.kind == op_kind::reg) {
            put_form(out, lock, b.bits == 8 ? 0x88 : 0x89, -1, a, b.num, b.bits, rex8, b.any);
            return;
        }
        if (b.kind == op_kind::imm) {
            // Ширину даёт регистр, а для памяти — только явное `dword`/`qword`.
            const unsigned bits = a.bits;  // у регистра своя, у памяти — только явная
            if (bits == 0) {
                throw "ширина записи в память не сказана: напиши `mov dword [rcx+38h],5`";
            }
            if (a.kind == op_kind::reg) {
                put_rex(out, bits == 64, 0, a.num, rex8);
                out.put(static_cast<int>((bits == 8 ? 0xB0 : 0xB8) + (a.num & 7)));
                put_imm(out, b, bits == 64 ? 64 : bits);
                return;
            }
            put_form(out, lock, bits == 8 ? 0xC6 : 0xC7, -1, a, 0, bits, rex8);
            put_imm(out, b, bits == 8 ? 8 : 32);  // у qword-записи непосредственное всё равно 32
            return;
        }
        throw "такой формы mov кодировщик не знает — поставь db с байтами";
    }

    if (mnemonic == "lea") {
        if (count != 2 || a.kind != op_kind::reg || b.kind != op_kind::mem) {
            throw "lea — это регистр и адрес";
        }
        put_form(out, lock, 0x8D, -1, b, a.num, a.bits, rex8, a.any);
        return;
    }

    if (mnemonic == "xadd") {
        if (count != 2 || b.kind != op_kind::reg) {
            throw "xadd — приёмник и регистр";
        }
        put_form(out, lock, 0x0F, b.bits == 8 ? 0xC0 : 0xC1, a, b.num, b.bits, rex8);
        return;
    }

    if (mnemonic == "imul") {
        if (count != 3 || a.kind != op_kind::reg || ops[2].kind != op_kind::imm) {
            throw "imul кодировщик знает в форме `imul rdi,rax,imm8`";
        }
        const bool byte_imm = ops[2].imm_bits == 8 ||
                              (!ops[2].imm_hole && ops[2].imm >= -128 && ops[2].imm <= 127);
        put_form(out, lock, byte_imm ? 0x6B : 0x69, -1, b, a.num, a.bits, rex8);
        put_imm(out, ops[2], byte_imm ? 8 : 32);
        return;
    }

    if (mnemonic == "test") {
        if (count != 2) {
            throw "у test два операнда";
        }
        if (b.kind == op_kind::imm && a.kind == op_kind::reg && a.num == 0) {
            // Короткая форма с аккумулятором: её и пишет компилятор.
            put_rex(out, a.bits == 64, 0, 0, rex8);
            out.put(a.bits == 8 ? 0xA8 : 0xA9);
            put_imm(out, b, a.bits == 8 ? 8 : 32);
            return;
        }
        if (b.kind == op_kind::reg) {
            put_form(out, lock, b.bits == 8 ? 0x84 : 0x85, -1, a, b.num, b.bits, rex8);
            return;
        }
        throw "такой формы test кодировщик не знает — поставь db с байтами";
    }

    if (const byte_def* shift = digit_of(k_shift, std::size(k_shift), mnemonic); shift != nullptr) {
        if (count != 2 || b.kind != op_kind::imm) {
            throw "сдвиг кодировщик знает только с непосредственным счётчиком";
        }
        const unsigned bits = a.bits != 0 ? a.bits : 32;
        put_form(out, lock, bits == 8 ? 0xC0 : 0xC1, -1, a, shift->value, bits, rex8);
        put_imm(out, b, 8);
        return;
    }

    if (const byte_def* alu = digit_of(k_alu, std::size(k_alu), mnemonic); alu != nullptr) {
        if (count != 2) {
            throw "у команды группы 1 два операнда";
        }
        if (b.kind == op_kind::imm) {
            const unsigned bits = a.bits;
            if (bits == 0) {
                throw "ширина не сказана: напиши `sub qword [rcx+8],1` либо через регистр";
            }
            const bool byte_imm = b.imm_bits == 8 ||
                                  (!b.imm_hole && b.imm_bits == 0 && b.imm >= -128 && b.imm <= 127);
            if (byte_imm && bits != 8) {
                put_form(out, lock, 0x83, -1, a, alu->value, bits, rex8);
                put_imm(out, b, 8);
                return;
            }
            // Аккумулятор с полным непосредственным — своя короткая форма, без modrm.
            if (a.kind == op_kind::reg && a.num == 0) {
                put_rex(out, bits == 64, 0, 0, rex8);
                out.put(static_cast<int>((alu->value * 8) + (bits == 8 ? 4 : 5)));
                put_imm(out, b, bits == 8 ? 8 : 32);
                return;
            }
            put_form(out, lock, bits == 8 ? 0x80 : 0x81, -1, a, alu->value, bits, rex8);
            put_imm(out, b, bits == 8 ? 8 : 32);
            return;
        }
        // Приёмник-регистр читает (опкод +2), приёмник-память пишет (+0) — так же, как у mov.
        if (a.kind == op_kind::reg) {
            if (b.kind == op_kind::reg && b.bits != a.bits) {
                throw "ширины операндов обязаны совпасть";
            }
            const unsigned op = (alu->value * 8u) + 2u + (a.bits == 8 ? 0u : 1u);
            put_form(out, lock, static_cast<int>(op), -1, b, a.num, a.bits, rex8);
            return;
        }
        if (b.kind == op_kind::reg) {
            const unsigned op = (alu->value * 8u) + (b.bits == 8 ? 0u : 1u);
            put_form(out, lock, static_cast<int>(op), -1, a, b.num, b.bits, rex8);
            return;
        }
        throw "такой формы кодировщик не знает — поставь db с байтами";
    }

    throw "такой инструкции кодировщик не знает (мнемоники и регистры — строчными, как их \n           печатает отладчик): допиши её в graft/asm.hpp либо поставь db с байтами";
}

consteval asm_buf assemble_text(std::string_view text) {
    asm_buf out;
    std::size_t i = 0;
    while (i <= text.size()) {
        std::size_t end = i;
        while (end < text.size() && text[end] != '\n') {
            ++end;
        }
        asm_line(out, text.substr(i, end - i));
        if (end >= text.size()) {
            break;
        }
        i = end + 1;
    }
    if (out.n == 0) {
        throw "пустая сигнатура";
    }
    return out;
}

template <name_t S>
consteval auto assemble() {
    constexpr asm_buf r = assemble_text(std::string_view{S.value});
    pattern<r.n>      out;
    for (std::size_t i = 0; i < r.n; ++i) {
        out.value[i] = r.value[i];
        out.mask[i]  = r.mask[i];
    }
    out.hole      = r.hole;
    out.hole_size = r.hole_size;
    return out;
}

}  // namespace graft::scan::detail

namespace graft::scan {

// Сигнатура строчками ассемблера. Тип тот же, что у `sig`, — `pattern`, поэтому обе
// годятся и для `matches`, и для `find`.
template <name_t S>
inline constexpr auto code = detail::assemble<S>();

}  // namespace graft::scan
