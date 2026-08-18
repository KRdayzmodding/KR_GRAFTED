// PBO-часть мода: где лежат скрипты и в каких модулях движка их разбирать.
// Зависимость только от ванильных данных — ни CF, ни чего-то ещё этому примеру не надо.
class CfgPatches
{
    class HELLO_GRAFT
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
    class HELLO_GRAFT
    {
        type = "mod";
        dependencies[] = {"Core", "World", "Game", "Mission"};

        class defs
        {
            // 3_Game — сюда генератор кладёт объявления нативов.
            class gameScriptModule
            {
                value = "";
                files[] = {"HELLO_GRAFT/scripts/3_Game"};
            };
            // 5_Mission — отсюда мы их зовём.
            class missionScriptModule
            {
                value = "";
                files[] = {"HELLO_GRAFT/scripts/5_Mission"};
            };
        };
    };
};
