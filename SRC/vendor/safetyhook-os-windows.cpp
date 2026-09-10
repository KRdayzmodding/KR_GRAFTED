// safetyhook v0.7.0 — src/os.windows.cpp, ВЕНДОРНАЯ КОПИЯ С ОДНОЙ ЗАМЕНОЙ.
// Boost Software License 1.0, (C) cursey. Полный текст условий — THIRD_PARTY.md.
//
// Заменена ровно одна функция, trap_threads. Всё остальное — байт в байт как в
// апстриме, чтобы обновление сводилось к новой копии и повтору этой одной замены.
// Что именно заменено и почему — в комментарии у самой функции ниже.

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
// Здесь у safetyhook стояли TrapInfo, TrapManager и векторный обработчик: патч писался
// под снятыми правами страницы, а тех, кто по ней исполнялся, обработчик ловил и
// переставлял им rip в трамплин.
//
// В graft патч идёт ПОД ЗАМОРОЗКОЙ ПОТОКОВ (SRC/graft/hook.cpp, класс frozen_threads):
// исполняться по странице в этот момент некому, и ловушка не срабатывает ни разу.
// Заморозка при этом обязательна не ради неё, а сама по себе — она единственная
// закрывает поток, СНЯТЫЙ ПЛАНИРОВЩИКОМ внутри переписываемых байт: сбоя у такого
// потока нет, ловить нечего, а проснётся он на середине новой инструкции.
//
// Оставлять мёртвую ловушку нельзя, и это не вкусовщина. Её список отравленных страниц
// НЕ ЧИСТИТСЯ никогда: каждая пара врезка+снятие добавляет две записи навсегда, а
// обработчик отвечает «продолжай» на любой сбой, попавший на такую страницу. Настоящее
// падение на ней превратилось бы в вечный цикл вместо дампа — ровно то, что забирает у
// guard.hpp его обещание.
//
// ВНИМАНИЕ: после этой замены trap_threads НИЧЕГО не защищает сам. Единственные её
// вызывающие — enable и disable у InlineHook, и обе обёрнуты заморозкой в hook.cpp.
// Появится третий вызывающий — обернуть обязан и он.
void trap_threads(uint8_t* from, uint8_t* to, size_t len, const std::function<void()>& run_fn) {
    // Апстримная проверка, сохранена как есть: если трамплин уже освобождён, писать в
    // него нечего и незачем.
    MEMORY_BASIC_INFORMATION to_mbi{};
    if (VirtualQuery(to, &to_mbi, sizeof(to_mbi)) == 0 || to_mbi.State != MEM_COMMIT) {
        return;
    }

    // Бит исполнения НЕ снимаем: потоки и так стоят, а страница кода без него — готовая
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
