// Классы мода, к которым graft привязывает нативы.
//
// Методы объявляются как у ванильных коллекций — обычными членами. Объект приходит в
// C++ первым аргументом (это и есть this), в объявление скрипта он не попадает.
class SeraphGraft
{
}

// Вложенная структура: класс, у которого поля — числа, строка, другой такой же объект
// и массив. C++ читает всё это по именам полей, на любую глубину.
class SeraphNode
{
    int m_id;
    float m_weight;
    string m_label;
    ref SeraphNode m_child;
    ref array<int> m_values;

    void SeraphNode(int id, float weight, string label)
    {
        m_id = id;
        m_weight = weight;
        m_label = label;
        m_values = new array<int>;
    }
}

// SeraphBox<T> — СВОЙ шаблонный тип с proto-реализацией в C++. Объявление написано
// руками: шаблонный синтаксис генератор не выражает, C++ цепляется привязкой с
// graft::declared. Методы не упоминают T, поэтому компилируются в обычные нативы —
// ровно так же, как движок поступает с array.Count/Resize.
class SeraphBox<Class T>
{
    proto native int Tag();           // импл в C++, от T не зависит
    proto native int Bump(int by);    // C++ ведёт состояние на каждый объект

    // Имя несёт метку плагина: у двух плагинов, держащих состояние на одном скриптовом
    // классе, освобождение должно быть своё у каждого. Метку ставит сборка
    // (GRAFT_PLUGIN_TAG), у сгенерированных объявлений она проставляется сама.
    private proto native void NativeDispose_SIXW_GRAFT();

    void ~SeraphBox()
    {
        NativeDispose_SIXW_GRAFT();   // состояние объекта на стороне C++
    }

    void Put(T value) { }   // обычный скриптовый метод, чтобы T был задействован
}
