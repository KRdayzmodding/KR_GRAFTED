// Скриптовая половина примера. Класса CppHashMap здесь НЕ объявлено: его целиком —
// вместе с деструктором — печатает генератор в scripts/1_Core. Тут только работа с ним.
//
// В script-логе после запуска:
//   [EXAMPLE_HASHMAP] count=2 zombies=12
//   [EXAMPLE_HASHMAP] обход: bandits=3 zombies=12
//   [EXAMPLE_HASHMAP] byId: 1=Chernarus 2=Livonia
//   [EXAMPLE_HASHMAP] после delete: count=0
modded class MissionBase
{
    override void OnInit()
    {
        super.OnInit();

        // Обычный new: объект скриптовый, а таблица за ним — std::unordered_map в C++.
        CppHashMap<string, int> kills = new CppHashMap<string, int>;
        kills.Set("zombies", 12);
        kills.Set("bandits", 3);
        int count = kills.Count();
        int zombies = kills.Get("zombies");
        Print("[EXAMPLE_HASHMAP] count=" + count.ToString() + " zombies=" + zombies.ToString());

        // Обход: ключи отдаются по одному (заливать array<string> из C++ нельзя — за
        // строками движок считает ссылки). Порядок хеш-таблицы не определён, но между
        // вызовами не меняется.
        string line = "";
        for (int i = 0; i < count; i++)
        {
            string key = kills.KeyAt(i);
            int value = kills.Get(key);
            line = line + key + "=" + value.ToString() + " ";
        }
        Print("[EXAMPLE_HASHMAP] обход: " + line);

        // Другая пара типов — ДРУГАЯ инстанциация того же шаблона C++. Ни строчки
        // нового кода: ни в C++, ни здесь.
        CppHashMap<int, string> byId = new CppHashMap<int, string>;
        byId.Set(1, "Chernarus");
        byId.Set(2, "Livonia");
        Print("[EXAMPLE_HASHMAP] byId: 1=" + byId.Get(1) + " 2=" + byId.Get(2));

        // Владение обычное: умер скриптовый объект — движок позвал ~CppHashMap, тот
        // позвал NativeDispose_EXAMPLE_HASHMAP, и std::unordered_map в C++ снесён.
        // Новая таблица про старую ничего не знает.
        delete kills;
        CppHashMap<string, int> fresh = new CppHashMap<string, int>;
        int empty = fresh.Count();
        Print("[EXAMPLE_HASHMAP] после delete: count=" + empty.ToString());
    }
}
