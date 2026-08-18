// Единственная скриптовая строчка этого мода: зовём натив из C++ и печатаем ответ.
//
// MissionBase — общий предок и клиентской, и серверной миссии, поэтому строка появится
// и в обычной игре, и на сервере. Смотреть её в script-логе рядом с exe.
modded class MissionBase
{
    override void OnInit()
    {
        super.OnInit();
        Print("[HELLO_GRAFT] " + HelloGraft("DayZ"));
    }
}
