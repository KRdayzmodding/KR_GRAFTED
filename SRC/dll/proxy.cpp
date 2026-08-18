// Точка входа graft-модуля: DLL под именем dwmapi.dll рядом с DayZDiag_x64.exe /
// DayZServer_x64.exe. Движок импортирует dwmapi статически, поэтому наш код грузится
// вместе с exe — до main() и до регистрации нативов.
//
// Почему именно dwmapi: имя из таблицы импорта exe, не входит в KnownDLLs (иначе
// загрузчик берёт только System32-копию — так было с Normaliz), гарантированно есть
// в System32, и в серверном процессе его импортит только сам exe. Три реальные функции
// прозрачно перенаправляем в системную копию, чтобы движок работал как обычно.
// (Путь форварда — статическая строка: System-каталог на C: в наших установках.)
#pragma comment(linker, "/export:DwmExtendFrameIntoClientArea=C:\\Windows\\System32\\dwmapi.DwmExtendFrameIntoClientArea")
#pragma comment(linker, "/export:DwmGetWindowAttribute=C:\\Windows\\System32\\dwmapi.DwmGetWindowAttribute")
#pragma comment(linker, "/export:DwmSetWindowAttribute=C:\\Windows\\System32\\dwmapi.DwmSetWindowAttribute")

#include <windows.h>

#include "graft/abi.h"
#include "graft/engine.hpp"

// Метка «эта dwmapi.dll — наша». По ней `graft install` отличает свой хост от чужого
// прокси и отказывается затирать чужой.
extern "C" __declspec(dllexport) unsigned __cdecl graft_host_version() {
    return GRAFT_ABI_VERSION;
}

namespace {

DWORD WINAPI Install(LPVOID) {
    graft::install();
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        // В клиенте не делаем НИЧЕГО — даже потока не заводим. Загрузки самой библиотеки
        // не избежать: она называется dwmapi.dll и лежит рядом с exe, а его таблица
        // импорта тянет её до main(). Но дальше этого дело не идёт: ни скана, ни хуков,
        // ни плагинов, ни строки в журнале. Остаются только три форварда выше — ровно то,
        // ради чего движок эту библиотеку и импортирует.
        //
        // Решение принимается ЗДЕСЬ, а не внутри install(): проверка стоит двух вызовов
        // Win32 и в DllMain безопасна, а поток в чужом процессе — уже вмешательство.
        if (!graft::serving()) {
            return TRUE;
        }
        // Работу делаем в отдельном потоке: в DllMain нельзя грузить библиотеки и
        // ждать лоадер-лок.
        CloseHandle(CreateThread(nullptr, 0, &Install, nullptr, 0, nullptr));
    }
    return TRUE;
}
