// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
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

// Секция отображённого образа: байты + адрес, по которому они лежат в памяти.
struct view {
    std::span<const std::uint8_t> bytes;        // сама секция: указатель и длина вместе
    std::uintptr_t                base = 0;     // адрес bytes[0]
    bool                          exec = false; // исполняемая: в таких ищем инструкции

    // Адрес C-строки ровно text (не подстроки), начиная с ea. 0 — нет.
    std::uintptr_t find_cstr(const char* text, std::uintptr_t from = 0) const;
    // Адрес инструкции `lea reg,[rip+d]` (opcode — 3 байта префикса), указывающей на target.
    std::uintptr_t find_lea(std::span<const std::uint8_t, 3> opcode, std::uintptr_t target, std::uintptr_t from = 0) const;
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

inline constexpr std::uint8_t lea_rdx[3] = {0x48, 0x8D, 0x15}; // 2-й аргумент fastcall
inline constexpr std::uint8_t lea_r8[3]  = {0x4C, 0x8D, 0x05}; // 3-й аргумент fastcall

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
std::uintptr_t vote(const std::vector<view>& sections, std::span<const std::uint8_t, 3> opcode, const char* const* anchors, bool before = false, const std::uintptr_t* reject = nullptr, std::size_t reject_n = 0);

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

// Секции загруженного образа. Одна на всех: и хосту для поиска регистрации, и
// плагину для поиска внутренностей движка.
std::vector<view> sections_of(void* module);

// ── Сверка найденного ────────────────────────────────────────────────────────
// Ход по call-графу даёт КАНДИДАТА; кандидат становится ответом только когда его
// первые байты совпали с сигнатурой. Так «нашлось не то» превращается в отказ, а не
// в прыжок в середину чужого кода.
bool begins_with(std::uintptr_t ea, std::span<const std::uint8_t> sig);

// Смещение из инструкции с таким опкодом в [ea, ea+span): `nth`-й по счёту.
// Возвращает false, если такой инструкции там нет. Этим достаются и поля структур
// (`mov rax,[rcx+disp]`), и адреса глобалей (`mov [rip+disp],rax`).
bool disp32_of(std::uintptr_t ea, std::size_t span, std::span<const std::uint8_t> opcode, std::int32_t& out, std::uintptr_t* site = nullptr, unsigned nth = 1);
bool disp8_of(std::uintptr_t ea, std::size_t span, std::span<const std::uint8_t> opcode, unsigned& out, unsigned nth = 1);

// Сигнатура с ДЖОКЕРАМИ: элемент < 0 — «любой байт». Сверять можно не всё: там, где в
// инструкцию запечено смещение поля или регистр, выбранный компилятором игры, сверять
// нечего — а всё остальное сверить обязаны. Пишется ровно так, как читается в отладчике:
//
//   scan::matches(fn, {0x48, 0x89, 0x5C, 0x24, -1, 0x57})
//
// begins_with — тот же приём без джокеров; он остаётся отдельным, потому что сверка
// готового блока байт не должна проходить через int-массив.
bool matches(std::uintptr_t ea, std::span<const int> sig);

inline bool matches(std::uintptr_t ea, std::initializer_list<int> sig) {
    return matches(ea, std::span<const int>{sig.begin(), sig.size()});
}

// Та же сигнатура, но так, как она читается в отладчике — строкой:
//
//   scan::matches(fn, scan::sig<"48 89 5C 24 ?? 57">)
//
// Разбирается на КОМПИЛЯЦИИ, поэтому опечатка («4» вместо «48», лишняя буква, `??` из
// трёх знаков) — ошибка сборки, а не сигнатура, которая молча ничего не находит и
// отлаживается уже на живом сервере. Лишние пробелы законны: строку копируют из
// отладчика, а он выравнивает столбцы.
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

consteval int sig_byte(std::string_view token) {
    if (token == "??" || token == "?") {
        return -1;  // джокер: там смещение поля или регистр, выбранный компилятором игры
    }
    if (token.size() != 2) {
        throw "байт сигнатуры — ровно две шестнадцатеричные цифры либо ??";
    }
    return (sig_nibble(token[0]) * 16) + sig_nibble(token[1]);
}

consteval std::size_t sig_size(std::string_view text) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == ' ') {
            ++i;
            continue;
        }
        ++n;
        while (i < text.size() && text[i] != ' ') {
            ++i;
        }
    }
    return n;
}

template <name_t S>
consteval auto parse_sig() {
    constexpr std::string_view text{S.value};
    constexpr std::size_t      n = sig_size(text);
    static_assert(n > 0, "пустая сигнатура");
    std::array<int, n> out{};
    std::size_t        k = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == ' ') {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < text.size() && text[j] != ' ') {
            ++j;
        }
        out[k++] = sig_byte(text.substr(i, j - i));
        i        = j;
    }
    return out;
}

}  // namespace detail

template <name_t S>
inline constexpr auto sig = detail::parse_sig<S>();

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
