// safetyhook v0.7.0 — src/os.windows.cpp, ВЕНДОРНАЯ КОПИЯ С ОДНОЙ ЗАМЕНОЙ.
// Boost Software License 1.0, (C) cursey. Полный текст условий — THIRD_PARTY.md.
//
// Заменена ровно одна функция: trap_threads. Всё остальное — байт в байт как в
// апстриме, чтобы обновление сводилось к новой копии и повтору этой одной замены.
// Что именно заменено и почему — в комментарии у самой функции ниже.
//
// Файл не компилируется как часть safetyhook: в списке исходников graft_hook
// src/os.windows.cpp апстрима отсутствует, вместо него стоит этот.

#include <map>
#include <memory>
#include <mutex>

#include "safetyhook/common.hpp"
#include "safetyhook/utility.hpp"

#if SAFETYHOOK_OS_WINDOWS

#define NOMINMAX
#if __has_include(<Windows.h>)
#include <Windows.h>
#elif __has_include(<windows.h>)
#include <windows.h>
#else
#error "Windows.h not found"
#endif

#include "safetyhook/os.hpp"

// Слой заморозки graft: им заменён апстримный trap_threads, см. ниже.
#include "graft/freeze.hpp"

namespace safetyhook {
std::expected<uint8_t*, OsError> vm_allocate(uint8_t* address, size_t size, VmAccess access) {
    DWORD protect = 0;

    if (access == VM_ACCESS_R) {
        protect = PAGE_READONLY;
    } else if (access == VM_ACCESS_RW) {
        protect = PAGE_READWRITE;
    } else if (access == VM_ACCESS_RX) {
        protect = PAGE_EXECUTE_READ;
    } else if (access == VM_ACCESS_RWX) {
        protect = PAGE_EXECUTE_READWRITE;
    } else {
        return std::unexpected{OsError::FAILED_TO_ALLOCATE};
    }

    auto* result = VirtualAlloc(address, size, MEM_COMMIT | MEM_RESERVE, protect);

    if (result == nullptr) {
        return std::unexpected{OsError::FAILED_TO_ALLOCATE};
    }

    return static_cast<uint8_t*>(result);
}

void vm_free(uint8_t* address) {
    VirtualFree(address, 0, MEM_RELEASE);
}

std::expected<uint32_t, OsError> vm_protect(uint8_t* address, size_t size, VmAccess access) {
    DWORD protect = 0;

    if (access == VM_ACCESS_R) {
        protect = PAGE_READONLY;
    } else if (access == VM_ACCESS_RW) {
        protect = PAGE_READWRITE;
    } else if (access == VM_ACCESS_RX) {
        protect = PAGE_EXECUTE_READ;
    } else if (access == VM_ACCESS_RWX) {
        protect = PAGE_EXECUTE_READWRITE;
    } else {
        return std::unexpected{OsError::FAILED_TO_PROTECT};
    }

    return vm_protect(address, size, protect);
}

std::expected<uint32_t, OsError> vm_protect(uint8_t* address, size_t size, uint32_t protect) {
    DWORD old_protect = 0;

    if (VirtualProtect(address, size, protect, &old_protect) == FALSE) {
        return std::unexpected{OsError::FAILED_TO_PROTECT};
    }

    return old_protect;
}

std::expected<VmBasicInfo, OsError> vm_query(uint8_t* address) {
    MEMORY_BASIC_INFORMATION mbi{};
    auto result = VirtualQuery(address, &mbi, sizeof(mbi));

    if (result == 0) {
        return std::unexpected{OsError::FAILED_TO_QUERY};
    }

    VmAccess access{};
    access.read = (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0;
    access.write = (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0;
    access.execute = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0;

    VmBasicInfo info{};
    info.address = static_cast<uint8_t*>(mbi.AllocationBase);
    info.size = mbi.RegionSize;
    info.access = access;
    info.is_free = mbi.State == MEM_FREE;

    return info;
}

bool vm_is_readable(uint8_t* address, size_t size) {
    return IsBadReadPtr(address, size) == FALSE;
}

bool vm_is_writable(uint8_t* address, size_t size) {
    return IsBadWritePtr(address, size) == FALSE;
}

bool vm_is_executable(uint8_t* address) {
    // Check if the address is in a valid module allowing us to potentially skip a heavier memory query.
    HMODULE image{};
    if (!GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPTSTR>(address), &image) ||
        image == nullptr) {
        return vm_query(address).value_or(VmBasicInfo{}).access.execute;
    }

    // Just check if the section is executable.
    const auto* image_base = reinterpret_cast<uint8_t*>(image);
    const auto* dos_hdr = reinterpret_cast<const IMAGE_DOS_HEADER*>(image_base);

    if (dos_hdr->e_magic != IMAGE_DOS_SIGNATURE) {
        return vm_query(address).value_or(VmBasicInfo{}).access.execute;
    }

    const auto* nt_hdr = reinterpret_cast<const IMAGE_NT_HEADERS*>(image_base + dos_hdr->e_lfanew);

    if (nt_hdr->Signature != IMAGE_NT_SIGNATURE) {
        return vm_query(address).value_or(VmBasicInfo{}).access.execute;
    }

    const auto* section = IMAGE_FIRST_SECTION(nt_hdr);

    for (auto i = 0; i < nt_hdr->FileHeader.NumberOfSections; ++i, ++section) {
        if (address >= image_base + section->VirtualAddress &&
            address < image_base + section->VirtualAddress + section->Misc.VirtualSize) {
            return (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        }
    }

    return vm_query(address).value_or(VmBasicInfo{}).access.execute;
}

SystemInfo system_info() {
    SystemInfo info{};

    SYSTEM_INFO si{};
    GetSystemInfo(&si);

    info.page_size = si.dwPageSize;
    info.allocation_granularity = si.dwAllocationGranularity;
    info.min_address = static_cast<uint8_t*>(si.lpMinimumApplicationAddress);
    info.max_address = static_cast<uint8_t*>(si.lpMaximumApplicationAddress);

    return info;
}

// ── ЛОКАЛЬНОЕ ИЗМЕНЕНИЕ GRAFT ────────────────────────────────────────────────
// Здесь у safetyhook стояли TrapInfo, TrapManager и trap_threads: патч писался под
// снятыми правами страницы, а обращения к ней ловил ВЕКТОРНЫЙ ОБРАБОТЧИК, заведённый
// с First = 1 и отвечавший «продолжай». Потоки при этом не останавливались вовсе.
//
// Для graft это неприемлемо по двум причинам, и обе проверяются кейсами Hook.* :
//
//   1. Обработчик встаёт ВЫШЕ кадрового. Библиотека обещает забрать падение натива
//      себе (guard.hpp, __try/__except) — это обещание не имеет права зависеть от
//      того, согласится ли чужой обработчик пропустить сбой дальше.
//   2. Список отравленных страниц не чистится НИКОГДА: каждая пара врезка+снятие
//      добавляет две записи навсегда. Дальше — линейный поиск по ним на каждом сбое
//      в процессе и растущий шанс, что настоящий сбой получит «продолжай», то есть
//      вечный цикл вместо честного падения с минидампом.
//
// Заменено на заморозку потоков (реализация — SRC/graft/hook.cpp, объявление —
// graft/freeze.hpp): потоки стоят, патч пишется, rip попавшим внутрь переставляется,
// после чего в процессе не остаётся ничего. Так же это делал MinHook, стоявший здесь
// до safetyhook, — поведение сбоев и падений от смены библиотеки не поехало.
void trap_threads(uint8_t* from, uint8_t* to, size_t len, const std::function<void()>& run_fn) {
    // Апстримная проверка, сохранена как есть: если трамплин уже освобождён, писать
    // в него нечего и незачем.
    MEMORY_BASIC_INFORMATION to_mbi{};
    if (VirtualQuery(to, &to_mbi, sizeof(to_mbi)) == 0 || to_mbi.State != MEM_COMMIT) {
        return;
    }

    const ::graft::freeze::scope frozen;

    // Бит исполнения НЕ снимаем: потоки и так стоят, а страница без него — готовая
    // ловушка, если что-то пойдёт не так между двумя VirtualProtect.
    DWORD from_was = 0;
    DWORD to_was = 0;
    const bool from_ok = VirtualProtect(from, len, PAGE_EXECUTE_READWRITE, &from_was) != FALSE;
    const bool to_ok = VirtualProtect(to, len, PAGE_EXECUTE_READWRITE, &to_was) != FALSE;

    if (run_fn) {
        run_fn();
    }

    if (to_ok) {
        VirtualProtect(to, len, to_was, &to_was);
    }
    if (from_ok) {
        VirtualProtect(from, len, from_was, &from_was);
    }
    FlushInstructionCache(GetCurrentProcess(), from, len);
    FlushInstructionCache(GetCurrentProcess(), to, len);

    // Потоки ещё стоят: переставляем rip тем, кто оказался внутри переписанных байт.
    // Снаружи области это уже опоздание.
    frozen.relocate(from, to, len);
}

void fix_ip(ThreadContext thread_ctx, uint8_t* old_ip, uint8_t* new_ip) {
    auto* ctx = reinterpret_cast<CONTEXT*>(thread_ctx);

#if SAFETYHOOK_ARCH_X86_64
    auto ip = ctx->Rip;
#elif SAFETYHOOK_ARCH_X86_32
    auto ip = ctx->Eip;
#endif

    if (ip == reinterpret_cast<uintptr_t>(old_ip)) {
        ip = reinterpret_cast<uintptr_t>(new_ip);
    }

#if SAFETYHOOK_ARCH_X86_64
    ctx->Rip = ip;
#elif SAFETYHOOK_ARCH_X86_32
    ctx->Eip = ip;
#endif
}

} // namespace safetyhook

#endif
