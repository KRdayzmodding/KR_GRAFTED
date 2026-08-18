#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "graft/scan.hpp"

// Привязка к кадру движка без строки в скрипте. Только хост.
// К чему именно прицеплено и почему — в SRC/graft/frame.cpp.
namespace graft::frame {

namespace detail {
void note_frame(float dt);
void note_state(const char* what);
// Вызов совпал по индексу, но не наш: wrong_class — чужой объект, иначе индекс ещё
// не разрешён (-1). Оба раньше проходили фильтр и роняли сервер.
void note_reject(bool wrong_class);
}  // namespace detail

void install(const std::vector<scan::view>& sections);  // после MH_Initialize

std::size_t frames();      // сколько кадров движка поймано
float last_dt();           // timeslice последнего кадра — движковый, не наши часы
const char* state();
std::size_t rejected_class();      // отсеяно по классу объекта
std::size_t rejected_unresolved();  // отсеяно по неразрешённому индексу       // встала привязка или нет, одной строкой

}  // namespace graft::frame
