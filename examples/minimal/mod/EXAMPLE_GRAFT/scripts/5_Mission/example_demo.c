// Демонстрация: каждый натив из src/natives.cpp — по строчке в script-логе.
// Ни одного `proto native` руками здесь нет: объявления печатает сборка в 1_Core.
//
//   [EXAMPLE_GRAFT] Add(2, 40) = 42
//   [EXAMPLE_GRAFT] Greet("dayz") = hello, dayz
//   [EXAMPLE_GRAFT] Version() = 1
//   [EXAMPLE_GRAFT] Sum([1,2,39]) = 42
//   [EXAMPLE_GRAFT] CountAbove([4,8,15], 5) = 2
//   [EXAMPLE_GRAFT] FillSquares(4) -> 0,1,4,9
//   [EXAMPLE_GRAFT] Scale(<1,2,3>, 2.5) = <2.5,5,7.5>
modded class MissionBase
{
    override void OnInit()
    {
        super.OnInit();

        // Числа и строки: обычные функции C++.
        int sum = ExampleAdd(2, 40);
        Print("[EXAMPLE_GRAFT] Add(2, 40) = " + sum.ToString());
        Print("[EXAMPLE_GRAFT] Greet(\"dayz\") = " + ExampleGreet("dayz"));

        // Метод скриптового класса: объект приезжает в C++ первым аргументом (это this),
        // в объявлении его нет — ровно как у ванильных коллекций.
        ExampleGraft inst = new ExampleGraft;
        int version = inst.Version();
        Print("[EXAMPLE_GRAFT] Version() = " + version.ToString());

        array<int> nums = new array<int>;
        nums.Insert(1);
        nums.Insert(2);
        nums.Insert(39);
        // Копией: в сигнатуре C++ стоит std::vector<int>.
        int total = ExampleSum(nums);
        Print("[EXAMPLE_GRAFT] Sum([1,2,39]) = " + total.ToString());

        array<int> more = new array<int>;
        more.Insert(4);
        more.Insert(8);
        more.Insert(15);
        // Без копии: graft::array<int> — вьюха прямо в память движка, по ней в C++
        // ходят std::ranges.
        int above = ExampleCountAbove(more, 5);
        Print("[EXAMPLE_GRAFT] CountAbove([4,8,15], 5) = " + above.ToString());

        // out-массив: размер ставит C++ через движковый Resize, числа пишет сам.
        array<int> squares = new array<int>;
        ExampleFillSquares(squares, 4);
        string line = "";
        for (int i = 0; i < squares.Count(); i++)
        {
            int square = squares.Get(i);
            if (i > 0)
                line = line + ",";
            line = line + square.ToString();
        }
        Print("[EXAMPLE_GRAFT] FillSquares(4) -> " + line);

        // vector — это std::array<float,3> в C++.
        vector scaled = ExampleScale(Vector(1, 2, 3), 2.5);
        Print("[EXAMPLE_GRAFT] Scale(<1,2,3>, 2.5) = " + scaled.ToString());

        // Журналы игры из C++. Ни одного Print в этих двух строках нет — их пишет сам
        // плагин: первая ляжет в script-лог рядом с этими, вторая — в crash-лог.
        // Обе возвращают, дошло ли до движка (до первого тика корня ещё нет).
        bool said = ExampleSay("это строка в script-лог, её напечатал C++");
        bool cried = ExampleComplain("а это строка в crash-лог, тоже из C++ (демонстрация, не сбой)");
        Print("[EXAMPLE_GRAFT] Say -> " + said.ToString() + ", Complain -> " + cried.ToString());
    }
}
