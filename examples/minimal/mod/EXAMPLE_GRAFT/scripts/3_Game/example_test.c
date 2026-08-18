#ifndef UTESTS
// Проверка связи скрипт <-> C++. Запуск: dayz-mcp run_suites(["example::graft"]).
// Сьюту нужно завести в dayz-mcp.config.json (utest.sandboxPbos + knownSuites).
//
// ВНИМАНИЕ: в теле co_routine нельзя писать '&&' — компилятор Enforce на нём падает.
// Условия собирай вложенными if.
[TEST_SUITE("example::graft", EXAMPLE_GRAFT_TEST)];
class EXAMPLE_GRAFT_TEST : uTestSuite
{
    [TEST_CASE("Add").IN(EXAMPLE_GRAFT_TEST)];
    void Add(co_call ctx)
    {
        co_routine coro = co_new(ctx);
        int got = ExampleAdd(2, 40);
        assert(got == 42, "42", got.ToString(), "int туда и обратно");
    }

    [TEST_CASE("Greet").IN(EXAMPLE_GRAFT_TEST)];
    void Greet(co_call ctx)
    {
        co_routine coro = co_new(ctx);
        string got = ExampleGreet("dayz");
        assert(got == "hello, dayz", "hello, dayz", got, "string на вход, owned string назад");
    }

    [TEST_CASE("MemberMethod").IN(EXAMPLE_GRAFT_TEST)];
    void MemberMethod(co_call ctx)
    {
        co_routine coro = co_new(ctx);
        ExampleGraft inst = new ExampleGraft;
        int got = inst.Version();   // обычный член, как у ванильных коллекций
        assert(got == 1, "1", got.ToString(), "натив, привязанный методом класса");
    }

    [TEST_CASE("Ranges").IN(EXAMPLE_GRAFT_TEST)];
    void Ranges(co_call ctx)
    {
        co_routine coro = co_new(ctx);
        array<int> nums = new array<int>;
        nums.Insert(4);
        nums.Insert(8);
        nums.Insert(15);
        int above = ExampleCountAbove(nums, 5);
        assert(above == 2, "2", above.ToString(), "std::ranges по скриптовому массиву");
    }

    [TEST_CASE("FillFromCpp").IN(EXAMPLE_GRAFT_TEST)];
    void FillFromCpp(co_call ctx)
    {
        co_routine coro = co_new(ctx);
        array<int> squares = new array<int>;   // пустой: размер поставит C++
        int n = ExampleFillSquares(squares, 4);
        bool ok = false;
        if (n == 4)
        {
            if (squares.Get(3) == 9)
                ok = true;
        }
        assert(ok, "4 / 9", n.ToString() + " / " + squares.Get(3).ToString(),
            "C++ сам растит и заполняет массив");
    }

    [TEST_CASE("Array").IN(EXAMPLE_GRAFT_TEST)];
    void Array(co_call ctx)
    {
        co_routine coro = co_new(ctx);
        array<int> nums = new array<int>;
        nums.Insert(1);
        nums.Insert(2);
        nums.Insert(39);
        int got = ExampleSum(nums);
        assert(got == 42, "42", got.ToString(), "C++ читает скриптовый array<int>");
    }
}
#endif
