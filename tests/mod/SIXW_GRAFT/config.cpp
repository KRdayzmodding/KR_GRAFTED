// PBO-часть graft-модуля. Объявляет proto native SeraphGraftPing (его impl даёт
// C++ proxy-DLL dwmapi.dll) и uTest-сьюту seraph::graft, проверяющую связь скрипт<->C++.
// Внешних зависимостей нет: фреймворк тестов лежит рядом файлом scripts/3_Game/uTest.c.
class CfgPatches
{
    class SIXW_GRAFT
    {
        units[] = {};
        weapons[] = {};
        ammo[] = {};
        requiredVersion = 0.1;
        requiredAddons[] = {"DZ_Data", "DZ_Scripts"};
    };
};

class CfgMods
{
    class SIXW_GRAFT
    {
        type = "mod";
        dependencies[] = {"Core", "World", "Game", "Mission"};

        class defs
        {
            class engineScriptModule
            {
                value = "";
                files[] = {"SIXW_GRAFT/scripts/1_Core"};
            };
            class gameScriptModule
            {
                value = "";
                files[] = {"SIXW_GRAFT/scripts/3_Game"};
            };
        };
    };
};
