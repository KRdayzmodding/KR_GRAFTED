// СГЕНЕРИРОВАНО protogen из C++ реестра нативов — руками не править.
// Источник истины — блоки GRAFT_BINDINGS в исходниках мода.
// Импл живёт в graft-модуле (proxy-DLL рядом с exe); без неё скрипт не слинкуется.
// Модуль: 1_Core

proto native int ExampleAdd(int p0, int p1);
proto native owned string ExampleGreet(string p0);
proto native int ExampleSum(array<int> p0);
proto native int ExampleCountAbove(array<int> p0, int p1);
proto native int ExampleFillSquares(out array<int> p0, int p1);
proto native vector ExampleScale(vector p0, float p1);
proto native bool ExampleSay(string p0);
proto native bool ExampleComplain(string p0);

modded class ExampleGraft
{
    proto native int Version();
}
