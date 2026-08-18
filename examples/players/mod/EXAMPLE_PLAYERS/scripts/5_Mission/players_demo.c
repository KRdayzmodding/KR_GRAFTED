// Скриптовая половина примера. Данные собирает C++ САМ, по тику: скрипт его об этом не
// просит и о тике вообще не знает. Здесь мы только СПРАШИВАЕМ уже собранное.
//
// В script-логе на сервере:
//   [EXAMPLE_PLAYERS] debug monitor (поле CGame из C++) = 0
//   [EXAMPLE_PLAYERS] тик C++ работает 10.0 с, известно игроков: 1
//   [EXAMPLE_PLAYERS] 76561198000000000 -> Survivor @ 7500 300 7500, тиков 4
modded class MissionBase
{
    override void OnInit()
    {
        super.OnInit();

        // Поле движкового объекта, прочитанное из C++ по имени: game["m_DebugMonitorEnabled"_f].
        int flag = ExampleDebugMonitor();
        Print("[EXAMPLE_PLAYERS] debug monitor (поле CGame из C++) = " + flag.ToString());

        if (GetGame().IsServer())
            GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(this.GraftPlayersReport, 10000, true);
    }

    // Раз в десять секунд печатаем то, что C++ накопил своим тиком.
    void GraftPlayersReport()
    {
        // Секунды считает C++ у себя в тике — скрипт его об этом не просит.
        float uptime = ExampleUptime();
        int known = ExamplePlayersKnown();
        Print("[EXAMPLE_PLAYERS] тик C++ работает " + uptime.ToString() + " с, известно игроков: " + known.ToString());

        array<Man> players = new array<Man>;
        GetGame().GetPlayers(players);
        foreach (Man man : players)
        {
            PlayerIdentity id = man.GetIdentity();
            if (!id)
                continue;
            // Ключ тот же, что C++ положил себе в std::map: steam id.
            string steam = id.GetPlainId();
            Print("[EXAMPLE_PLAYERS] " + steam + " -> " + ExamplePlayerReport(steam));
        }
    }
}
