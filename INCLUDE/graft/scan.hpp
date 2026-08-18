#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

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
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::uintptr_t base = 0;  // адрес data[0]
    bool exec = false;        // исполняемая: только в таких ищем инструкции

    // Адрес C-строки ровно text (не подстроки), начиная с ea. 0 — нет.
    std::uintptr_t find_cstr(const char* text, std::uintptr_t from = 0) const;
    // Адрес инструкции `lea reg,[rip+d]` (opcode — 3 байта префикса), указывающей на target.
    std::uintptr_t find_lea(const std::uint8_t (&opcode)[3], std::uintptr_t target,
                            std::uintptr_t from = 0) const;
    // Цели всех `call rel32` после/до ea в пределах span байт, по порядку адресов.
    // Их несколько, потому что байт 0xE8 может встретиться и внутри чужого смещения —
    // отсеивает уже вызывающий код (цель обязана лежать в исполняемой секции).
    std::vector<std::uintptr_t> calls_after(std::uintptr_t ea, std::size_t span = 0x40) const;
    std::vector<std::uintptr_t> calls_before(std::uintptr_t ea, std::size_t span = 0x40) const;
    bool contains(std::uintptr_t ea) const { return ea >= base && ea - base < size; }
};

inline constexpr std::uint8_t lea_rdx[3] = {0x48, 0x8D, 0x15};  // 2-й аргумент fastcall
inline constexpr std::uint8_t lea_r8[3] = {0x4C, 0x8D, 0x05};   // 3-й аргумент fastcall

// Точки движка. Сигнатуры выведены из декомпиляции (re/out/server/reg_*.c):
// последний числовой аргумент — размер буфера возврата, для `proto native` он 0.
using reg_global_fn = void*(__fastcall*)(void* ctx, const char* name, void* impl,
                                         unsigned ret_buf);
using reg_method_fn = void*(__fastcall*)(void* ctx, void* cls, const char* name, void* impl,
                                         unsigned ret_buf, char create);
using find_class_fn = void*(__fastcall*)(void* ctx, const char* name);

struct api {
    reg_global_fn register_global = nullptr;
    reg_method_fn register_method = nullptr;
    find_class_fn find_class = nullptr;
    explicit operator bool() const { return register_global && register_method && find_class; }
};

// Голосование нескольких якорей: случайный байт 0xE8 в чужом смещении может дать
// ложный call, но совпасть у трёх разных якорей он не может.
std::uintptr_t vote(const std::vector<view>& sections, const std::uint8_t (&opcode)[3],
                    const char* const* anchors, bool before = false,
                    const std::uintptr_t* reject = nullptr, std::size_t reject_n = 0);

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
    using prepare_fn = void(__fastcall*)(void* self, void* frame, std::uint32_t index,
                                         std::uint32_t sim, double dt);
    std::uintptr_t site = 0;              // адрес самой инструкции `call` в CGame::Update
    prepare_fn prepare = nullptr;         // куда она ведёт сейчас
    const std::int32_t* index = nullptr;  // движковый кэш индекса OnUpdate
    explicit operator bool() const { return site && prepare && index; }
};

frame_entry find_frame_entry(const std::vector<view>& sections);

// Цель первого `call rel32` внутри функции — так добираемся до внутренностей
// движка, у которых нет своих строк-маяков (линковщик, поиск функции по имени).
std::uintptr_t first_call(const std::vector<view>& sections, std::uintptr_t fn,
                          std::size_t span = 0x60);

}  // namespace graft::scan
