// PBO-часть примера. Класс CppHashMap своей рукой НЕ объявлен: и методы, и деструктор
// печатает сборка в scripts/1_Core/grafted_natives_EXAMPLE_HASHMAP.c. Своего кода —
// только демонстрация в scripts/5_Mission.
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
            // 1_Core — сюда генератор кладёт объявление класса.
            class engineScriptModule
            {
                value = "";
                files[] = {"EXAMPLE_HASHMAP/scripts/1_Core"};
            };
            // 5_Mission — отсюда мы им пользуемся.
            class missionScriptModule
            {
                value = "";
                files[] = {"EXAMPLE_HASHMAP/scripts/5_Mission"};
            };
        };
    };
};
