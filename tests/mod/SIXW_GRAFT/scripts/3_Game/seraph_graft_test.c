// uTest-сьюта на KR_UTEST — фреймворк лежит рядом файлом uTest.c (MIT), внешних
// зависимостей у мода нет. Матрица ABI graft-модуля: по кейсу на каждый тип, который
// умеет `proto native`. Импл — C++ в proxy-DLL (SRC/seraph/natives.cpp), объявления
// сгенерены graft_protogen. После патча игры именно эта сьюта скажет, не поехала ли
// привязка.
//
// Кейс — обычный `void Имя()`: раннер зовёт его по имени через GameScript.CallFunction.
// Запрет на '&&' был свойством тела корутины (компилятор Enforce на нём падал) —
// здесь корутин нет, и запрет снят, но уже написанные вложенные if не трогаем.

// Секундомер замеров. Раньше брался LOG_DURATION из KR_CORE; теперь мод не зависит ни
// от чего, а мера та же: TickCount(0) отдаёт абсолютную отметку в сотнях наносекунд,
// поэтому дельта делится на 10000 и получаются миллисекунды.
class SeraphClock : Managed
{
    private int m_mark;

    void SeraphClock() { m_mark = TickCount(0); }

    //! Миллисекунды с прошлой отметки; отметка сдвигается на сейчас.
    float Exchange()
    {
        int now = TickCount(0);
        int delta = now - m_mark;
        m_mark = now;
        return delta / 10000.0;
    }

    static string Format(float ms)
    {
        if (ms >= 1000) return (ms / 1000).ToString() + " s";
        if (ms >= 1) return ms.ToString() + " ms";
        return (ms * 1000).ToString() + " us";
    }
}

[TEST_SUITE("seraph::graft", SERAPH_GRAFT_TEST)];
class SERAPH_GRAFT_TEST : uTestSuite
{
    // Потолки для Perf_HotPathBudget — в единицах «ванильный map.Count()», то есть
    // «сколько пустых вызовов через скриптовую машину стоит эта операция».
    //
    // Замер — минимум из шести раундов (см. сам кейс). Измеренное на этой машине:
    //
    //                     одна DLL      хост + 2 плагина
    //   bridge            1.199 1.203   1.170 1.170 1.275
    //   field             1.112 1.138   1.008 1.039 1.042
    //   obj               1.086 1.023   1.021 1.028 1.045
    //   text              2.357 2.217   2.056 2.170 2.215
    //   marshal           1.499 1.436   1.380 1.400 1.495
    //   lookupRT              —         20.13 20.66 24.60
    //   lookupCT              —         0.895 0.939 1.007
    //   textLong              —         4.40  4.47  4.96
    //   textArena             —         3.08  3.86  4.27
    //
    // Отдельным заходом пробовали выдавать строки АЛЛОКАТОРОМ САМОГО ДВИЖКА (владение
    // переходит ему, своей памяти ноль). Отвергнуто замером: text 3.14 против 2.36,
    // textLong 5.81 против 4.68, textArena 4.61 против 3.56 — и, главное, отброшенные
    // строки текли (+14.7 МБ на 200000 против +0 у кольца). Подробности в text.cpp.
    //   arrVec                —         1.74  1.86  1.98
    //   arrSpan               —         0.94  1.09  1.13
    //   fieldProxy            —         0.99
    //   protoOut              —         4.33  5.60
    //   missing               —         1.19  0.95
    //   guarded               —         0.94
    //
    // fieldProxy — то же поле, что и field, но синтаксисом sol2 (`node["m_id"_f]`).
    // 0.99 против 1.04: сахар бесплатный, и это не совпадение — имя живёт в ТИПЕ,
    // поэтому кэш слота у обеих форм один и тот же. Динамический язык так не умеет.
    //
    // protoOut — вызов В ДРУГУЮ СТОРОНУ: движковый маршалируемый `proto` с четырьмя
    // аргументами и out-параметром (EnScript.GetClassVar). Около 200 нс: собирается блок
    // из четырёх 40-байтных переменных, каждая копируется из шаблона движка. Дороже
    // вызова внутрь по природе — так ходят за тем, чего прямым `proto native` не достать.
    //
    // missing — вызов объявления, КОТОРОГО В ЭТОЙ СБОРКЕ ИГРЫ НЕТ. Зеркало печатает
    // объединение всех веток препроцессора (дефайны запущенной игры узнать неоткуда),
    // поэтому такие вызовы законны и должны быть дешёвыми. 1.19 против bridge 1.24 —
    // то есть ровно цена перехода в натив, добавленной нет вовсе: поиск прекращается
    // после восьми промахов, дальше это чтение статической ячейки. Без потолка тут был
    // бы полный поиск на каждый вызов, а это 26 единиц (см. lookupRT).
    //
    // guarded — натив, обёрнутый в __try/__except. 0.94 против bridge 1.21: защиты от
    // падения в рантайме НЕТ ВООБЩЕ, и это не удача, а устройство x64-SEH — записи
    // раскрутки лежат в .pdata и читаются только когда исключение уже случилось.
    //
    // Раскол на модули не стоил ничего: движок зовёт трамплин плагина НАПРЯМУЮ, а всё,
    // что трамплин трогает по дороге, скомпилировано в тот же плагин.
    //
    // textLong/textArena — один и тот же возвращаемый текст в 64 байта. В первом случае
    // натив вернул std::string (аллокация + копия в арену), во втором — graft::text::of,
    // который форматирует ПРЯМО в арену. Разница 21% и есть цена одной аллокации. На
    // короткой строке её не видно вовсе: до 15 байт std::string живёт в своём SSO-буфере
    // и кучу не трогает — поэтому метрика text (короткая) и заведена отдельно.
    //
    // arrVec/arrSpan — один и тот же массив из 16 чисел. Первый натив взял его
    // std::vector (аллокация и копия на КАЖДЫЙ вызов), второй — std::span прямо в буфер
    // движка. Вдвое, и это цена одной пары malloc/free.
    //
    // lookupRT/lookupCT — один и тот же вызов движкового метода, отличается только тем,
    // где известно имя. В рантайме это обход script-контекстов, хеш имени в движке и
    // VirtualQuery на каждый вызов: 21 «пустой вызов», то есть около 700 нс. С именем в
    // типе дескриптор ищется один раз за процесс, и остаётся 0.96 — дешевле, чем сам
    // ванильный натив. Разница в 22 раза, и это самая крупная находка за всю оптимизацию.
    //
    // ПОТОЛКИ СТОЯТ ВДВОЕ ВЫШЕ ИЗМЕРЕННОГО, И ЭТО НЕ ЛЕНЬ. Первая калибровка была +25%
    // — и замигала: на занятой машине два прогона из четырёх ушли за порог, хотя код не
    // менялся. Кейс сторожит ГРУБУЮ регрессию — вернувшийся на горячий путь VirtualQuery
    // (3-5x), потерянную мемоизацию (2x и выше), сервис хоста, уехавший в трамплин.
    // Такие он ловит и с двойным запасом, а мигающий вентиль не стоит вообще ничего:
    // его первым делом отключают.
    static const float PERF_BRIDGE = 2.00;     // мост поверх ванильного натива
    static const float PERF_FIELD = 2.00;     // чтение поля по имени
    static const float PERF_OBJ = 2.00;       // объектный аргумент (deref_object)
    static const float PERF_TEXT = 4.00;      // возврат owned string
    static const float PERF_MARSHAL = 2.60;   // маршалируемый вызов шаблона
    static const float PERF_LOOKUP = 2.60;    // вызов движкового метода по имени из типа
    static const float PERF_TEXT_LONG = 9.00; // возврат строки, не влезающей в SSO
    static const float PERF_SUGAR = 1.35;     // насколько прокси может быть дороже явной формы
    static const float PERF_GUARDED = 2.50;   // натив, защищённый __try/__except
    static const float PERF_MISSING = 2.50;   // вызов того, чего в этой сборке нет
    static const float PERF_PROTO_OUT = 9.00; // вызов движкового `proto` В ДРУГУЮ СТОРОНУ

    // ── Менеджер модулей ──────────────────────────────────────────────────────
    // Нативы этой сьюты приезжают из ДВУХ РАЗНЫХ плагинов: всё, кроме SeraphHashMap, —
    // из SIXW_GRAFT, хеш-таблица — из SIXW_HASHMAP. Поэтому «два плагина уживаются»
    // проверяется обычным прогоном сьюты, а не отдельным ритуалом, который забудут
    // запустить: сломается сосуществование — покраснеет половина кейсов.

    [TEST_CASE("Manager_HostVersionsMatchTheBuild").IN(SERAPH_GRAFT_TEST)];
    void Manager_HostVersionsMatchTheBuild()
    {
        assert(GraftVersion() == 5, "5", GraftVersion().ToString(), "версия интерфейса хоста");
        assert(GraftLayoutVersion() == 2, "2", GraftLayoutVersion().ToString(),
            "версия раскладки движка");
    }

    // ── Журналы ───────────────────────────────────────────────────────────────
    // Пользовательский канал уходит в журналы САМОЙ ИГРЫ: Print в script-лог, Error2 в
    // crash-лог. Зовутся они по ИМПЛУ, найденному по имени на регистрации, — через
    // модуль скриптов до них не добраться (GameScript это 3_Game, а они в 1_Core).

    bool AnyFileLike(string mask)
    {
        string name;
        FileAttr attr;
        FindFileHandle handle = FindFile("$profile:" + mask, name, attr, 0);
        bool found = name != "";
        CloseFindFile(handle);
        return found;
    }

    [TEST_CASE("Log_EngineGlobalsAreFoundByName").IN(SERAPH_GRAFT_TEST)];
    void Log_EngineGlobalsAreFoundByName()
    {
        int found = SeraphGraftSayVia("Print", "[сьюта] глобаль движка позвана вслепую");
        assert(found == 1, "1", found.ToString(), "Print нашёлся по имени и позвался");
        int missing = SeraphGraftSayVia("НетТакойГлобали", "неважно");
        assert(missing == -1, "-1", missing.ToString(), "чего нет — того нет, без падения");
    }

    [TEST_CASE("Log_UserChannelGoesToTheGameLogs").IN(SERAPH_GRAFT_TEST)];
    void Log_UserChannelGoesToTheGameLogs()
    {
        bool said = SeraphGraftSay("строка из C++ в script-лог");
        assert(said, "true", said.ToString(), "Print игры дотянулся из C++");
        bool cried = SeraphGraftCry("строка из C++ в crash-лог");
        assert(cried, "true", cried.ToString(), "Error2 игры дотянулся из C++");
        // crash-лог в профиле появился. Он мог появиться и без нас (движок пишет туда
        // свои ошибки), поэтому кейс сторожит не «наша строка там одна», а то, что
        // запись из C++ прошла и файл на месте.
        assert(AnyFileLike("crash_*.log"), "true", AnyFileLike("crash_*.log").ToString(),
            "crash-лог в профиле есть");
    }

    [TEST_CASE("Manager_BothPluginsLoaded").IN(SERAPH_GRAFT_TEST)];
    void Manager_BothPluginsLoaded()
    {
        assert(IsGrafted("SIXW_GRAFT"), "true", IsGrafted("SIXW_GRAFT").ToString(),
            "плагин с основными нативами загружен");
        assert(IsGrafted("SIXW_HASHMAP"), "true",
            IsGrafted("SIXW_HASHMAP").ToString(), "плагин с хеш-таблицей загружен");
    }

    // Хост считает себя модулем номер ноль: его нативы идут через то же слияние.
    [TEST_CASE("Manager_HostCountsItself").IN(SERAPH_GRAFT_TEST)];
    void Manager_HostCountsItself()
    {
        assert(IsGrafted("graft"), "true", IsGrafted("graft").ToString(),
            "сам хост в списке модулей");
        assert(GraftPluginCount() >= 3, ">= 3", GraftPluginCount().ToString(),
            "хост + два плагина");
    }

    [TEST_CASE("Manager_UnknownPluginReportsMinusOne").IN(SERAPH_GRAFT_TEST)];
    void Manager_UnknownPluginReportsMinusOne()
    {
        assert(IsGrafted("NOPE") == false, "false", IsGrafted("NOPE").ToString(),
            "чего нет — того нет");
        // Ноль — законная версия, поэтому «нет такого» отвечается минус единицей.
        assert(GraftPluginVersion("NOPE") == -1, "-1", GraftPluginVersion("NOPE").ToString(),
            "отсутствие отличается от версии 0");
        assert(GraftPluginVersion("SIXW_GRAFT") == 1, "1",
            GraftPluginVersion("SIXW_GRAFT").ToString(), "версия загруженного плагина");
    }

    // Коллизия имён — не абстракция: два плагина в одной установке легко занимают одно
    // имя. Здоровая установка обязана иметь ноль.
    [TEST_CASE("Manager_NoNameCollisions").IN(SERAPH_GRAFT_TEST)];
    void Manager_NoNameCollisions()
    {
        string first = "";
        if (GraftCollisionCount() > 0)
            first = GraftCollisionAt(0);
        assert(GraftCollisionCount() == 0, "0", GraftCollisionCount().ToString() + " " + first,
            "ни одно имя не отклонено");
    }

    // Оба плагина работают ОДНОВРЕМЕННО и в одном выражении: слева натив одного,
    // справа — другого.
    [TEST_CASE("Manager_TwoPluginsInOneExpression").IN(SERAPH_GRAFT_TEST)];
    void Manager_TwoPluginsInOneExpression()
    {
        SeraphHashMap<string, int> table = new SeraphHashMap<string, int>;   // SIXW_HASHMAP
        table.Set("abc", SeraphGraftStrLen("abcde"));                  // SIXW_GRAFT
        int got = table.Get("abc") + SeraphGraftPing(0);
        assert(got == 5 + 0x0005E1AF, (5 + 0x0005E1AF).ToString(), got.ToString(),
            "нативы двух плагинов в одном выражении");
    }

    // ── Сколько на самом деле должна жить возвращённая строка ────────────────
    // Эти два кейса — не проверка кода, а ЗАФИКСИРОВАННЫЙ ФАКТ О ДВИЖКЕ, добытый
    // экспериментом: натив затирает всё, что держит арена, а скрипт смотрит, уцелел ли
    // его текст. Ответ оказался таким:
    //
    //   движок НЕ копирует возвращённый const char* в момент возврата — он держит наш
    //   указатель на стеке выражения и копирует ТОЛЬКО когда значение потребляется
    //   (присваивание, вставка в контейнер, передача дальше).
    //
    // Отсюда и вся конструкция с ареной: освободить строку в конце вызова нельзя,
    // потому что внутри ОДНОГО оператора скрипта нативов может быть несколько, и первый
    // отдал строку, которую заберут только после последнего.

    // Присваивание уже случилось — с этого момента строка принадлежит движку.
    [TEST_CASE("Arena_LifetimeAfterAssignment").IN(SERAPH_GRAFT_TEST)];
    void Arena_LifetimeAfterAssignment()
    {
        string got = SeraphGraftEcho("one");
        int wiped = SeraphGraftPoisonArena();
        assert(got == "echo:one", "echo:one", got + " (затёрто " + wiped.ToString() + " байт)",
            "движок скопировал строку себе — арене хранить её незачем");
    }

    // Затирание МЕЖДУ возвратом и потреблением. Результат зависит от того, ЧЬЯ это
    // память, и в этом весь смысл перехода на движковый аллокатор:
    //   * своё кольцо  — строка гибнет: движок держал наш указатель, мы его затёрли;
    //   * движковый блок — строке ничего не делается, затирать нечего, память не наша.
    [TEST_CASE("Arena_PoisonMattersOnlyForOurOwnMemory").IN(SERAPH_GRAFT_TEST)];
    void Arena_PoisonMattersOnlyForOurOwnMemory()
    {
        map<string, int> m = new map<string, int>;
        m.Insert(SeraphGraftEcho("key"), SeraphGraftPoisonAndPass(7));
        string key = "";
        if (m.Count() > 0)
            key = m.GetKey(0);
        if (SeraphGraftEngineStrings())
        {
            assert(key == "echo:key", "echo:key", key,
                "строку выдал движок — своей памятью её не испортить");
        }
        else
        {
            assert(key != "echo:key", "не echo:key (движок хранит наш указатель)", key,
                "своё кольцо: внутри оператора освобождать возвращённую строку нельзя");
        }
    }

    // ── Граница применимости кольца ──────────────────────────────────────────
    // Строка живёт, пока после неё не выдадут kRing байт новых. Если оператор скрипта
    // умудрится выдать больше — что требует сотен нативов со строковым возвратом в
    // одном выражении — движок скопирует НЕ ТОТ текст. Эти два кейса фиксируют, что
    // отказ именно такой: ограниченный и без падения.

    // Кольцо прокручено между возвратом строки и её потреблением: текст обязан
    // испортиться, но процесс — выжить, а строка — остаться конечной.
    [TEST_CASE("Arena_OverrunGivesWrongText").IN(SERAPH_GRAFT_TEST)];
    void Arena_OverrunGivesWrongText()
    {
        // Кольца может не быть вовсе: строки выдаёт движок и переполнять нечего.
        if (SeraphGraftEngineStrings())
        {
            assert(true, "-", "-", "строки выдаёт движок — окна нет, переполнять нечего");
            return;
        }
        map<string, int> m = new map<string, int>;
        m.Insert(SeraphGraftEcho("survivor"), SeraphGraftChurnAndPass(40000, 1));
        string key = "";
        if (m.Count() > 0)
            key = m.GetKey(0);
        // Главное здесь — не значение, а то, что мы сюда дошли и строка конечна.
        assert(key.Length() < 4096, "< 4096", key.Length().ToString(),
            "переполнение окна даёт неверный текст, но чтение не уходит за блок");
    }

    // А до потребления прокрутка безвредна: движок уже скопировал строку себе.
    [TEST_CASE("Arena_ChurnAfterAssignmentIsHarmless").IN(SERAPH_GRAFT_TEST)];
    void Arena_ChurnAfterAssignmentIsHarmless()
    {
        string got = SeraphGraftEcho("survivor");
        SeraphGraftChurnAndPass(40000, 1);
        assert(got == "echo:survivor", "echo:survivor", got,
            "после присваивания строка принадлежит движку и прокрутке не подвластна");
    }

    // Строку, которую скрипт не потребил, обязан освободить тот, кто ей владеет. Пока
    // память была наша, вопрос не стоял: кольцо переиспользовалось само. Теперь блок
    // движковый, и если движок его не снимает — это утечка, видимая по памяти процесса.
    [TEST_CASE("Arena_DiscardedStringsDoNotLeak").IN(SERAPH_GRAFT_TEST)];
    void Arena_DiscardedStringsDoNotLeak()
    {
        int i;
        string longArg = "0123456789012345678901234567890123456789012345678901234567890123";
        // Прогрев: первые блоки движок ещё выделяет у системы, дальше идёт его фрилист.
        for (i = 0; i < 20000; i++)
            SeraphGraftEcho(longArg);
        int before = SeraphGraftPrivateKB();
        for (i = 0; i < 200000; i++)
            SeraphGraftEcho(longArg);
        int after = SeraphGraftPrivateKB();
        int grew = after - before;
        // 200000 строк по 69 байт — это 13 МБ, если бы текла каждая.
        assert(grew < 2048, "< 2048 КБ", grew.ToString() + " КБ на 200000 строк",
            "неиспользованные строки не накапливаются");
    }

    // Запас кольца не подобран, а ИЗМЕРЕН: сколько байт максимум понадобилось между
    // двумя внешними вызовами против размера кольца. Замер на этой сьюте — 89 байт при
    // кольце 16384, то есть запас в 184 раза. Если однажды сблизятся, счётчик скажет
    // об этом раньше, чем строку успеет затереть.
    [TEST_CASE("Arena_RingHasRoomToSpare").IN(SERAPH_GRAFT_TEST)];
    void Arena_RingHasRoomToSpare()
    {
        if (SeraphGraftEngineStrings())
        {
            assert(true, "-", "-", "строки выдаёт движок — своего окна нет");
            return;
        }
        int i;
        string s;
        for (i = 0; i < 100; i++)
            s = SeraphGraftEcho("строка подлиннее для честного замера расхода");
        int peak = SeraphGraftTextPeak();
        int ring = SeraphGraftTextRing();
        // ВАЖНО про эту метрику: она меряет расход за ОДНУ ЦЕПОЧКУ вызовов, а не за
        // оператор скрипта — в m.Insert(A(), B()) цепочек две. Расход на оператор
        // измерить нечем, поэтому число говорит о порядке величины одной строки, а не
        // доказывает запас. Настоящий аргумент запаса — в комментарии к text.cpp:
        // 16 КБ это порядка трёхсот строк, а оператор столько нативов не вызывает.
        assert(peak * 8 < ring, "peak * 8 < ring", "peak=" + peak.ToString() + " ring=" + ring.ToString(),
            "одна строка много меньше кольца");
    }

    [TEST_CASE("Int_PingMatchesXorFormula").IN(SERAPH_GRAFT_TEST)];
    void Int_PingMatchesXorFormula()
    {
        int token = 1234;
        int reply = SeraphGraftPing(token);
        int expected = token ^ 0x0005E1AF;
        assert(reply == expected, expected.ToString(), reply.ToString(),
            "int туда и обратно: reply == token ^ magic");
    }

    [TEST_CASE("Int_PingZero").IN(SERAPH_GRAFT_TEST)];
    void Int_PingZero()
    {
        int reply = SeraphGraftPing(0);
        assert(reply == 0x0005E1AF, "385455", reply.ToString(), "ping(0) == magic");
    }

    [TEST_CASE("Float_MixedWithInt").IN(SERAPH_GRAFT_TEST)];
    void Float_MixedWithInt()
    {
        float got = SeraphGraftMix(2.5, 4.0, 3);   // 2.5*4 + 3
        assert(Math.AbsFloat(got - 13.0) < 0.001, "13", got.ToString(),
            "float в xmm, int в целочисленном регистре — вперемешку");
    }

    [TEST_CASE("String_LengthArrivesInCpp").IN(SERAPH_GRAFT_TEST)];
    void String_LengthArrivesInCpp()
    {
        int got = SeraphGraftStrLen("hello");
        int empty = SeraphGraftStrLen("");
        bool ok = false;
        if (got == 5)
        {
            if (empty == 0)
                ok = true;
        }
        assert(ok, "5 0", got.ToString() + " " + empty.ToString(),
            "string-аргумент доходит как char*");
    }

    [TEST_CASE("Bool_TwoStringArgs").IN(SERAPH_GRAFT_TEST)];
    void Bool_TwoStringArgs()
    {
        bool yes = SeraphGraftHasPrefix("hello world", "hello");
        bool no = SeraphGraftHasPrefix("hello world", "bye");
        bool ok = false;
        if (yes)
        {
            if (!no)
                ok = true;
        }
        assert(ok, "true false", yes.ToString() + " " + no.ToString(),
            "два string-аргумента + bool-возврат");
    }

    [TEST_CASE("Vector_InAndOut").IN(SERAPH_GRAFT_TEST)];
    void Vector_InAndOut()
    {
        vector got = SeraphGraftVecScale(Vector(1, 2, 3), 2.0);
        bool ok = false;
        if (Math.AbsFloat(got[0] - 2.0) < 0.001)
        {
            if (Math.AbsFloat(got[1] - 4.0) < 0.001)
            {
                if (Math.AbsFloat(got[2] - 6.0) < 0.001)
                    ok = true;
            }
        }
        assert(ok, "<2,4,6>", got.ToString(), "vector и аргументом, и возвратом");
    }

    [TEST_CASE("String_OwnedReturn").IN(SERAPH_GRAFT_TEST)];
    void String_OwnedReturn()
    {
        string got = SeraphGraftEcho("abc");
        assert(got == "echo:abc", "echo:abc", got, "owned string — возврат строки из C++");
    }

    [TEST_CASE("Method_StaticOnScriptClass").IN(SERAPH_GRAFT_TEST)];
    void Method_StaticOnScriptClass()
    {
        int got = SeraphGraft.Magic();
        assert(got == 0x0005E1AF, "385455", got.ToString(),
            "натив, привязанный методом класса (RegisterMethod)");
    }

    [TEST_CASE("Entity_MethodsFromCpp").IN(SERAPH_GRAFT_TEST)];
    void Entity_MethodsFromCpp()
    {
        Object obj = GetGame().CreateObjectEx("Apple", "1000 5 1000", ECE_NONE);
        bool ok = false;
        string info = "obj=null";
        if (obj)
        {
            // C++ зовёт движковый IEntity.GetOrigin на игровом объекте
            vector fromCpp = SeraphGraftEntityOrigin(obj);
            vector fromScript = obj.GetPosition();
            bool has = SeraphGraftEntityHas(obj, "GetOrigin");
            // owned string от движкового GetName: C++ должен увидеть ровно то же,
            // что и скрипт (у Apple это пусто, но сходиться обязано)
            string nameCpp = SeraphGraftEntityName(obj);
            string nameScr = obj.GetName();
            info = "cpp=" + fromCpp.ToString() + " scr=" + fromScript.ToString() + " has=" + has.ToString() + " name=" + nameCpp + "/" + nameScr;
            if (vector.Distance(fromCpp, fromScript) < 0.01)
            {
                if (has)
                {
                    if (nameCpp == nameScr)
                        ok = true;
                }
            }
        }
        assert(ok, "позиция C++ == позиция скрипта", info,
            "движковые методы игрового объекта вызываются из C++");
        if (obj)
            GetGame().ObjectDelete(obj);
    }

    [TEST_CASE("Entity_MovedFromCpp").IN(SERAPH_GRAFT_TEST)];
    void Entity_MovedFromCpp()
    {
        Object obj = GetGame().CreateObjectEx("Apple", "1000 5 1000", ECE_NONE);
        bool ok = false;
        if (obj)
        {
            vector moved = SeraphGraftEntityMove(obj, "1200 7 1300");
            vector now = obj.GetPosition();
            if (vector.Distance(moved, now) < 0.01)
            {
                if (Math.AbsFloat(now[0] - 1200) < 0.5)
                    ok = true;
            }
        }
        assert(ok, "объект переставлен в 1200 7 1300", "см. код",
            "C++ меняет состояние игрового объекта движковым SetOrigin");
        if (obj)
            GetGame().ObjectDelete(obj);
    }

    [TEST_CASE("Modern_RangesOverScriptArray").IN(SERAPH_GRAFT_TEST)];
    void Modern_RangesOverScriptArray()
    {
        array<int> values = new array<int>;
        values.Insert(4);
        values.Insert(8);
        values.Insert(15);
        values.Insert(16);
        int above = SeraphGraftCountAbove(values, 7);        // 8,15,16
        int evenDoubled = SeraphGraftSumEvenDoubled(values); // (4+8+16)*2
        bool ok = false;
        if (above == 3)
        {
            if (evenDoubled == 56)
                ok = true;
        }
        assert(ok, "3 / 56", above.ToString() + " / " + evenDoubled.ToString(),
            "std::ranges и views работают прямо по скриптовому массиву");
    }

    [TEST_CASE("Modern_TryCallReportsMiss").IN(SERAPH_GRAFT_TEST)];
    void Modern_TryCallReportsMiss()
    {
        Object obj = GetGame().CreateObjectEx("Apple", "1000 5 1000", ECE_NONE);
        string missing = SeraphGraftProbeCall(obj, "NoSuchMethodZZZ");
        string scripted = SeraphGraftProbeCall(obj, "GetType");   // скриптовый метод, не натив
        string good = SeraphGraftProbeCall(obj, "GetID");
        bool ok = false;
        if (missing == "not-found")
        {
            if (scripted == "not-native")
            {
                if (good.IndexOf("ok:") == 0)
                    ok = true;
            }
        }
        assert(ok, "not-found / not-native / ok:*",
            missing + " / " + scripted + " / " + good,
            "промах виден в типе и не роняет сервер");
        if (obj)
            GetGame().ObjectDelete(obj);
    }

    [TEST_CASE("Modern_TryFieldReportsMiss").IN(SERAPH_GRAFT_TEST)];
    void Modern_TryFieldReportsMiss()
    {
        SeraphNode node = new SeraphNode(77, 0, "n");
        string found = node.TryField("m_id");
        string missing = node.TryField("m_nope");
        bool ok = false;
        if (found == "77")
        {
            if (missing == "нет поля")
                ok = true;
        }
        assert(ok, "77 / нет поля", found + " / " + missing,
            "отсутствующее поле — пустой optional, а не тихий ноль");
    }

    [TEST_CASE("Modern_TextFormatting").IN(SERAPH_GRAFT_TEST)];
    void Modern_TextFormatting()
    {
        // возврат строки собран через std::format, без статических буферов
        string first = SeraphGraftEcho("abc");
        string second = SeraphGraftEcho("xyz");
        bool ok = false;
        if (first == "echo:abc")
        {
            if (second == "echo:xyz")
                ok = true;
        }
        assert(ok, "echo:abc / echo:xyz", first + " / " + second,
            "graft::text: форматирование и несколько живых возвратов подряд");
    }

    [TEST_CASE("Diag_MethodFlags").IN(SERAPH_GRAFT_TEST)];
    void Diag_MethodFlags()
    {
        // ванильные: член класса, статический, метод шаблонного класса
        SeraphGraftMethodFlags("string", "Length");
        SeraphGraftMethodFlags("string", "Format");
        SeraphGraftMethodFlags("Math", "Sqrt");
        SeraphGraftMethodFlags("Math", "AbsInt");
        SeraphGraftMethodFlags("map", "Count");
        SeraphGraftMethodFlags("array", "Resize");
        SeraphGraftMethodFlags("Class", "ToString");
        // наши
        SeraphGraftMethodFlags("SeraphGraft", "Magic");
        SeraphGraftMethodFlags("SeraphGraft", "SelfTag");
        SeraphGraftMethodFlags("SeraphBox", "Tag");
        SeraphGraftMethodFlags("SeraphHashMap", "Set");

        assert(true, "смотри graft.log", "смотри graft.log", "снимок флагов дескрипторов");
    }

    [TEST_CASE("HashMap_StringToInt").IN(SERAPH_GRAFT_TEST)];
    void HashMap_StringToInt()
    {
        // шаблон скрипта, хранилище — std::unordered_map в C++
        SeraphHashMap<string, int> m = new SeraphHashMap<string, int>;
        m.Set("alpha", 1);
        m.Set("beta", 22);
        m.Set("gamma", 333);

        bool ok = false;
        if (m.Count() == 3)
        {
            if (m.Get("gamma") == 333)
            {
                if (m.Contains("beta"))
                    ok = true;
            }
        }
        assert(ok, "3 / 333 / true",
            m.Count().ToString() + " / " + m.Get("gamma").ToString() + " / " + m.Contains("beta").ToString(),
            "шаблонная хеш-таблица поверх std::unordered_map");
    }

    [TEST_CASE("HashMap_RemoveAndClear").IN(SERAPH_GRAFT_TEST)];
    void HashMap_RemoveAndClear()
    {
        SeraphHashMap<string, int> m = new SeraphHashMap<string, int>;
        m.Set("a", 1);
        m.Set("b", 2);
        bool removed = m.Remove("a");
        int afterRemove = m.Count();
        m.Clear();
        int afterClear = m.Count();
        bool ok = false;
        if (removed)
        {
            if (afterRemove == 1)
            {
                if (afterClear == 0)
                    ok = true;
            }
        }
        assert(ok, "true / 1 / 0",
            removed.ToString() + " / " + afterRemove.ToString() + " / " + afterClear.ToString(),
            "удаление и очистка доходят до C++");
    }

    [TEST_CASE("HashMap_KeysAndIsolation").IN(SERAPH_GRAFT_TEST)];
    void HashMap_KeysAndIsolation()
    {
        SeraphHashMap<string, int> first = new SeraphHashMap<string, int>;
        SeraphHashMap<string, int> second = new SeraphHashMap<string, int>;
        first.Set("one", 10);
        first.Set("two", 20);
        second.Set("one", 999);

        // перебор ключей: порядок у хеш-таблицы не определён, поэтому проверяем состав
        string keys = "";
        for (int i = 0; i < first.Count(); i++)
            keys = keys + first.KeyAt(i);

        bool ok = false;
        if (keys.Length() == 6)
        {
            if (keys.Contains("one"))
            {
                if (keys.Contains("two"))
                {
                    if (first.Get("one") == 10)
                    {
                        if (second.Get("one") == 999)
                            ok = true;
                    }
                }
            }
        }
        assert(ok, "one+two / 10 / 999",
            keys + " / " + first.Get("one").ToString() + " / " + second.Get("one").ToString(),
            "перебор ключей и независимость таблиц у разных объектов");
    }

    [TEST_CASE("HashMap_OtherInstantiations").IN(SERAPH_GRAFT_TEST)];
    void HashMap_OtherInstantiations()
    {
        // те же нативы обслуживают любую инстанциацию шаблона
        SeraphHashMap<int, string> byId = new SeraphHashMap<int, string>;
        byId.Set(7, "seven");
        SeraphHashMap<string, float> byName = new SeraphHashMap<string, float>;
        byName.Set("pi", 3.14);

        bool ok = false;
        if (byId.Get(7) == "seven")
        {
            if (Math.AbsFloat(byName.Get("pi") - 3.14) < 0.001)
                ok = true;
        }
        assert(ok, "seven / 3.14",
            byId.Get(7) + " / " + byName.Get("pi").ToString(),
            "один нативный слой обслуживает разные K и V");
    }

    [TEST_CASE("Primitive_OwnEnumType").IN(SERAPH_GRAFT_TEST)];
    void Primitive_OwnEnumType()
    {
        // свой примитив C++ (enum Team) скрипт видит как обычный int
        int opposite = SeraphGraftOpposite(1);   // red -> blue
        array<int> teams = new array<int>;
        teams.Insert(1);
        teams.Insert(2);
        teams.Insert(1);
        int reds = SeraphGraftCountReds(teams);
        bool ok = false;
        if (opposite == 2)
        {
            if (reds == 2)
                ok = true;
        }
        assert(ok, "2 / 2", opposite.ToString() + " / " + reds.ToString(),
            "свой примитив: аргумент, возврат и элемент массива");
    }

    [TEST_CASE("Fields_ScalarsAndString").IN(SERAPH_GRAFT_TEST)];
    void Fields_ScalarsAndString()
    {
        SeraphNode node = new SeraphNode(42, 2.5, "root");
        int id = node.Id();
        float w = node.Weight();
        string label = node.Label();
        bool ok = false;
        if (id == 42)
        {
            if (Math.AbsFloat(w - 2.5) < 0.001)
            {
                if (label == "root")
                    ok = true;
            }
        }
        assert(ok, "42 / 2.5 / root", id.ToString() + " / " + w.ToString() + " / " + label,
            "C++ читает поля объекта по имени");
    }

    [TEST_CASE("Fields_WriteBack").IN(SERAPH_GRAFT_TEST)];
    void Fields_WriteBack()
    {
        SeraphNode node = new SeraphNode(1, 0, "x");
        int got = node.SetId(777);
        bool ok = false;
        if (got == 777)
        {
            if (node.m_id == 777)   // скрипт видит то, что записал C++
                ok = true;
        }
        assert(ok, "777 / 777", got.ToString() + " / " + node.m_id.ToString(),
            "C++ пишет в поле объекта, скрипт видит изменение");
    }

    [TEST_CASE("Fields_NestedChain").IN(SERAPH_GRAFT_TEST)];
    void Fields_NestedChain()
    {
        SeraphNode a = new SeraphNode(1, 0, "a");
        SeraphNode b = new SeraphNode(20, 0, "b");
        SeraphNode c = new SeraphNode(300, 0, "c");
        a.m_child = b;
        b.m_child = c;
        int sum = a.ChainSum();
        assert(sum == 321, "321", sum.ToString(),
            "C++ спускается по вложенным объектам произвольной глубины");
    }

    [TEST_CASE("Fields_NestedContainer").IN(SERAPH_GRAFT_TEST)];
    void Fields_NestedContainer()
    {
        SeraphNode node = new SeraphNode(1, 0, "n");
        node.m_values.Insert(5);
        node.m_values.Insert(50);
        node.m_values.Insert(500);
        int sum = node.ValuesSum();
        assert(sum == 555, "555", sum.ToString(),
            "массив внутри объекта читается той же вьюхой");
    }

    [TEST_CASE("Fields_Reflection").IN(SERAPH_GRAFT_TEST)];
    void Fields_Reflection()
    {
        SeraphNode node = new SeraphNode(1, 0, "n");
        int count = node.FieldCount();
        string first = node.FieldName(0);
        bool ok = false;
        if (count == 5)
        {
            if (first == "m_id")
                ok = true;
        }
        assert(ok, "5 / m_id", count.ToString() + " / " + first,
            "C++ обходит незнакомую структуру: имена и число полей");
    }

    [TEST_CASE("Method_MemberLikeVanilla").IN(SERAPH_GRAFT_TEST)];
    void Method_MemberLikeVanilla()
    {
        // обычный member-метод: вызывается через объект, без явного self
        SeraphGraft inst = new SeraphGraft;
        int got = inst.SelfTag();
        assert(got == 0x0005E1B0, "385456", got.ToString(),
            "нестатичный метод объявлен как у ванильных коллекций");
    }

    [TEST_CASE("Template_OwnProtoClass").IN(SERAPH_GRAFT_TEST)];
    void Template_OwnProtoClass()
    {
        // свой шаблонный тип: импл его proto-методов лежит в C++
        SeraphBox<int> a = new SeraphBox<int>;
        SeraphBox<string> b = new SeraphBox<string>;
        bool ok = false;
        if (a.Tag() == 0x0005E1AF)
        {
            if (b.Tag() == 0x0005E1AF)
                ok = true;
        }
        assert(ok, "385455/385455", a.Tag().ToString() + "/" + b.Tag().ToString(),
            "натив на своём шаблонном классе работает для любого T");
    }

    [TEST_CASE("Template_PerInstanceState").IN(SERAPH_GRAFT_TEST)];
    void Template_PerInstanceState()
    {
        SeraphBox<int> a = new SeraphBox<int>;
        SeraphBox<int> b = new SeraphBox<int>;
        a.Bump(5);
        a.Bump(7);
        b.Bump(100);
        bool ok = false;
        if (a.Bump(0) == 12)
        {
            if (b.Bump(0) == 100)
                ok = true;
        }
        assert(ok, "12 / 100", a.Bump(0).ToString() + " / " + b.Bump(0).ToString(),
            "this доезжает до C++ и различает экземпляры шаблона");
    }

    [TEST_CASE("Nested_ArrayOfGameObjects").IN(SERAPH_GRAFT_TEST)];
    void Nested_ArrayOfGameObjects()
    {
        array<Object> objects = new array<Object>;
        objects.Insert(GetGame().CreateObjectEx("Apple", "1000 10 1000", ECE_NONE));
        objects.Insert(GetGame().CreateObjectEx("Apple", "1010 20 1000", ECE_NONE));
        objects.Insert(GetGame().CreateObjectEx("Apple", "1020 30 1000", ECE_NONE));

        // контейнер -> игровой объект -> движковый метод
        int sum = SeraphGraftSumHeights(objects);
        int moved = SeraphGraftRaiseAll(objects, 5.0);
        int sumAfter = SeraphGraftSumHeights(objects);

        bool ok = false;
        if (sum == 60)
        {
            if (moved == 3)
            {
                if (sumAfter == 75)
                    ok = true;
            }
        }
        assert(ok, "60 / 3 / 75",
            sum.ToString() + " / " + moved.ToString() + " / " + sumAfter.ToString(),
            "вложенность: массив игровых объектов обходится и меняется из C++");

        for (int i = 0; i < objects.Count(); i++)
        {
            if (objects.Get(i))
                GetGame().ObjectDelete(objects.Get(i));
        }
    }

    [TEST_CASE("Array_IntsRead").IN(SERAPH_GRAFT_TEST)];
    void Array_IntsRead()
    {
        array<int> a = new array<int>;
        a.Insert(1);
        a.Insert(2);
        a.Insert(39);
        int got = SeraphGraftSumArray(a);
        assert(got == 42, "42", got.ToString(), "C++ читает array<int> скрипта");
    }

    [TEST_CASE("Array_StringsRead").IN(SERAPH_GRAFT_TEST)];
    void Array_StringsRead()
    {
        array<string> a = new array<string>;
        a.Insert("ab");
        a.Insert("cde");
        int got = SeraphGraftStrArrayLen(a);
        assert(got == 5, "5", got.ToString(), "элементы array<string> приходят как char*");
    }

    [TEST_CASE("Array_WriteBack").IN(SERAPH_GRAFT_TEST)];
    void Array_WriteBack()
    {
        array<int> a = new array<int>;
        a.Insert(1);
        a.Insert(2);
        a.Insert(3);
        int n = SeraphGraftDoubleArray(a);
        bool ok = false;
        if (n == 3)
        {
            if (a.Get(0) == 2)
            {
                if (a.Get(2) == 6)
                    ok = true;
            }
        }
        assert(ok, "3 / 2 / 6", n.ToString() + " / " + a.Get(0).ToString() + " / " + a.Get(2).ToString(),
            "C++ пишет в существующие элементы массива");
    }

    [TEST_CASE("Array_SortAndRemove").IN(SERAPH_GRAFT_TEST)];
    void Array_SortAndRemove()
    {
        array<int> a = new array<int>;
        a.Insert(30);
        a.Insert(10);
        a.Insert(20);
        int first = SeraphGraftSortArray(a, false);   // движковый Sort
        int left = SeraphGraftRemoveAt(a, 0);         // движковый Remove
        bool ok = false;
        if (first == 10)
        {
            if (left == 2)
                ok = true;
        }
        assert(ok, "10 / 2", first.ToString() + " / " + left.ToString(),
            "Sort и Remove движка, вызванные из C++");
    }

    [TEST_CASE("Set_Read").IN(SERAPH_GRAFT_TEST)];
    void Set_Read()
    {
        set<int> s = new set<int>;
        s.Insert(10);
        s.Insert(20);
        int got = SeraphGraftSetSum(s);
        assert(got == 30, "30", got.ToString(), "set<T> устроен как array<T>");
    }

    [TEST_CASE("Typename_Name").IN(SERAPH_GRAFT_TEST)];
    void Typename_Name()
    {
        string got = SeraphGraftTypeName(SeraphGraft);
        assert(got == "SeraphGraft", "SeraphGraft", got, "typename несёт имя класса");
    }

    [TEST_CASE("Ref_ArbitraryClass").IN(SERAPH_GRAFT_TEST)];
    void Ref_ArbitraryClass()
    {
        SeraphGraft inst = new SeraphGraft;
        bool got = SeraphGraftRefAlive(inst);
        assert(got, "true", got.ToString(), "ссылка на произвольный скриптовый класс");
    }

    [TEST_CASE("Array_GrownFromCpp").IN(SERAPH_GRAFT_TEST)];
    void Array_GrownFromCpp()
    {
        array<int> a = new array<int>;         // пустой: память выделит C++ движковым Resize
        int n = SeraphGraftFillSquares(a, 4);
        bool ok = false;
        if (n == 4)
        {
            if (a.Count() == 4)
            {
                if (a.Get(3) == 9)
                    ok = true;
            }
        }
        assert(ok, "4 / 4 / 9", n.ToString() + " / " + a.Count().ToString() + " / " + a.Get(3).ToString(),
            "C++ сам растит массив движковым Resize");
    }

    [TEST_CASE("Array_GrowsBeyondCapacity").IN(SERAPH_GRAFT_TEST)];
    void Array_GrowsBeyondCapacity()
    {
        array<int> a = new array<int>;
        a.Insert(1);                            // ёмкость маленькая
        int n = SeraphGraftFillSquares(a, 100); // просим сильно больше — движок выделит
        bool ok = false;
        if (n == 100)
        {
            if (a.Get(99) == 9801)
                ok = true;
        }
        assert(ok, "100 / 9801", n.ToString() + " / " + a.Get(99).ToString(),
            "рост за пределы ёмкости работает: память выделяет движок");
    }

    [TEST_CASE("Array_ClearStrings").IN(SERAPH_GRAFT_TEST)];
    void Array_ClearStrings()
    {
        array<string> a = new array<string>;
        a.Insert("alpha");
        a.Insert("beta");
        int n = SeraphGraftClearArray(a);
        bool ok = false;
        if (n == 0)
        {
            if (a.Count() == 0)
                ok = true;
        }
        assert(ok, "0 / 0", n.ToString() + " / " + a.Count().ToString(),
            "Clear движка работает и для строк: за элементами следит он сам");
    }

    [TEST_CASE("Map_Size").IN(SERAPH_GRAFT_TEST)];
    void Map_Size()
    {
        map<string, int> m = new map<string, int>;
        m.Insert("a", 1);
        m.Insert("b", 2);
        m.Insert("c", 3);
        int got = SeraphGraftMapSize(m);
        assert(got == 3, "3", got.ToString(), "размер map читается из объекта");
    }

    [TEST_CASE("Table_ExportFromCpp").IN(SERAPH_GRAFT_TEST)];
    void Table_ExportFromCpp()
    {
        // Хеш-мапа C++ перекладывается в map скрипта: строки приходят возвратом
        // (owned string — движок копирует), числа можно и массивом.
        map<string, int> restored = new map<string, int>;
        int n = SeraphGraftTableCount();
        for (int i = 0; i < n; i++)
            restored.Insert(SeraphGraftTableKey(i), SeraphGraftTableValue(i));

        bool ok = false;
        if (n == 3)
        {
            if (restored.Count() == 3)
            {
                if (restored.Get("gamma") == 333)
                    ok = true;
            }
        }
        assert(ok, "3 / 3 / 333",
            n.ToString() + " / " + restored.Count().ToString() + " / " + restored.Get("gamma").ToString(),
            "хеш-мапа C++ доехала в map скрипта");
    }

    [TEST_CASE("Table_ExportValuesIntoOutArray").IN(SERAPH_GRAFT_TEST)];
    void Table_ExportValuesIntoOutArray()
    {
        array<int> values = new array<int>;   // пустой: размер поставит C++
        int n = SeraphGraftExportValues(values);
        int sum = 0;
        for (int i = 0; i < values.Count(); i++)
            sum += values.Get(i);
        bool ok = false;
        if (n == 3)
        {
            if (sum == 356)                   // 1 + 22 + 333
                ok = true;
        }
        assert(ok, "3 / 356", n.ToString() + " / " + sum.ToString(),
            "out array<int> заполняется прямо из C++");
    }

    [TEST_CASE("Table_ImportToCpp").IN(SERAPH_GRAFT_TEST)];
    void Table_ImportToCpp()
    {
        map<string, int> m = new map<string, int>;
        m.Insert("ab", 10);      // 10*2
        m.Insert("xyz", 100);    // 100*3
        array<string> keys = new array<string>;
        array<int> values = new array<int>;
        int n = m.Count();
        for (int i = 0; i < n; i++)
        {
            keys.Insert(m.GetKey(i));
            values.Insert(m.GetElement(i));
        }
        int got = SeraphGraftImportTable(keys, values);
        assert(got == 320, "320", got.ToString(), "map скрипта доехала в хеш-мапу C++");
    }

    [TEST_CASE("Map_ClearFromCpp").IN(SERAPH_GRAFT_TEST)];
    void Map_ClearFromCpp()
    {
        map<string, int> m = new map<string, int>;
        m.Insert("a", 1);
        m.Insert("b", 2);
        int left = SeraphGraftMapClear(m);
        bool ok = false;
        if (left == 0)
        {
            if (m.Count() == 0)
                ok = true;
        }
        assert(ok, "0 / 0", left.ToString() + " / " + m.Count().ToString(),
            "Clear() движка, вызванный из C++");
    }

    [TEST_CASE("Map_IterateIntKeys").IN(SERAPH_GRAFT_TEST)];
    void Map_IterateIntKeys()
    {
        map<int, int> m = new map<int, int>;
        m.Insert(2, 3);          // 6
        m.Insert(9, 10);         // 90 — коллизия слота с 1 при ёмкости 8
        m.Insert(1, 100);        // 100
        int got = SeraphGraftMapIntSum(m);
        assert(got == 196, "196", got.ToString(), "узлы с int-ключами: другой шаг узла");
    }

    [TEST_CASE("Map_IterateStringKeys").IN(SERAPH_GRAFT_TEST)];
    void Map_IterateStringKeys()
    {
        map<string, int> m = new map<string, int>;
        m.Insert("ab", 10);      // 10*2
        m.Insert("xyz", 100);    // 100*3
        m.Insert("q", 7);        // 7*1
        int got = SeraphGraftMapChecksum(m);
        assert(got == 327, "327", got.ToString(),
            "C++ обходит map<string,int> движковым итератором");
    }

    [TEST_CASE("Map_CopiedIntoSeraphHashMap").IN(SERAPH_GRAFT_TEST)];
    void Map_CopiedIntoSeraphHashMap()
    {
        map<string, int> m = new map<string, int>;
        m.Insert("ab", 10);
        m.Insert("xyz", 100);
        int got = SeraphGraftMapToTable(m);
        assert(got == 320, "320", got.ToString(),
            "map скрипта переложена в std::unordered_map одним вызовом");
    }

    [TEST_CASE("Map_IterateIntPairs").IN(SERAPH_GRAFT_TEST)];
    void Map_IterateIntPairs()
    {
        map<int, float> m = new map<int, float>;
        m.Insert(2, 1.5);        // 3.0
        m.Insert(9, 2.0);        // 18.0
        int got = SeraphGraftMapIntFloat(m);
        assert(got == 210, "210", got.ToString(),
            "узел из двух 4-байтных полей разных типов: шаг 16");
    }

    [TEST_CASE("Map_ContentsViaBridge").IN(SERAPH_GRAFT_TEST)];
    void Map_ContentsViaBridge()
    {
        map<string, int> m = new map<string, int>;
        m.Insert("ab", 10);     // 10*2
        m.Insert("xyz", 100);   // 100*3
        // мост: раскладываем карту в два массива, их C++ читает полностью
        array<string> keys = new array<string>;
        array<int> values = new array<int>;
        int n = m.Count();
        for (int i = 0; i < n; i++)
        {
            keys.Insert(m.GetKey(i));
            values.Insert(m.GetElement(i));
        }
        int got = SeraphGraftSumPairs(keys, values);
        assert(got == 320, "320", got.ToString(), "содержимое map доходит до C++ через массивы");
    }

    [TEST_CASE("HashMap_UnknownInstantiation").IN(SERAPH_GRAFT_TEST)];
    void HashMap_UnknownInstantiation()
    {
        // Ни одной из этих инстанциаций в C++ не перечислено: привязка одна, на общем
        // классе, а типы приезжают тегами в рантайме.
        SeraphHashMap<int, int> ii = new SeraphHashMap<int, int>;
        ii.Set(5, 50);
        SeraphHashMap<float, string> fs = new SeraphHashMap<float, string>;
        fs.Set(2.5, "два с половиной");
        SeraphHashMap<string, string> ss = new SeraphHashMap<string, string>;
        ss.Set("k", "v");

        bool ok = false;
        if (ii.Get(5) == 50)
        {
            if (fs.Get(2.5) == "два с половиной")
            {
                if (ss.Get("k") == "v")
                    ok = true;
            }
        }
        assert(ok, "50 / два с половиной / v",
            ii.Get(5).ToString() + " / " + fs.Get(2.5) + " / " + ss.Get("k"),
            "шаблон работает с типами, которых в C++ никто не перечислял");
    }

    // ── Полный спектр типов через маршалируемый путь ──────────────────────────
    [TEST_CASE("Any_EveryPrimitive").IN(SERAPH_GRAFT_TEST)];
    void Any_EveryPrimitive()
    {
        SeraphGraft a = new SeraphGraft;
        vector v = "1 2 3";
        string got = a.Describe(7);
        got = got + "|" + a.Describe(2.5);
        got = got + "|" + a.Describe("текст");
        got = got + "|" + a.Describe(true);
        got = got + "|" + a.Describe(v);
        string want = "int:7|float:2.5|string:текст|bool:1|vector:1 2 3";
        assert(got == want, want, got, "int/float/string/bool/vector приезжают со своим типом");
    }

    [TEST_CASE("Any_ObjectsAndTypename").IN(SERAPH_GRAFT_TEST)];
    void Any_ObjectsAndTypename()
    {
        SeraphGraft a = new SeraphGraft;
        SeraphNode node = new SeraphNode(1, 0, "n");
        array<int> arr = new array<int>;
        string got = a.Describe(node);
        got = got + "|" + a.Describe(arr);
        got = got + "|" + a.Describe(SeraphNode);
        string want = "object:SeraphNode|object:array<int>|typename:SeraphNode";
        assert(got == want, want, got,
            "объект, контейнер и typename различимы, у объекта видно настоящее имя типа");
    }

    [TEST_CASE("Any_OutArguments").IN(SERAPH_GRAFT_TEST)];
    void Any_OutArguments()
    {
        SeraphGraft a = new SeraphGraft;
        int bytes;
        int words;
        float ratio;
        a.Measure("привет мир друг", bytes, words, ratio);
        bool ok = false;
        if (bytes == 28)          // utf-8
        {
            if (words == 3)
            {
                if (ratio > 9.3)
                    ok = true;
            }
        }
        assert(ok, "28 / 3 / >9.3",
            bytes.ToString() + " / " + words.ToString() + " / " + ratio.ToString(),
            "out-аргументы возвращаются в скрипт");
    }

    [TEST_CASE("Any_NestedObjects").IN(SERAPH_GRAFT_TEST)];
    void Any_NestedObjects()
    {
        SeraphGraft a = new SeraphGraft;
        SeraphNode x = new SeraphNode(1, 0, "a");
        SeraphNode y = new SeraphNode(20, 0, "b");
        SeraphNode z = new SeraphNode(300, 0, "c");
        x.m_child = y;
        y.m_child = z;
        int sum = a.ChainOf(x);
        assert(sum == 321, "321", sum.ToString(),
            "объект приезжает значением, дальше спуск по вложенным полям");
    }

    [TEST_CASE("Any_VectorRoundTrip").IN(SERAPH_GRAFT_TEST)];
    void Any_VectorRoundTrip()
    {
        SeraphGraft a = new SeraphGraft;
        vector src = "1 2 3";
        vector v = a.Scale(src, 2.0);
        assert(v == "2 4 6", "2 4 6", v.ToString(), "vector туда и обратно через теги");
    }

    [TEST_CASE("Text_ManyReturnsInOneCall").IN(SERAPH_GRAFT_TEST)];
    void Text_ManyReturnsInOneCall()
    {
        // Прежнее кольцо было на 32 строки: сотня возвратов затирала ранние.
        SeraphHashMap<int, string> m = new SeraphHashMap<int, string>;
        for (int i = 0; i < 200; i++)
            m.Set(i, "значение-" + i.ToString());
        bool ok = false;
        if (m.Get(0) == "значение-0")
        {
            if (m.Get(199) == "значение-199")
                ok = true;
        }
        assert(ok, "значение-0 / значение-199", m.Get(0) + " / " + m.Get(199),
            "предела на число возвращаемых строк нет");
    }

    // ── Время жизни: когда C++ узнаёт о смерти скриптового объекта ────────────
    // Экземпляр класса C++ (с его полями) заводится при первом нативном вызове и живёт,
    // пока движок не разрушит объект. Скрипт про это ничего не знает: библиотека узнаёт о
    // смерти от самого движка. Проверяем оба пути.

    // ── Скорость: ванильный map против SeraphHashMap ─────────────────────────────
    // Замер секундомером SeraphClock: Exchange() отдаёт дельту с прошлой отметки,
    // поэтому все фазы меряются одним объектом.
    // ponytail: один прогон без прогрева — порядок величины виден, точных чисел
    // тут никто не обещает; для стабильных — гонять кейс несколько раз и усреднять.
    [TEST_CASE("Bench_MapVsSeraphHashMap").IN(SERAPH_GRAFT_TEST)];
    void Bench_MapVsSeraphHashMap()
    {
        int n = 10000;
        int i;

        // Ключи готовим заранее: конкатенация строк в замер попадать не должна.
        array<string> keys = new array<string>;
        for (i = 0; i < n; i++)
            keys.Insert("k" + i.ToString());

        map<string, int> vanilla = new map<string, int>;
        SeraphHashMap<string, int> cpp = new SeraphHashMap<string, int>;

        SeraphClock d = new SeraphClock();
        d.Exchange();                  // отметка на старте

        for (i = 0; i < n; i++)
            vanilla.Insert(keys.Get(i), i);
        float insertMap = d.Exchange();

        for (i = 0; i < n; i++)
            cpp.Set(keys.Get(i), i);
        float insertCpp = d.Exchange();

        int sumMap = 0;
        for (i = 0; i < n; i++)
            sumMap += vanilla.Get(keys.Get(i));
        float getMap = d.Exchange();

        int sumCpp = 0;
        for (i = 0; i < n; i++)
            sumCpp += cpp.Get(keys.Get(i));
        float getCpp = d.Exchange();

        // Count без аргументов и с int-возвратом — это ЦЕНА САМОГО ВЫЗОВА: разница между
        // строчками показывает, сколько стоит наш мост поверх ванильного натива.
        int c = 0;
        for (i = 0; i < n; i++)
            c += vanilla.Count();
        float countMap = d.Exchange();
        for (i = 0; i < n; i++)
            c += cpp.Count();
        float countCpp = d.Exchange();

        // Чтение поля по имени: Id() возвращает field<int>("m_id"), то есть перебор
        // таблицы переменных класса плюс вычисление адреса слота. Вычесть отсюда
        // countCpp — и останется цена самого поиска поля.
        SeraphNode node = new SeraphNode(7, 0, "n");
        d.Exchange();                  // создание объекта в замер не берём
        int f = 0;
        for (i = 0; i < n; i++)
            f += node.Id();
        float fieldRead = d.Exchange();

        // Объектный аргумент: тут работает deref_object, а в нём — VirtualQuery.
        SeraphGraft inst = new SeraphGraft();
        d.Exchange();
        int alive = 0;
        for (i = 0; i < n; i++)
            if (SeraphGraftRefAlive(inst)) alive++;
        float objArg = d.Exchange();

        // Возврат owned string: аренa строк плюс копия на стороне движка.
        string echoed;
        for (i = 0; i < n; i++)
            echoed = SeraphGraftEcho("abc");
        float strRet = d.Exchange();

        string head = "[BENCH] n=" + n.ToString() + "  ";
        Print(head + "insert: map " + SeraphClock.Format(insertMap) + " | SeraphHashMap " + SeraphClock.Format(insertCpp));
        Print(head + "get:    map " + SeraphClock.Format(getMap) + " | SeraphHashMap " + SeraphClock.Format(getCpp));
        Print(head + "count:  map " + SeraphClock.Format(countMap) + " | SeraphHashMap " + SeraphClock.Format(countCpp));
        Print(head + "field:  Id() " + SeraphClock.Format(fieldRead));
        Print(head + "obj:    RefAlive " + SeraphClock.Format(objArg));
        Print(head + "text:   Echo " + SeraphClock.Format(strRet));

        // Замер осмыслен только если обе таблицы отдали одно и то же.
        assert(sumMap == sumCpp, sumMap.ToString(), sumCpp.ToString(),
            "одинаковая сумма; время — строки [BENCH] в script-логе");
    }

    // ── Вентиль скорости ──────────────────────────────────────────────────────
    // Абсолютные миллисекунды гуляют между прогонами на ±30% (машина занята, частоты
    // плавают), а ОТНОШЕНИЕ к ванильному нативу — нет: шесть прогонов подряд дали
    // countCpp/countMap = 1.12…1.27. Поэтому меряем в единицах «ванильный map.Count()»:
    // столько стоит пустой вызов через скриптовую машину, и всё, что мы добавляем
    // сверху, видно как множитель.
    //
    // Кейс ловит ГРУБУЮ регрессию — лишний вызов через границу модуля, вернувшийся
    // VirtualQuery, потерянную мемоизацию. Две наносекунды он не поймает и не должен:
    // порог, поставленный впритык, начал бы мигать.
    // Один раунд замера: складывает в r[] СЫРЫЕ времена. Отношения считаются потом, из
    // минимумов — брать минимум уже от отношения нельзя: каждая метрика выбрала бы тот
    // раунд, где ей повезло с делителем, и оценка поехала бы вниз (видели 0.6 при
    // настоящих 1.0).
    void PerfRound(int n, out float r[])
    {
        int i;
        map<string, int> unitMap = new map<string, int>;
        SeraphHashMap<string, int> cpp = new SeraphHashMap<string, int>;
        cpp.Set("k", 1);
        SeraphNode node = new SeraphNode(7, 0, "n");
        SeraphGraft inst = new SeraphGraft();

        SeraphClock d = new SeraphClock();
        d.Exchange();

        // Единица измерения снимается ЗДЕСЬ ЖЕ, а не отдельной функцией: отношение
        // гасит дрейф машины только если обе величины сняты подряд.
        int c = 0;
        for (i = 0; i < n; i++)
            c += unitMap.Count();
        r[0] = d.Exchange();

        for (i = 0; i < n; i++)
            c += cpp.Count();
        r[1] = d.Exchange();

        for (i = 0; i < n; i++)
            c += node.Id();
        r[2] = d.Exchange();

        for (i = 0; i < n; i++)
            if (SeraphGraftRefAlive(inst)) c++;
        r[3] = d.Exchange();

        string s;
        for (i = 0; i < n; i++)
            s = SeraphGraftEcho("abc");
        r[4] = d.Exchange();

        int v = 0;
        for (i = 0; i < n; i++)
            v += cpp.Get("k");
        r[5] = d.Exchange();

        // Поиск движкового метода: имя в рантайме против имени на компиляции. Оба
        // натива зовут один и тот же метод одного и того же класса.
        for (i = 0; i < n; i++)
            c += SeraphGraftCallByRuntimeName(inst);
        r[6] = d.Exchange();
        for (i = 0; i < n; i++)
            c += SeraphGraftCallByCompileName(inst);
        r[7] = d.Exchange();

        // Длинная строка: короткая живёт в SSO-буфере std::string и аллокации не
        // требует вовсе, поэтому на ней экономия арены не видна. Здесь результат в
        // SSO не влезает — и разница между копией и перемещением становится заметна.
        string longArg = "0123456789012345678901234567890123456789012345678901234567890123";
        for (i = 0; i < n; i++)
            s = SeraphGraftEcho(longArg);
        r[8] = d.Exchange();

        // Тот же результат, но натив вернул graft::text: форматирование пошло прямо в
        // арену, временной std::string не было. Разница с r[8] — цена одной аллокации.
        for (i = 0; i < n; i++)
            s = SeraphGraftEchoArena(longArg);
        r[9] = d.Exchange();

        // Массив-аргумент: копия в std::vector против вьюхи std::span на ту же память.
        array<int> nums = new array<int>;
        for (i = 0; i < 16; i++)
            nums.Insert(i);
        d.Exchange();
        for (i = 0; i < n; i++)
            c += SeraphGraftSumArray(nums);
        r[10] = d.Exchange();
        for (i = 0; i < n; i++)
            c += SeraphGraftSumSpan(nums);
        r[11] = d.Exchange();

        // Сахар sol2 против явной формы: то же поле, тот же кэш слота.
        for (i = 0; i < n; i++)
            c += SeraphGraftNodeIdProxy(node);
        r[12] = d.Exchange();

        // Вызов В ДРУГУЮ СТОРОНУ: маршалируемый движковый proto с out-аргументом.
        for (i = 0; i < n; i++)
            c += SeraphGraftClassVarInt(node, "m_id");
        r[13] = d.Exchange();

        // Вызов объявления, которого в этой сборке игры нет: зеркало печатает объединение
        // всех веток препроцессора, поэтому такое законно и обязано стоить копейки.
        for (i = 0; i < n; i++)
            c += SeraphGraftCallMissing(inst);
        r[14] = d.Exchange();

        // Цена защиты от падения, когда ничего не падает. На x64 SEH табличный, поэтому
        // ожидание — ноль; метрика это проверяет, а не предполагает.
        for (i = 0; i < n; i++)
            c += SeraphGraftGuardedNoop(i);
        r[15] = d.Exchange();
    }

    // ── Прокси поля: синтаксис sol2 на живом объекте ────────────────────────
    [TEST_CASE("Field_ProxyAgreesWithExplicitForm").IN(SERAPH_GRAFT_TEST)];
    void Field_ProxyAgreesWithExplicitForm()
    {
        SeraphNode node = new SeraphNode(1234, 2.5, "узел");
        int viaProxy = SeraphGraftNodeIdProxy(node);
        int viaExplicit = node.Id();
        bool ok = false;
        if (viaProxy == 1234)
        {
            if (viaProxy == viaExplicit)
                ok = true;
        }
        assert(ok, "1234 == 1234", viaProxy.ToString() + " / " + viaExplicit.ToString(),
            "node[\"m_id\"_f] читает то же поле, что и field<T, имя>()");
    }

    // ── Падение нашего кода: чьё оно ────────────────────────────────────────
    // В проекте было записано, что SEH тут не работает. Разбор движка это опроверг:
    // его VEH зарегистрирован с First=0 (в конец списка) и на всё, кроме порчи кучи,
    // возвращает CONTINUE_SEARCH. Значит кадровый обработчик наш.
    //
    // Кейс намеренно роняет обращение по нулю ВНУТРИ натива. Если запись была верна —
    // сервер умрёт прямо здесь, и это будет видно. Если верен разбор — вернётся метка.
    [TEST_CASE("Crash_OurFaultIsOurs").IN(SERAPH_GRAFT_TEST)];
    void Crash_OurFaultIsOurs()
    {
        int here = SeraphGraftThreadHere();
        int atLoad = SeraphGraftThreadAtLoad();
        int caught = SeraphGraftGuardedFault();
        assert(caught == 1508, "1508",
            caught.ToString() + " (поток " + here.ToString() + ", загрузка " + atLoad.ToString() + ")",
            "падение внутри натива ловится нами, а не движком");
    }

    // ── Падение НЕЗАЩИЩЁННОГО натива: его забирает библиотека ────────────────
    // Кейс выше проверял, что кадровый обработчик вообще получает управление, — но там
    // натив защищал себя сам, да ещё и с noinline. Здесь натив не делает ничего:
    // обращение по нулю написано прямо в его теле, как его и напишет живой человек.
    // Ловить обязан трамплин, и это единственное, что отделяет кривой плагин от
    // мёртвого сервера.
    [TEST_CASE("Crash_LibraryCatchesUnguardedNative").IN(SERAPH_GRAFT_TEST)];
    void Crash_LibraryCatchesUnguardedNative()
    {
        int before = GraftFaultCount();
        int got = SeraphGraftFaultsHere();
        assert(got == 0, "0", got.ToString(), "упавший натив отдаёт нулевое значение");
        assert(GraftFaultCount() == before + 1, (before + 1).ToString(),
            GraftFaultCount().ToString(), "падение сосчитано");
        string last = GraftLastFault();
        assert(last.Contains("SIXW_GRAFT"), "имя плагина", last, "в отчёте написано, чей натив упал");
        assert(last.Contains("SeraphGraftFaultsHere"), "имя натива", last, "и как он называется");
        assert(last.Contains("c0000005"), "код сбоя", last, "и что именно случилось");
    }

    // Исключение C++ — второй слой защиты. Отдельный, потому что `try` и `__try` в одной
    // функции стоять не могут, и значит это два разных пути, а не один.
    [TEST_CASE("Crash_LibraryCatchesCppException").IN(SERAPH_GRAFT_TEST)];
    void Crash_LibraryCatchesCppException()
    {
        int before = GraftFaultCount();
        int got = SeraphGraftThrowsHere();
        assert(got == 0, "0", got.ToString(), "бросивший натив отдаёт нулевое значение");
        assert(GraftFaultCount() == before + 1, (before + 1).ToString(),
            GraftFaultCount().ToString(), "исключение сосчитано");
        string last = GraftLastFault();
        assert(last.Contains("натив передумал"), "текст исключения", last,
            "в отчёте написано, на что жаловались");
    }

    // Возврат строки после отказа. Движок копирует её сам и nullptr не проверяет:
    // если защита отдаст ноль, упадёт уже ДВИЖОК, и виноватых не найти.
    [TEST_CASE("Crash_FailedTextNativeGivesEmptyString").IN(SERAPH_GRAFT_TEST)];
    void Crash_FailedTextNativeGivesEmptyString()
    {
        string got = SeraphGraftThrowsText();
        assert(got == "", "пустая строка", "'" + got + "'",
            "упавший строковый натив не роняет движок на копировании");
    }

    // Сервер после трёх падений подряд обязан остаться рабочим — не «не упал», а именно
    // рабочим: вызовы идут, строки возвращаются, арена не уехала.
    [TEST_CASE("Crash_ServerKeepsWorkingAfterwards").IN(SERAPH_GRAFT_TEST)];
    void Crash_ServerKeepsWorkingAfterwards()
    {
        int i;
        for (i = 0; i < 3; i++)
        {
            SeraphGraftFaultsHere();
        }
        assert(SeraphGraftPing(0) == 0x0005E1AF, "385455", SeraphGraftPing(0).ToString(),
            "числовой вызов после падений");
        assert(SeraphGraftEcho("живой") == "echo:живой", "echo:живой", SeraphGraftEcho("живой"),
            "строковый вызов после падений: арена не уехала");
    }

    [TEST_CASE("Perf_HotPathBudget").IN(SERAPH_GRAFT_TEST)];
    void Perf_HotPathBudget()
    {
        int n = 10000;
        int i;
        int k;

        // Минимум из нескольких раундов, а не один замер: шум времени только ДОБАВЛЯЕТ
        // (чужой поток, промах кеша, скачок частоты), поэтому наименьшее из наблюдений
        // ближе всего к настоящей цене. Один раунд давал разброс до 40%, и порог на нём
        // пришлось бы ставить втрое выше измеренного — то есть почти бесполезным.
        // Шесть раундов, а не четыре: на четырёх вентиль всё ещё изредка мигал.
        float best[16];
        float cur[16];
        for (k = 0; k < 16; k++)
            best[k] = 1000000;
        for (i = 0; i < 6; i++)
        {
            PerfRound(n, cur);
            for (k = 0; k < 16; k++)
                if (cur[k] < best[k]) best[k] = cur[k];
        }

        float unit = best[0];
        float rBridge = best[1] / unit;
        float rField = best[2] / unit;
        float rObj = best[3] / unit;
        float rText = best[4] / unit;
        float rMarshal = best[5] / unit;
        float rLookupRt = best[6] / unit;
        float rLookupCt = best[7] / unit;
        float rTextLong = best[8] / unit;
        float rTextArena = best[9] / unit;
        float rArrVec = best[10] / unit;
        float rArrSpan = best[11] / unit;
        float rFieldProxy = best[12] / unit;
        float rProtoOut = best[13] / unit;
        float rMissing = best[14] / unit;
        float rGuarded = best[15] / unit;

        string got = "bridge " + rBridge.ToString() + " field " + rField.ToString();
        got = got + " obj " + rObj.ToString() + " text " + rText.ToString();
        got = got + " marshal " + rMarshal.ToString();
        got = got + " lookupRT " + rLookupRt.ToString() + " lookupCT " + rLookupCt.ToString();
        got = got + " textLong " + rTextLong.ToString() + " textArena " + rTextArena.ToString();
        got = got + " arrVec " + rArrVec.ToString() + " arrSpan " + rArrSpan.ToString();
        got = got + " fieldProxy " + rFieldProxy.ToString() + " protoOut " + rProtoOut.ToString();
        got = got + " missing " + rMissing.ToString() + " guarded " + rGuarded.ToString();
        got = got + " unit " + unit.ToString();

        // В graft.log, а не в журнал сьюты: тот эфемерный и удаляется вместе с профилем,
        // а числа нужны и на зелёном прогоне — из них берутся цифры в README.
        SeraphGraftNote("[perf] " + got);

        assert(rBridge < PERF_BRIDGE, "< " + PERF_BRIDGE.ToString(), got, "мост поверх ванильного натива");
        assert(rField < PERF_FIELD, "< " + PERF_FIELD.ToString(), got, "чтение поля по имени");
        assert(rObj < PERF_OBJ, "< " + PERF_OBJ.ToString(), got, "объектный аргумент (deref_object)");
        assert(rText < PERF_TEXT, "< " + PERF_TEXT.ToString(), got, "возврат owned string");
        assert(rMarshal < PERF_MARSHAL, "< " + PERF_MARSHAL.ToString(), got, "маршалируемый вызов шаблона");
        // Имя метода на компиляции обязано быть дешевле имени в рантайме. Сравниваем
        // одно с другим, а не с потолком: обе величины сняты в одном раунде.
        assert(rLookupCt < rLookupRt, "lookupCT < lookupRT", got,
            "имя метода на компиляции дешевле поиска на каждом вызове");
        assert(rLookupCt < PERF_LOOKUP, "< " + PERF_LOOKUP.ToString(), got,
            "вызов по имени из типа");
        assert(rTextLong < PERF_TEXT_LONG, "< " + PERF_TEXT_LONG.ToString(), got,
            "возврат длинной строки (мимо SSO)");
        // Форматирование прямо в арену не может быть дороже пути через std::string:
        // это ровно тот же результат минус одна аллокация.
        assert(rTextArena < rTextLong, "textArena < textLong", got,
            "возврат graft::text дешевле возврата std::string");
        // span смотрит в тот же буфер, что копирует vector: дороже он быть не может.
        assert(rArrSpan < rArrVec, "arrSpan < arrVec", got,
            "массив через std::span дешевле копии в std::vector");
        // Сахар обязан быть бесплатным: то же поле, тот же кэш слота, только имя приехало
        // через operator[]. Сравниваем с явной формой, а не с потолком — обе в одном раунде.
        assert(rFieldProxy < rField * PERF_SUGAR, "fieldProxy ~ field", got,
            "прокси поля стоит столько же, сколько field<T, имя>()");
        // Вызов наружу дороже вызова внутрь по природе: собирается блок переменных,
        // копируются шаблоны. Потолок сторожит грубую регрессию, а не точное число.
        assert(rProtoOut < PERF_PROTO_OUT, "< " + PERF_PROTO_OUT.ToString(), got,
            "маршалируемый вызов движкового proto из C++");
        // Зеркало объявляет ВСЕ ветки препроцессора, значит часть методов в этой сборке
        // не существует. Их вызов обязан быть дешевле обычного натива: поиск прекращается
        // после нескольких промахов и дальше это чтение статической ячейки. Без потолка
        // тут был бы полный поиск на каждый вызов — те самые 21 единица.
        assert(rMissing < PERF_MISSING, "< " + PERF_MISSING.ToString(), got,
            "вызов объявления, которого нет в этой сборке игры");
        // Защита от падения на x64 табличная: в рантайме её быть не должно вовсе.
        assert(rGuarded < PERF_GUARDED, "< " + PERF_GUARDED.ToString(), got,
            "натив под __try стоит столько же, сколько без него");
    }

    void MakeBoxInScope()
    {
        SeraphBox<int> box = new SeraphBox<int>;
        box.Bump(5);   // тут заводится экземпляр на стороне C++
    }

    [TEST_CASE("Lifetime_ScopeExit").IN(SERAPH_GRAFT_TEST)];
    void Lifetime_ScopeExit()
    {
        int before = SeraphGraftLiveBoxes();
        MakeBoxInScope();
        int after = SeraphGraftLiveBoxes();
        assert(after == before, before.ToString(), after.ToString(),
            "объект вышел из скоупа: C++ отпустил его вместе со скриптовым");
    }

    [TEST_CASE("Lifetime_ExplicitDelete").IN(SERAPH_GRAFT_TEST)];
    void Lifetime_ExplicitDelete()
    {
        int before = SeraphGraftLiveBoxes();
        SeraphBox<int> box = new SeraphBox<int>;
        box.Bump(7);
        int alive = SeraphGraftLiveBoxes();
        delete box;
        int after = SeraphGraftLiveBoxes();
        bool ok = false;
        if (alive == before + 1)
        {
            if (after == before)
                ok = true;
        }
        assert(ok, (before + 1).ToString() + " / " + before.ToString(),
            alive.ToString() + " / " + after.ToString(),
            "delete в скрипте доходит до C++ без единой строчки в скрипте");
    }

    [TEST_CASE("Lifetime_NoNativeCallNoInstance").IN(SERAPH_GRAFT_TEST)];
    void Lifetime_NoNativeCallNoInstance()
    {
        int before = SeraphGraftLiveBoxes();
        SeraphBox<int> untouched = new SeraphBox<int>;   // ни одного натива не звали
        int after = SeraphGraftLiveBoxes();
        delete untouched;
        assert(after == before, before.ToString(), after.ToString(),
            "экземпляр C++ заводится только при первом нативном вызове");
    }

    // ── Обратное направление: движковые `proto` из C++ ───────────────────────
    // До этих кейсов всё ниже существовало только как разбор дизассемблера. Здесь оно
    // проверяется на живом движке — по одному шагу за кейс, чтобы при поломке было
    // видно, ЧТО именно поехало: чтение дескриптора, сборка блока или сам вызов.

    // Шаг 1. Дескриптор функции знает, сколько у неё параметров (+88). Не сойдётся —
    // блок аргументов собирать не по чему.
    [TEST_CASE("Out_DescriptorCarriesParamCount").IN(SERAPH_GRAFT_TEST)];
    void Out_DescriptorCarriesParamCount()
    {
        // EnScript.GetClassVar(Class inst, string varname, int index, out void result)
        int n = SeraphGraftProtoArity("EnScript", "GetClassVar");
        assert(n == 4, "4", n.ToString(), "число параметров читается из дескриптора");
    }

    // Шаг 2. У параметра есть готовый шаблон переменной, и тег в нём настоящий. Это и
    // есть находка, на которой держится всё остальное: шаблон не сочиняем, а копируем.
    [TEST_CASE("Out_ParamTemplateHasRealTypeTag").IN(SERAPH_GRAFT_TEST)];
    void Out_ParamTemplateHasRealTypeTag()
    {
        int tag = SeraphGraftProtoParamTag("EnScript", "GetClassVar", 0);
        int family = (tag >> 28) & 0xF;   // первый параметр — Class, семейство 6
        assert(family == 6, "6", family.ToString(), "шаблон переменной несёт сама функция");
    }

    
    

    
    // Шаг 3. Настоящий маршалируемый вызов наружу с проверяемым ответом:
    // typename.GetModule() зовётся на дескрипторе класса и говорит, где класс объявлен.
    [TEST_CASE("Out_MarshalledCallReturnsValue").IN(SERAPH_GRAFT_TEST)];
    void Out_MarshalledCallReturnsValue()
    {
        string module = SeraphGraftTypeModule("Man");
        assert(module == "Game", "Game", module, "маршалируемый `proto` движка зовётся из C++");
    }

    // Шаг 4. Создание скриптового объекта из C++ — тот же `new`, что в скрипте.
    [TEST_CASE("Out_SpawnCreatesScriptObject").IN(SERAPH_GRAFT_TEST)];
    void Out_SpawnCreatesScriptObject()
    {
        string made = SeraphGraftSpawnClass("array<int>");
        assert(made == "array<int>", "array<int>", made, "typename.Spawn создаёт объект из C++");
    }


    // Шаг 5. Переменная по имени руками движка — маршалируемый вызов с out-аргументом
    // целиком. Сверяем с прямым чтением поля: разойдутся — виноват один из двух путей.
    //
    // Заодно закреплены две особенности GetClassVar, найденные разведкой: код возврата
    // врёт (0 и на успехе), а глобальные переменные она не отдаёт — из скрипта тоже.
    [TEST_CASE("Out_ClassVarThroughEngine").IN(SERAPH_GRAFT_TEST)];
    void Out_ClassVarThroughEngine()
    {
        SeraphNode node = new SeraphNode(4242, 1.5, "узел");
        int viaEngine = SeraphGraftClassVarInt(node, "m_id");
        assert(viaEngine == 4242, "4242", viaEngine.ToString(),
            "поле читается движковым GetClassVar из C++");
    }

    [TEST_CASE("Out_GlobalVariableIsNotServedByEngine").IN(SERAPH_GRAFT_TEST)];
    void Out_GlobalVariableIsNotServedByEngine()
    {
        DayZGame fromScript;
        EnScript.GetClassVar(null, "g_Game", 0, fromScript);
        // Не наша беда: из самого скрипта тот же вызов даёт тот же null. Кейс закрепляет
        // факт — починят движок, покраснеет он, а не поиск корня.
        assert(fromScript == null, "null", "не null",
            "глобальные переменные GetClassVar не отдаёт даже скрипту");
    }

    // Шаг 5б. Корень игры приходит вместе с тиком: мод и так зовёт GraftTick одной
    // строкой, поэтому GetGame() приезжает бесплатно и без единой лишней строки.
    [TEST_CASE("Out_RootArrivesWithTick").IN(SERAPH_GRAFT_TEST)];
    void Out_RootArrivesWithTick()
    {
        GraftTick(0.0, GetGame());
        string want = GetGame().ClassName();
        string got = SeraphGraftGameClass();
        assert(got == want, want, got, "корень игры доступен из C++");
    }

    // Шаг 8. Третья и последняя форма вызова: маршалируемый метод НАСТОЯЩЕГО КЛАССА,
    // с объектом в rcx. Две другие (статическая и метод типа-значения) проверены выше;
    // эта до сих пор выполнялась только при живом игроке (GetPlainId), то есть почти
    // никогда. Здесь она работает на пустом сервере: ScriptModule.CallFunction зовёт
    // скриптовую функцию GetGame(), и ответ сверяется с корнем от тика.
    [TEST_CASE("Out_ObjectReceiverMarshalledCall").IN(SERAPH_GRAFT_TEST)];
    void Out_ObjectReceiverMarshalledCall()
    {
        GraftTick(0.0, GetGame());
        string want = GetGame().ClassName();
        string got = SeraphGraftCallScriptFunction();
        assert(got == want, want, got,
            "маршалируемый метод объекта: ScriptModule.CallFunction зовёт скриптовую функцию");
    }

    // Шаг 6. Целевой сценарий целиком: корень -> буфер -> движковый GetPlayers -> обход.
    // На пустом сервере ответ "0:" — и он уже доказывает всю цепочку.
    [TEST_CASE("Out_PlayersFromCppMatchScript").IN(SERAPH_GRAFT_TEST)];
    void Out_PlayersFromCppMatchScript()
    {
        GraftTick(0.0, GetGame());
        array<Man> mine = new array<Man>;
        GetGame().GetPlayers(mine);
        string got = SeraphGraftPlayerIds();
        string want = mine.Count().ToString() + ":";
        assert(got.IndexOf(want) == 0, want + "...", got,
            "игроки собираются из C++ тем же движковым GetPlayers");
    }

    // Шаг 7. И их steam id — то, ради чего всё это. Пропускаем, если на сервере пусто:
    // проверять нечего, а красный кейс на пустом сервере ничего не значит.
    // Шаг 8. Возврат ОБЪЕКТА из движкового натива. Правило x64 ABI: структура с базовым
    // классом (а зеркало всё такое) возвращается через скрытый буфер, движок же кладёт
    // указатель в rax. Разъедется — сюда приедет не тот объект, и это ловится сверкой с
    // указателем, пришедшим из скрипта. Стоило steam id: GetIdentity отдавал мусор,
    // GetPlainId у него — пустую строку.
    [TEST_CASE("Out_ObjectReturnIsTheSameObjectAsInScript").IN(SERAPH_GRAFT_TEST)];
    void Out_ObjectReturnIsTheSameObjectAsInScript()
    {
        GraftTick(0.0, GetGame());
        bool same = SeraphGraftObjectReturnMatches(GetGame().GetMission());
        assert(same, "true", same.ToString(), "натив вернул тот же объект, что видит скрипт");
    }

    [TEST_CASE("Out_SteamIdsFromCppMatchScript").IN(SERAPH_GRAFT_TEST)];
    void Out_SteamIdsFromCppMatchScript()
    {
        GraftTick(0.0, GetGame());
        array<Man> mine = new array<Man>;
        GetGame().GetPlayers(mine);
        if (mine.Count() == 0)
        {
            assert(true, "-", "-", "на сервере нет игроков — сверять нечего");
            return;
        }
        PlayerIdentity id = mine.Get(0).GetIdentity();
        if (!id)
        {
            assert(true, "-", "-", "у игрока нет identity — сверять нечего");
            return;
        }
        string want = id.GetPlainId();
        string got = SeraphGraftPlayerIds();
        assert(got.Contains(want), "содержит " + want, got,
            "steam id читается из C++ маршалируемым GetPlainId");
    }

    // ── Точка входа ─────────────────────────────────────────────────────────
    // Своего потока у библиотеки нет: всё обязано идти по скриптовому. GraftTick — то
    // место, где C++ просыпается, и мод зовёт его из своего OnUpdate.

    [TEST_CASE("Tick_PluginIsSubscribed").IN(SERAPH_GRAFT_TEST)];
    void Tick_PluginIsSubscribed()
    {
        int subscribed = GraftTickCount();
        assert(subscribed > 0, "> 0", subscribed.ToString(),
            "подписка плагина дошла до хоста");
    }

    [TEST_CASE("Tick_ReachesPlugin").IN(SERAPH_GRAFT_TEST)];
    void Tick_ReachesPlugin()
    {
        int before = SeraphGraftTicks();
        GraftTick(0.25, GetGame());
        int after = SeraphGraftTicks();
        // Не равенство: тик теперь идёт и сам, по кадрам движка, поэтому между двумя
        // чтениями может пройти настоящий кадр. Проверяется, что вызов ДОШЁЛ.
        assert(after > before, "> " + before.ToString(), after.ToString(),
            "тик доходит из скрипта до кода на C++");
    }

    // РАЗВЕДКА: чем зацепиться за кадр без этой самой строки в скрипте. Проверяется
    // одно — совпадает ли поток, на котором бьётся кандидат, с потоком, на котором зовут
    // нативы. Не совпал — кандидат бесполезен, звать движок оттуда нельзя.
    // Числа уезжают в graft.log: журнал сьюты эфемерный, а решение принимается по ним.
    [TEST_CASE("Tick_LoopProbe").IN(SERAPH_GRAFT_TEST)];
    void Tick_LoopProbe()
    {
        // Тик без единой строки в скрипте: обработчик плагина обязан быть вызван столько
        // же раз, сколько кадров насчитал движок, — а GraftTick из мода никто не звал.
        assert(GraftFrames() > 0, "> 0", GraftFrames().ToString(),
            "хук на движковую точку входа кадра сработал");
        assert(SeraphGraftTicks() >= GraftFrames(), ">= " + GraftFrames().ToString(),
            SeraphGraftTicks().ToString(), "кадр движка доходит до плагина без скрипта");
        // Корень приезжает первым аргументом движковой точки входа — тем же, что и
        // GetGame(). Проверяем не указатель, а следствие: игровой объект читается.
        assert(GraftRootstockMatches(GetGame()), "да", GraftRootstockMatches(GetGame()).ToString(),
            "корень из точки входа кадра — это GetGame()");
        // dt обязан быть ТОТ ЖЕ, что движок вручил скрипту: мы читаем его поле
        // m_DeltaTime из того же кадра, а GetDeltaT() возвращает его же.
        assert(GraftFrameDt() == GetGame().GetDeltaT(), GetGame().GetDeltaT().ToString(),
            GraftFrameDt().ToString(), "dt кадра — движковый timeslice, а не наши часы");
        // ГЛАВНОЕ. Тик приходит из движковой точки входа кадра, то есть СНАРУЖИ
        // скриптового вызова. Вопрос «законен ли отсюда вызов движкового proto» был
        // открыт всё исследование — вот ответ, фактом.
        string tickInfo = "[tick] ok " + SeraphGraftTickProtoOk().ToString();
        tickInfo = tickInfo + " fail " + SeraphGraftTickProtoFail().ToString();
        tickInfo = tickInfo + " why " + SeraphGraftTickProtoWhy().ToString();
        tickInfo = tickInfo + " class " + SeraphGraftTickClassFound().ToString();
        tickInfo = tickInfo + " ticks " + SeraphGraftTicks().ToString();
        tickInfo = tickInfo + " varOk " + SeraphGraftTickVarOk().ToString();
        tickInfo = tickInfo + " varWhy " + SeraphGraftTickVarWhy().ToString();
        SeraphGraftNote(tickInfo);
        assert(SeraphGraftTickVarOk() > 0, "> 0", SeraphGraftTickVarOk().ToString(),
            "движковый proto с числовым результатом зовётся из точки входа кадра");
        assert(SeraphGraftTickVarWhy() == -1, "-1", SeraphGraftTickVarWhy().ToString(),
            "и ни разу не отказал");
        // ИЗВЕСТНАЯ ГРАНИЦА, а не забытый баг: строковый результат из тика приходит
        // пустым, хотя числовой и объектный приходят. Кейс сторожит границу — почините
        // возврат строки, и он покраснеет, требуя переписать себя.
        assert(SeraphGraftTickProtoOk() == 0, "0 (известная граница)",
            SeraphGraftTickProtoOk().ToString(),
            "строковый результат из тика пока не доезжает");
        // Счётчик падений тут не годится: кейсы Crash_* роняют нативы нарочно и бегут
        // раньше. Что тик цел — видно из того, что вызовы наружу продолжают удаваться:
        // сбой внутри обработчика оборвал бы всю цепочку.

        string probe = GraftLoopProbe();
        SeraphGraftNote("[loop] кадр " + EnProfiler.GetGameFrame().ToString() + " | " + probe);
        assert(probe != "", "не пусто", probe, "разведка кадра отвечает");
    }

    [TEST_CASE("Tick_DeltaArrivesIntact").IN(SERAPH_GRAFT_TEST)];
    void Tick_DeltaArrivesIntact()
    {
        GraftTick(0.25, GetGame());
        float dt = SeraphGraftLastDt();
        assert(dt == 0.25, "0.25", dt.ToString(), "dt доезжает без искажений");
    }
}
