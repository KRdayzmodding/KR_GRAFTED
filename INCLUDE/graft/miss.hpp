// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once

// Почему обращение не состоялось. Возвращается в `std::expected<T, miss>`, чтобы промах
// было видно в типе и у него была причина, а не только ноль.
//
// Отдельным заголовком, а не в types.hpp: тот тянет за собой весь разбор типов Enforce, а
// причина отказа нужна и там, где его нет, — поиску по образу (scan.hpp) прежде всего.
namespace graft {

enum class miss {
    null_object,
    not_found,
    not_native,
    // Дальше — только про обратное направление (звать движковый `proto`): блок
    // аргументов собирается по дескриптору, и расхождение с ним это не «вернём ноль»,
    // а кривой кадр вызова.
    wrong_arity,
    too_many_args,
    unsafe_arg,
    wrong_type,  // тип аргумента разошёлся с дескриптором движка
    no_template, // переменную не из чего собрать: движок ещё не разобрал ядро
    // Дальше — про поиск в образе движка (scan.hpp). Дописаны в конец: числа прежних причин
    // не сдвигаются.
    ambiguous,  // нашлось несколько — выбирать из них нельзя: ошибиться в выборе дороже
                // отказа (врезка в угаданное место портит чужой вызов через час)
    unreadable, // память по адресу не отображена: искать там нечего, и это не то же
                // самое, что искомого в ней нет
};

// Причина словами — для журнала: по одному «не нашлось» не понять, пропал маяк или раздвоился.
// Без `default`: новая причина без слов — предупреждение компилятора, а не пустая строка.
inline const char* to_string(miss why) {
    switch (why) {
        case miss::null_object:
            return "null object";
        case miss::not_found:
            return "not found";
        case miss::not_native:
            return "not native";
        case miss::wrong_arity:
            return "wrong argument count";
        case miss::too_many_args:
            return "too many arguments";
        case miss::unsafe_arg:
            return "unsafe argument";
        case miss::wrong_type:
            return "wrong argument type";
        case miss::no_template:
            return "no variable template";
        case miss::ambiguous:
            return "several found";
        case miss::unreadable:
            return "memory is not readable";
    }
    return "?"; // значение вне перечисления: сюда не попасть, но без return — UB
}

} // namespace graft
