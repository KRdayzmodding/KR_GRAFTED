/**
 * KR_UTEST - a single-file unit test framework for DayZ (Enforce Script).
 *
 * Drop this file into any 3_Game (or deeper) script folder of your mod, or load
 * the @KR_UTEST addon built from this repo. The KRUTEST include guard makes it
 * safe for several mods to ship their own copy at once: DayZ compiles every file
 * of one script module into a single translation unit, so only the first copy
 * gets compiled and the rest are skipped.
 *
 * Quick start:
 *
 *   [TEST_SUITE("math::basic", MATH_TEST)];
 *   class MATH_TEST : uTestSuite
 *   {
 *       [TEST_CASE("Add_Simple").IN(MATH_TEST)];
 *       void Add_Simple()
 *       {
 *           int a = 2 + 2;
 *           assert(a == 4, "4", a.ToString(), "2 + 2 must be 4");
 *       }
 *   }
 *
 * Running (the runner at the bottom of this file needs -scrDef=UTESTS_RUN):
 *
 *   DayZDiag_x64.exe ... -scrDef=UTESTS_RUN -utest=math::basic
 *
 *   -utest=              run every registered suite
 *   -utest=a,b           run suites "a" and "b"
 *   -utest_filter=X,Y    run only the cases named X and Y
 *
 * License: MIT.
 */
#ifndef KRUTEST
#define KRUTEST

enum uTestStatus { PASSED, FAILED, SKIPPED }

// ─────────────────────────────────────────────────────────────────────────────
//  Stack trace - used only to stamp file:line onto a failure.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * One parsed frame of a DumpStackString() dump.
 *
 *   input:  "MyTest() KR/mymod/3_Game/tests/math.c : 42"
 *   result: function = "MyTest", file = "KR/mymod/3_Game/tests/math.c", line = 42
 *
 * A leading '#' marks a call made from an attribute; it is stripped.
 */
class uTestFrame : Managed
{
    string function;
    string file;
    int    line;

    void uTestFrame(string _function, string _file, int _line)
    {
        function = _function;
        file     = _file;
        line     = _line;
    }
}

class uTestStack : Managed
{
    ref array<ref uTestFrame> frames = new array<ref uTestFrame>();

    //! Captures the current stack without the Capture() frame itself,
    //! so frame 0 is always the caller of Capture().
    static uTestStack Capture()
    {
        string raw;
        DumpStackString(raw);

        uTestStack stack = new uTestStack();

        array<string> lines = new array<string>();
        raw.Split("\n", lines);

        foreach (string entry : lines)
        {
            uTestFrame frame = ParseFrame(entry.Trim());
            if (frame) { stack.frames.Insert(frame); }
        }

        if (stack.frames.Count() > 0) { stack.frames.RemoveOrdered(0); }

        return stack;
    }

    int Count() { return frames.Count(); }

    uTestFrame Get(int index)
    {
        if (index < 0 || index >= frames.Count()) { return null; }
        return frames[index];
    }

    //! First frame belonging to the given function, or null.
    uTestFrame Find(string function)
    {
        for (int i = 0; i < frames.Count(); i++)
        {
            if (frames[i].function == function) { return frames[i]; }
        }
        return null;
    }

    string Format()
    {
        string result = "";
        for (int i = 0; i < frames.Count(); i++)
        {
            if (i > 0) { result += "\n"; }
            result += frames[i].function + "() " + frames[i].file + " : " + frames[i].line.ToString();
        }
        return result;
    }

    // Format: [#]FuncName() path/to/file.c : 123
    protected static uTestFrame ParseFrame(string entry)
    {
        if (entry.Length() == 0) { return null; }

        if (entry[0] == "#") { entry = entry.Substring(1, entry.Length() - 1); }

        int paren = entry.IndexOf("()");
        if (paren == -1) { return null; }

        string function = entry.Substring(0, paren);
        string rest     = entry.Substring(paren + 2, entry.Length() - paren - 2);
        rest = rest.Trim();

        string file = rest;
        int    line = 0;

        int colon = rest.LastIndexOf(" : ");
        if (colon != -1)
        {
            file = rest.Substring(0, colon);
            file = file.Trim();

            string line_str = rest.Substring(colon + 3, rest.Length() - colon - 3);
            line = line_str.Trim().ToInt();
        }

        return new uTestFrame(function, file, line);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Test case
// ─────────────────────────────────────────────────────────────────────────────

class uTestCaseInfo : Managed
{
    string name;
    string file;
    string line;

    void uTestCaseInfo(string _name, string _file = "", string _line = "")
    {
        name = _name;
        file = _file;
        line = _line;
    }
}

class uTestCase : Managed
{
    protected string      test_func;
    protected uTestStatus status = uTestStatus.PASSED;
    protected string      expected;
    protected string      actual;
    protected string      message;
    protected string      __file__;
    protected string      __line__;

    void uTestCase(string funcName) { test_func = funcName; }

    string      getFunc() { return     test_func; }
    uTestStatus getStatus() { return   status; }
    string      getExpected() { return expected; }
    string      getActual() { return   actual; }
    string      getMessage() { return  message; }
    string      getFile() { return     __file__; }
    string      getLine() { return     __line__; }

    void setPassed() { status  = uTestStatus.PASSED; }
    void setFailed() { status  = uTestStatus.FAILED; }
    void setSkipped() { status = uTestStatus.SKIPPED; }

    void setFile(string     file) { __file__ = file; }
    void setLine(string     line) { __line__ = line; }
    void setExpected(string v) { expected    = v; }
    void setActual(string   v) { actual      = v; }
    void setMessage(string  v) { message     = v; }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Suite
// ─────────────────────────────────────────────────────────────────────────────

class uTestSuite : Managed
{
    ref map<string, ref uTestCase> tests = new map<string, ref uTestCase>();
    string                         suiteName;
    int                            passed;
    int                            failed;
    int                            skipped;
    float                          duration;

    // Cases run one after another, so the framework always knows which case is
    // live. assert()/failed()/SKIP() therefore work from helper methods too -
    // the stack is only read to stamp the location of an actual failure.
    protected uTestCase current_case;

    void BeforeAll() {}
    void AfterAll() {}

    //! Runs every registered case of this suite. Cases are dispatched by name,
    //! so a case method must be a plain "void Name()" of this class.
    void RUN()
    {
        int start_ticks = TickCount(0);

        passed = 0; failed = 0; skipped = 0;

        BeforeAll();

        foreach (string name, ref uTestCase test : tests)
        {
            current_case = test;

            int call_ok = 0;
            if (GetGame() && GetGame().GameScript)
            {
                call_ok = GetGame().GameScript.CallFunction(this, test.getFunc(), NULL, 0);
            }

            if (!call_ok && test.getStatus() == uTestStatus.PASSED)
            {
                MarkFailed(test, "callable void " + test.getFunc() + "()", "call failed",
                           "[uTest] case method not found or has a non-empty signature");
            }
        }

        current_case = null;

        foreach (string _name, ref uTestCase _test : tests)
        {
            switch (_test.getStatus())
            {
            case uTestStatus.PASSED:  passed++;  break;
            case uTestStatus.FAILED:  failed++;  break;
            case uTestStatus.SKIPPED: skipped++; break;
            }
        }

        AfterAll();

        duration = (TickCount(0) - start_ticks) / 10000.0;
    }

    protected void MarkFailed(uTestCase test, string expected, string actual, string message)
    {
        test.setFailed();
        test.setExpected(expected);
        test.setActual(actual);
        test.setMessage(message);
    }

    // Frame 0 is StampLocation, frame 1 is assert/failed, frame 2 is whoever
    // called them. Prefer the frame of the case itself (assert may sit inside a
    // helper), fall back to the direct caller.
    protected void StampLocation(uTestCase test)
    {
        uTestStack stack = uTestStack.Capture();

        uTestFrame frame = stack.Find(test.getFunc());
        if (!frame) { frame = stack.Get(2); }
        if (!frame) { return; }

        test.setFile(frame.file);
        test.setLine(frame.line.ToString());
    }

    protected bool NoCase(string who)
    {
        if (current_case) { return false; }

        uTestStack stack = uTestStack.Capture();
        Error("[uTest] " + who + " called outside a running test case:\n" + stack.Format());
        return true;
    }

    protected void assert(Class class_obj, string expected = "", string actual = "", string message = "")
    {
        if (NoCase("assert()")) { return; }
        if (class_obj != null)  { return; }

        StampLocation(current_case);
        MarkFailed(current_case, expected, actual, message);
    }

    protected void assert(bool condition, string expected = "", string actual = "", string message = "")
    {
        if (NoCase("assert()")) { return; }
        if (condition)          { return; }

        StampLocation(current_case);
        MarkFailed(current_case, expected, actual, message);
    }

    protected void failed(string expected = "", string actual = "", string message = "")
    {
        if (NoCase("failed()")) { return; }

        StampLocation(current_case);
        MarkFailed(current_case, expected, actual, message);
    }

    protected void SKIP(string message = "")
    {
        if (NoCase("SKIP()")) { return; }

        current_case.setSkipped();
        current_case.setMessage(message);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Registry + runner
// ─────────────────────────────────────────────────────────────────────────────

class uTest : Managed
{
    static ref map<string, typename>                       suites = new map<string, typename>();
    static ref map<typename, ref array<ref uTestCaseInfo>> cases  = new map<typename, ref array<ref uTestCaseInfo>>();

    static void RegisterSuite(string name, typename suiteType)
    {
        suites[name] = suiteType;
        if (!cases.Contains(suiteType)) { cases[suiteType] = new array<ref uTestCaseInfo>(); }
    }

    static void RegisterTestCase(typename suiteType, string funcName, string file, string line)
    {
        if (!cases.Contains(suiteType)) { cases[suiteType] = new array<ref uTestCaseInfo>(); }
        cases[suiteType].Insert(new uTestCaseInfo(funcName, file, line));
    }

    //! Runs the named suites (all of them, alphabetically, when names is empty)
    //! and prints the report.
    static void RUN(array<string> names = null, array<string> caseFilter = null)
    {
        ref array<ref uTestSuite> results = new array<ref uTestSuite>();
        uTestSuite suite;

        if (names && names.Count() > 0)
        {
            foreach (string n : names)
            {
                if (!suites.Contains(n)) { Print("[uTest] Suite not found: " + n); continue; }
                suite = RunSuite(n, suites[n], caseFilter);
                if (suite) results.Insert(suite);
            }
        }
        else
        {
            array<string> sortedNames = {};
            foreach (string strn, typename st : suites)
            {
                sortedNames.Insert(strn);
            }

            sortedNames.Sort();

            foreach (string sn : sortedNames)
            {
                suite = RunSuite(sn, suites[sn], caseFilter);
                if (suite) results.Insert(suite);
            }
        }

        string header = "Runned tests";
        if (names && names.Count() == 1)
        {
            header += " in suite " + names[0];
        }
        else if (names && names.Count() > 1)
        {
            header += " in suites ";
            for (int ni = 0; ni < names.Count(); ni++)
            {
                if (ni > 0) header += ", ";
                header += names[ni];
            }
        }
        else { header += " in all suites"; }

        int fill = 89 - (header.Length() + 5);

        string bar = "";
        for (int bi = 0; bi < fill; bi++) bar += "─";

        if (fill > 0) { Print("\n─── " + header + " " + bar + "\n\n"); }
        else { Print("\n─── " + header + "\n\n"); }

        ReportResults(results);
        Print("\n─────────────────────────────────────────────────────────────────────────────────────────\n\n");
    }

    protected static uTestSuite RunSuite(string name, typename type, array<string> caseFilter = null)
    {
        uTestSuite suite = uTestSuite.Cast(type.Spawn());
        if (!suite) { Print("[uTest] Failed to spawn: " + type.ToString()); return null; }

        suite.suiteName = name;

        if (cases.Contains(type))
        {
            foreach (uTestCaseInfo info : cases[type])
            {
                if (caseFilter && caseFilter.Count() > 0 && caseFilter.Find(info.name) == -1)
                    continue;

                uTestCase tc = new uTestCase(info.name);
                tc.setFile(info.file);
                tc.setLine(info.line);
                suite.tests[info.name] = tc;
            }
        }

        suite.RUN();

        return suite;
    }

    protected static string Pad(string str, int width)
    {
        while (str.Length() < width) { str += " "; }
        return str;
    }

    //! "42.3" style formatting for the summary line, two decimals, no engine deps.
    protected static string PrettyMs(float value)
    {
        bool  negative = value < 0;
        float absolute = value;
        if (negative) { absolute = -value; }

        int total     = Math.Round(absolute * 100);
        int int_part  = total / 100;
        int frac_part = total % 100;

        string frac = frac_part.ToString();
        if (frac_part < 10) { frac = "0" + frac; }

        string result = "";
        if (negative) { result = "-"; }

        return result + int_part.ToString() + "." + frac;
    }

    protected static void ReportResults(array<ref uTestSuite> results)
    {
        int totalPassed, totalFailed, totalSkipped;
        uTestSuite suite;
        uTestCase tc;
        string row, loc;
        float sum_duration = 0;

        int maxNameLen = 0;
        foreach (uTestSuite s : results)
        {
            if (s.suiteName.Length() > maxNameLen)
                maxNameLen = s.suiteName.Length();
        }

        for (int i = 0; i < results.Count(); i++)
        {
            suite = results[i];
            row = " " + Pad(suite.suiteName, maxNameLen + 4);
            sum_duration += suite.duration;

            foreach (string case_name, ref uTestCase test : suite.tests)
            {
                tc = test;
                switch (tc.getStatus())
                {
                case uTestStatus.PASSED:  row += "·"; break;
                case uTestStatus.FAILED:  row += "╳"; break;
                case uTestStatus.SKIPPED: row += "○"; break;
                }
            }
            Print(""+row);

            totalPassed  += suite.passed;
            totalFailed  += suite.failed;
            totalSkipped += suite.skipped;
        }

        string summary = " " + totalPassed.ToString() + " passed";
        if (totalFailed > 0)  summary += "  " + totalFailed.ToString() + " failed";
        if (totalSkipped > 0) summary += "  " + totalSkipped.ToString() + " skipped";
        summary += "  " + PrettyMs(sum_duration) + " ms";

        Print("\n");
        Print(""+summary);

        if (totalFailed > 0)
        {
            Print("\n─────────────────────────────────────────────────────────────────────────────────────────");

            for (int fi = 0; fi < results.Count(); fi++)
            {
                suite = results[fi];
                foreach (string failed_name, ref uTestCase failed_test : suite.tests)
                {
                    tc = failed_test;
                    if (tc.getStatus() != uTestStatus.FAILED) { continue; }

                    loc = "";
                    if (tc.getFile() != "") { loc = "    " + tc.getFile() + ":" + tc.getLine(); }

                    row = " ✘ " + suite.suiteName + " ➜  " + tc.getFunc() + loc;

                    Print("\n");
                    Print(""+row);

                    if (tc.getMessage() != "") { Print("   " + tc.getMessage()); }

                    if (tc.getExpected() != "" || tc.getActual() != "") { Print("   " + tc.getActual() + "  !=  " + tc.getExpected()); }
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Registration attributes
// ─────────────────────────────────────────────────────────────────────────────

class TEST_CASE_ATTRIBUTE : Managed
{
    protected string   name;
    protected typename suite;
    protected string   file;
    protected string   line;

    void TEST_CASE_ATTRIBUTE(string testName) { name = testName; }

    TEST_CASE_ATTRIBUTE IN(typename suiteType)
    {
        suite = suiteType;
        uTest.RegisterTestCase(suite, name, file, line);
        return this;
    }
}

TEST_CASE_ATTRIBUTE TEST_CASE(string name) { return new TEST_CASE_ATTRIBUTE(name); }

class TEST_SUITE : Managed
{
    void TEST_SUITE(string category, typename suiteType) { uTest.RegisterSuite(category, suiteType); }
}

class uTestRunner : Managed
{
    ref array<string> caseFilter;
    ref array<string> suiteFilters;

    void uTestRunner(array<string> suiteNames = null, array<string> _caseFilter = null)
    {
        suiteFilters = suiteNames;
        caseFilter   = _caseFilter;
    }

    void Run() { uTest.RUN(suiteFilters, caseFilter); }

    //! Reads "-<CLIParam>=suite,suite" and "-<CLIFilter>=case,case" and runs.
    //! Returns null (and runs nothing) when the suites parameter is absent.
    static uTestRunner RunFromCLI(string CLIParam, string CLIFilter)
    {
        string CLIParamReaden;
        string CLIParamFilterReaden;

        if (!GetCLIParam(CLIParam, CLIParamReaden))
        {
            Print("[uTest] CLI parameter not set, nothing to run: -" + CLIParam);
            return null;
        }

        array<string> suiteNames = new array<string>();
        CLIParamReaden.Split(",", suiteNames);
        for (int si = suiteNames.Count() - 1; si >= 0; si--)
        {
            if (suiteNames[si] == "") suiteNames.Remove(si);
        }

        array<string> cases;
        if (GetCLIParam(CLIFilter, CLIParamFilterReaden))
        {
            cases = new array<string>();
            CLIParamFilterReaden.Split(",", cases);
            for (int ci = cases.Count() - 1; ci >= 0; ci--)
            {
                if (cases[ci] == "") cases.Remove(ci);
            }
        }

        uTestRunner runner = new uTestRunner(suiteNames, cases);
        runner.Run();
        return runner;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Entry point - compiled only with -scrDef=UTESTS_RUN
// ─────────────────────────────────────────────────────────────────────────────

#ifdef UTESTS_RUN
modded class DayZGame
{
    protected bool            utest_started;
    protected ref uTestRunner utest_runner;

    // Not the constructor: g_Game is assigned only after DayZGame is built, so
    // GameScript (needed to dispatch cases by name) is not reachable there yet.
    // First update with a live mission is the earliest safe moment.
    override void OnUpdate(bool doSim, float timeslice)
    {
        super.OnUpdate(doSim, timeslice);

        if (utest_started)           { return; }
        if (!GetGame())              { return; }
        if (!GetGame().GetMission()) { return; }

        utest_started = true;
        utest_runner  = uTestRunner.RunFromCLI("utest", "utest_filter");
    }
}
#endif

#endif
