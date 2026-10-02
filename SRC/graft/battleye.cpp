// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include "graft/battleye.hpp"

#include <windows.h>

#include "graft/engine.hpp"

// Наблюдение за BEClient_x64.dll. Почему именно так — в battleye.hpp.
//
// Уведомление загрузчика (LdrRegisterDllNotification) — штатное средство ntdll: зовёт нас
// на каждую загрузку модуля, на любом потоке, без опроса и без врезки в LoadLibrary.
// Врезка в kernel32 для этого была бы куда навязчивее, и ставить её в процесс, где
// сидит античит, незачем.
namespace graft::battleye {
namespace {

// Структур уведомления в SDK нет — своя запись ровно под нужные поля (ntldr.h).
struct unicode_string {
    USHORT length;
    USHORT max_length;
    PWSTR  buffer;
};

struct loaded_data {
    ULONG                 flags;
    const unicode_string* full_name;
    const unicode_string* base_name;
    PVOID                 dll_base;
    ULONG                 size_of_image;
};

constexpr ULONG kReasonLoaded = 1;
constexpr UINT  kExitCode     = 0xBE; // видно в коде завершения, кто убил процесс

using callback_fn   = void(NTAPI*)(ULONG reason, const loaded_data* data, PVOID context);
using register_fn   = LONG(NTAPI*)(ULONG flags, callback_fn fn, PVOID context, PVOID* cookie);
using unregister_fn = LONG(NTAPI*)(PVOID cookie);

found_fn g_found  = nullptr;
PVOID    g_cookie = nullptr;

FARPROC ntdll(const char* name) {
    const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    return nt ? GetProcAddress(nt, name) : nullptr;
}

void NTAPI on_dll(ULONG reason, const loaded_data* data, PVOID) {
    if (reason != kReasonLoaded || !data || !data->base_name || !data->base_name->buffer || !g_found) {
        return;
    }
    const unicode_string& name = *data->base_name;
    if (is_client_module({name.buffer, name.length / sizeof(wchar_t)})) {
        g_found();
    }
}

} // namespace

bool watch(found_fn on_found) {
    const auto reg = reinterpret_cast<register_fn>(ntdll("LdrRegisterDllNotification"));
    if (!reg || g_cookie || !on_found) {
        return false;
    }
    g_found = on_found;
    // NTSTATUS: отрицательный — отказ.
    if (reg(0, &on_dll, nullptr, &g_cookie) < 0) {
        g_cookie = nullptr;
        return false;
    }
    // Модуль мог загрузиться ДО подписки. Проверяем ПОСЛЕ неё, а не до: иначе между
    // проверкой и подпиской остаётся окно, в которое он успел бы проскочить.
    if (GetModuleHandleW(kClientModule.data())) {
        on_found();
    }
    return true;
}

void unwatch() {
    const auto unreg = reinterpret_cast<unregister_fn>(ntdll("LdrUnregisterDllNotification"));
    if (unreg && g_cookie) {
        unreg(g_cookie);
    }
    g_cookie = nullptr;
    g_found  = nullptr;
}

void kill() {
    log("! BattlEye in the process (BEClient_x64.dll) - terminating the game: graft does not work under BattlEye");
    TerminateProcess(GetCurrentProcess(), kExitCode);
    for (;;) { // TerminateProcess не возвращается; цикл — для [[noreturn]]
    }
}

} // namespace graft::battleye
