// СГЕНЕРИРОВАНО protogen из C++ реестра нативов — руками не править.
// Источник истины — блоки GRAFT_BINDINGS в исходниках мода.
// Импл живёт в graft-модуле (proxy-DLL рядом с exe); без неё скрипт не слинкуется.
// Модуль: 1_Core


class CppHashMap<Class K, Class V>
{
    proto void Set(K p0, V p1);
    proto V Get(K p0);
    proto bool Contains(K p0);
    proto bool Remove(K p0);
    proto int Count();
    proto void Clear();
    proto K KeyAt(int p0);
}
