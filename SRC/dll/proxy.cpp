// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
// Точка входа graft-модуля: DLL под именем hid.dll рядом с exe игры. Все три бинаря —
// DayZ_x64.exe, DayZDiag_x64.exe и DayZServer_x64.exe — импортируют hid статически,
// поэтому наш код грузится вместе с exe: до main() и до регистрации нативов.
//
// Почему hid, а не dwmapi, с которой всё начиналось:
//   * dwmapi нет в таблице импорта ни боевого сервера, ни розничного клиента — она есть
//     только у diag. То есть хост заходил ровно в один бинарь из трёх, а `graft install`
//     в каталог сервера молча клал файл, который никто никогда не открывал;
//   * dwmapi экспортирует 119 функций, 75 из них только по ординалу, и в процессе к ней
//     ходят uxtheme, SHELL32 и OLEACC — в том числе по ординалам. Прокси на три имени
//     их импорты не закрывает, и держалось это лишь на том, что рядом в процесс по
//     полному пути форварда подтягивалась настоящая System32-копия.
// У hid всё наоборот: 47 экспортов, все именованные (экспортов только по ординалу нет
// вовсе), и во всём процессе её импортирует только сам exe — проверено по таблицам
// импорта всех модулей серверного процесса. В KnownDLLs её тоже нет: иначе загрузчик
// брал бы только System32-копию, как было с Normaliz.
//
// Перенаправляем ВСЕ 47, а не только пять HidP_*, которые импортирует движок: если в
// процесс придёт модуль, которому нужна шестая, он обязан получить настоящую функцию,
// а не отказ загрузки. Список — экспорты System32\hid.dll, по алфавиту.
// (Путь форварда — статическая строка: System-каталог на C: в наших установках.)
#pragma comment(linker, "/export:HidD_FlushQueue=C:\\Windows\\System32\\hid.HidD_FlushQueue")
#pragma comment(linker, "/export:HidD_FreePreparsedData=C:\\Windows\\System32\\hid.HidD_FreePreparsedData")
#pragma comment(linker, "/export:HidD_GetAttributes=C:\\Windows\\System32\\hid.HidD_GetAttributes")
#pragma comment(linker, "/export:HidD_GetConfiguration=C:\\Windows\\System32\\hid.HidD_GetConfiguration")
#pragma comment(linker, "/export:HidD_GetFeature=C:\\Windows\\System32\\hid.HidD_GetFeature")
#pragma comment(linker, "/export:HidD_GetHidGuid=C:\\Windows\\System32\\hid.HidD_GetHidGuid")
#pragma comment(linker, "/export:HidD_GetIndexedString=C:\\Windows\\System32\\hid.HidD_GetIndexedString")
#pragma comment(linker, "/export:HidD_GetInputReport=C:\\Windows\\System32\\hid.HidD_GetInputReport")
#pragma comment(linker, "/export:HidD_GetManufacturerString=C:\\Windows\\System32\\hid.HidD_GetManufacturerString")
#pragma comment(linker, "/export:HidD_GetMsGenreDescriptor=C:\\Windows\\System32\\hid.HidD_GetMsGenreDescriptor")
#pragma comment(linker, "/export:HidD_GetNumInputBuffers=C:\\Windows\\System32\\hid.HidD_GetNumInputBuffers")
#pragma comment(linker, "/export:HidD_GetPhysicalDescriptor=C:\\Windows\\System32\\hid.HidD_GetPhysicalDescriptor")
#pragma comment(linker, "/export:HidD_GetPreparsedData=C:\\Windows\\System32\\hid.HidD_GetPreparsedData")
#pragma comment(linker, "/export:HidD_GetProductString=C:\\Windows\\System32\\hid.HidD_GetProductString")
#pragma comment(linker, "/export:HidD_GetSerialNumberString=C:\\Windows\\System32\\hid.HidD_GetSerialNumberString")
#pragma comment(linker, "/export:HidD_Hello=C:\\Windows\\System32\\hid.HidD_Hello")
#pragma comment(linker, "/export:HidD_SetConfiguration=C:\\Windows\\System32\\hid.HidD_SetConfiguration")
#pragma comment(linker, "/export:HidD_SetFeature=C:\\Windows\\System32\\hid.HidD_SetFeature")
#pragma comment(linker, "/export:HidD_SetNumInputBuffers=C:\\Windows\\System32\\hid.HidD_SetNumInputBuffers")
#pragma comment(linker, "/export:HidD_SetOutputReport=C:\\Windows\\System32\\hid.HidD_SetOutputReport")
#pragma comment(linker, "/export:HidP_GetButtonArray=C:\\Windows\\System32\\hid.HidP_GetButtonArray")
#pragma comment(linker, "/export:HidP_GetButtonCaps=C:\\Windows\\System32\\hid.HidP_GetButtonCaps")
#pragma comment(linker, "/export:HidP_GetCaps=C:\\Windows\\System32\\hid.HidP_GetCaps")
#pragma comment(linker, "/export:HidP_GetData=C:\\Windows\\System32\\hid.HidP_GetData")
#pragma comment(linker, "/export:HidP_GetExtendedAttributes=C:\\Windows\\System32\\hid.HidP_GetExtendedAttributes")
#pragma comment(linker, "/export:HidP_GetLinkCollectionNodes=C:\\Windows\\System32\\hid.HidP_GetLinkCollectionNodes")
#pragma comment(linker, "/export:HidP_GetScaledUsageValue=C:\\Windows\\System32\\hid.HidP_GetScaledUsageValue")
#pragma comment(linker, "/export:HidP_GetSpecificButtonCaps=C:\\Windows\\System32\\hid.HidP_GetSpecificButtonCaps")
#pragma comment(linker, "/export:HidP_GetSpecificValueCaps=C:\\Windows\\System32\\hid.HidP_GetSpecificValueCaps")
#pragma comment(linker, "/export:HidP_GetUsageValue=C:\\Windows\\System32\\hid.HidP_GetUsageValue")
#pragma comment(linker, "/export:HidP_GetUsageValueArray=C:\\Windows\\System32\\hid.HidP_GetUsageValueArray")
#pragma comment(linker, "/export:HidP_GetUsages=C:\\Windows\\System32\\hid.HidP_GetUsages")
#pragma comment(linker, "/export:HidP_GetUsagesEx=C:\\Windows\\System32\\hid.HidP_GetUsagesEx")
#pragma comment(linker, "/export:HidP_GetValueCaps=C:\\Windows\\System32\\hid.HidP_GetValueCaps")
#pragma comment(linker, "/export:HidP_GetVersionInternal=C:\\Windows\\System32\\hid.HidP_GetVersionInternal")
#pragma comment(linker, "/export:HidP_InitializeReportForID=C:\\Windows\\System32\\hid.HidP_InitializeReportForID")
#pragma comment(linker, "/export:HidP_MaxDataListLength=C:\\Windows\\System32\\hid.HidP_MaxDataListLength")
#pragma comment(linker, "/export:HidP_MaxUsageListLength=C:\\Windows\\System32\\hid.HidP_MaxUsageListLength")
#pragma comment(linker, "/export:HidP_SetButtonArray=C:\\Windows\\System32\\hid.HidP_SetButtonArray")
#pragma comment(linker, "/export:HidP_SetData=C:\\Windows\\System32\\hid.HidP_SetData")
#pragma comment(linker, "/export:HidP_SetScaledUsageValue=C:\\Windows\\System32\\hid.HidP_SetScaledUsageValue")
#pragma comment(linker, "/export:HidP_SetUsageValue=C:\\Windows\\System32\\hid.HidP_SetUsageValue")
#pragma comment(linker, "/export:HidP_SetUsageValueArray=C:\\Windows\\System32\\hid.HidP_SetUsageValueArray")
#pragma comment(linker, "/export:HidP_SetUsages=C:\\Windows\\System32\\hid.HidP_SetUsages")
#pragma comment(linker, "/export:HidP_TranslateUsagesToI8042ScanCodes=C:\\Windows\\System32\\hid.HidP_TranslateUsagesToI8042ScanCodes")
#pragma comment(linker, "/export:HidP_UnsetUsages=C:\\Windows\\System32\\hid.HidP_UnsetUsages")
#pragma comment(linker, "/export:HidP_UsageListDifference=C:\\Windows\\System32\\hid.HidP_UsageListDifference")

#include <windows.h>

#include "graft/abi.h"
#include "graft/engine.hpp"

// Метка «эта hid.dll — наша». По ней `graft install` отличает свой хост от чужого
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
        // не избежать: она называется hid.dll и лежит рядом с exe, а его таблица импорта
        // тянет её до main(). Но дальше этого дело не идёт: ни скана, ни хуков, ни
        // плагинов, ни строки в журнале. Остаются только форварды выше — ровно то, ради
        // чего движок эту библиотеку и импортирует.
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
