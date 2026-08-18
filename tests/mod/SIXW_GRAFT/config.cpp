// PBO-часть graft-модуля. Объявляет proto native SeraphGraftPing (его impl даёт
// C++ proxy-DLL dwmapi.dll) и uTest-сьюту seraph::graft, проверяющую связь скрипт<->C++.
// Зависит от KR_CORE (uTest-фреймворк). Собирается dzrun-ом, имя папки = stem PBO.
class CfgPatches
{
    class SIXW_GRAFT
    {
        units[] = {};
        weapons[] = {};
        ammo[] = {};
        requiredVersion = 0.1;
        requiredAddons[] = {"DZ_Data", "JM_CF_Scripts", "KR_CORE"};
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
