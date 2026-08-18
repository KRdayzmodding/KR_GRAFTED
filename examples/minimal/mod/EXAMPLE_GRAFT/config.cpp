// PBO-часть примера: объявляет класс-хозяин нативных методов, подхватывает
// сгенерённые `proto native` и (опционально) сьюту uTest.
// KR_CORE нужен только тестам — если сьюта не нужна, убери его и scripts/3_Game.
class CfgPatches
{
    class EXAMPLE_GRAFT
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
    class EXAMPLE_GRAFT
    {
        type = "mod";
        dependencies[] = {"Core", "World", "Game", "Mission"};

        class defs
        {
            class engineScriptModule
            {
                value = "";
                files[] = {"EXAMPLE_GRAFT/scripts/1_Core"};
            };
            class gameScriptModule
            {
                value = "";
                files[] = {"EXAMPLE_GRAFT/scripts/3_Game"};
            };
        };
    };
};
