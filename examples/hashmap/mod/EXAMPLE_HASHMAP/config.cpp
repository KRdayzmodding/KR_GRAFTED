// PBO-часть примера. Скриптовых файлов своей рукой здесь нет вообще: единственный
// файл в scripts/1_Core — grafted_natives_EXAMPLE_HASHMAP.c, и его печатает сборка.
class CfgPatches
{
    class EXAMPLE_HASHMAP
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
    class EXAMPLE_HASHMAP
    {
        type = "mod";
        dependencies[] = {"Core", "World", "Game", "Mission"};

        class defs
        {
            class engineScriptModule
            {
                value = "";
                files[] = {"EXAMPLE_HASHMAP/scripts/1_Core"};
            };
        };
    };
};
