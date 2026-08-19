// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include <algorithm>
#include <deque>
#include <format>
#include <unordered_map>
#include <vector>

#include "graft/engine.hpp"
#include "graft/script.hpp"

// Смерть скриптового объекта — без единой строчки в скрипте.
//
// Как это делает сам движок (разобрано по дизассемблеру, RESEARCH/README.md): у
// дескриптора класса есть своя C++ таблица, её слот 8 ведёт освобождение объекта, а то
// зовёт ВИРТУАЛЬНЫЙ НУЛЕВОЙ СЛОТ самого объекта — обычный `scalar deleting destructor`
// MSVC (`call [rax]`, rdx = 0). Именно им чистят себя array/map/set: скриптового
// деструктора у них нет и не нужно, память освобождает C++ движка.
//
// Значит и нам объявлять нечего: объекту, за которым стоит состояние плагина, мы
// подменяем таблицу на свою копию — нулевой слот наш, остальные как были. Движок зовёт
// её ровно там же, где звал бы свою.
namespace graft::script {
namespace {

// Сигнатура нулевого слота: MSVC-шный `void* __fastcall dtor(this, flags)`, где бит 1
// во flags значит «и память освободи». Мы не решаем — мы только уведомляем и передаём
// вызов дальше как есть.
using dtor_fn = void*(__fastcall*)(void*, unsigned);

// Копия одной движковой таблицы. Первым элементом памяти лежит указатель на саму
// запись: объекту отдаётся адрес СЛЕДУЮЩЕГО элемента, поэтому по таблице из объекта
// исходный слот находится вычитанием, без поиска в контейнере.
struct patched {
    dtor_fn original = nullptr;
    std::vector<void*> memory;

    void** table() { return memory.data() + 1; }
};

// Таблицы не разрушаются вместе со статикой: движок может разрушать объекты и после
// того, как побежали деструкторы статиков при выгрузке. Работать с трупом хуже, чем не
// освободить пару килобайт на выходе.
std::deque<patched>& tables() {
    static auto* all = new std::deque<patched>;
    return *all;
}
std::unordered_map<void**, patched*>& by_original() {
    static auto* all = new std::unordered_map<void**, patched*>;
    return *all;
}
// ponytail: вектор, а не set — забывальщиков столько же, сколько классов с состоянием.
std::vector<void (*)(void*)>& forgetters() {
    static auto* all = new std::vector<void (*)(void*)>;
    return *all;
}

bool is_code(const void* p) {
    if (!p) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof mbi) == 0 || mbi.State != MEM_COMMIT) {
        return false;
    }
    constexpr DWORD kExec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & kExec) != 0 && (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
}

// Длину таблицы никто не хранит — считаем её сами: подряд идущие адреса машинного кода.
// Потолок нужен не от жадности, а чтобы не уехать в соседние данные, если .rdata
// продолжается такими же указателями.
constexpr std::size_t kMaxSlots = 256;

std::size_t slot_count(void* const* vt) {
    std::size_t n = 0;
    // Спрашиваем систему и про сам слот: у последней таблицы в секции следующая страница
    // может быть не отображена, и чтение за край — падение на ровном месте.
    while (n < kMaxSlots && detail::readable(&vt[n], sizeof(void*)) && is_code(vt[n])) {
        ++n;
    }
    return n;
}

void* __fastcall on_destroy(void* self, unsigned flags) {
    auto** vt = *reinterpret_cast<void***>(self);
    auto* record = *reinterpret_cast<patched**>(vt - 1);
    for (void (*forget)(void*) : forgetters()) {
        forget(self);
    }
    return record->original ? record->original(self, flags) : nullptr;
}

patched* copy_of(void** vt) {
    const auto it = by_original().find(vt);
    if (it != by_original().end()) {
        return it->second;
    }
    const std::size_t count = slot_count(vt);
    if (count == 0) {
        return nullptr;
    }
    patched& made = tables().emplace_back();
    made.original = reinterpret_cast<dtor_fn>(vt[0]);
    made.memory.resize(count + 1);
    made.memory[0] = &made;
    std::copy(vt, vt + count, made.memory.begin() + 1);
    made.table()[0] = reinterpret_cast<void*>(&on_destroy);
    by_original().emplace(vt, &made);
    by_original().emplace(made.table(), &made);  // повторную подмену узнаём по копии
    log(std::format("объекты: таблица {} скопирована ({} слотов)", static_cast<void*>(vt), count));
    return &made;
}

}  // namespace

// ponytail: без синхронизации — объекты рождаются и умирают на скриптовом потоке, там же
// зовутся нативы.
void watch_object(void* self, void (*forget)(void*)) {
    if (!self || !forget) {
        return;
    }
    auto& all = forgetters();
    if (std::find(all.begin(), all.end(), forget) == all.end()) {
        all.push_back(forget);
    }
    // Указатель на таблицу приходит из чужой памяти — прежде чем читать по нему, надо
    // убедиться, что это вообще указатель. Это же отсекает вызовы трамплинов с
    // ненастоящим объектом (так их зовут оффлайновые тесты).
    if (!detail::plausible(self) || !detail::readable(self, sizeof(void*))) {
        return;
    }
    auto** vt = *reinterpret_cast<void***>(self);
    if (!detail::plausible(vt)) {
        return;
    }
    const auto known = by_original().find(vt);
    if (known != by_original().end() && known->second->table() == vt) {
        return;  // этому объекту таблицу уже подменили
    }
    if (patched* made = copy_of(vt)) {
        *reinterpret_cast<void***>(self) = made->table();
    }
}

}  // namespace graft::script
