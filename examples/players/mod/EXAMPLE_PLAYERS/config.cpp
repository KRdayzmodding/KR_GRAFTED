class CfgPatches
{
    class EXAMPLE_PLAYERS
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
    class EXAMPLE_PLAYERS
    {
        type = "mod";
        dependencies[] = {"Core", "World", "Game", "Mission"};

        class defs
        {
            class engineScriptModule
            {
                value = "";
                files[] = {"EXAMPLE_PLAYERS/scripts/1_Core"};
            };
            class missionScriptModule
            {
                value = "";
                files[] = {"EXAMPLE_PLAYERS/scripts/5_Mission"};
            };
        };
    };
};
