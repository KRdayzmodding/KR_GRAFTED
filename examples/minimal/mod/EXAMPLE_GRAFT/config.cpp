// PBO-часть примера. Зависимость только от ванильных данных: ни CF, ни фреймворка
// тестов примеру не нужно.
class CfgPatches
{
    class EXAMPLE_GRAFT
    {
        units[] = {};
        weapons[] = {};
        ammo[] = {};
        requiredVersion = 0.1;
        requiredAddons[] = {"DZ_Data"};
    };
};

class CfgMods
{
    class EXAMPLE_GRAFT
    {
        type = "mod";
        dependencies[] = {"Core", "World", "Game", "Mission"};

        class defs
        {
            // 1_Core — класс-хозяин (example_class.c) и объявления от генератора.
            class engineScriptModule
            {
                value = "";
                files[] = {"EXAMPLE_GRAFT/scripts/1_Core"};
            };
            // 5_Mission — демонстрация: отсюда нативы зовут.
            class missionScriptModule
            {
                value = "";
                files[] = {"EXAMPLE_GRAFT/scripts/5_Mission"};
            };
        };
    };
};
