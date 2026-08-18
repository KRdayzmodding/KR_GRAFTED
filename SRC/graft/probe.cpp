#include <windows.h>

#include "graft/script.hpp"

// Единственная функция, которую inline-часть script.hpp зовёт наружу. Живёт отдельной
// TU, потому что нужна ОБЕИМ сторонам — и хосту, и плагину, — а тащить windows.h в
// заголовок, который включает каждый файл с нативами, не хочется.
//
// К хосту она отношения не имеет: ей нужна система, а не состояние библиотеки. Поэтому
// в таблицу сервисов она не идёт и на горячем пути остаётся обычным локальным вызовом.
namespace graft::script::detail {

bool readable(const void* p, std::size_t n) {
    if (!plausible(p)) {
        return false;
    }
    // Пробовать чтение под SEH нельзя: движок ставит свой фильтр исключений и рапортует
    // о падении раньше, чем сработал бы наш __except (проверено — вылет 0xC0000005 с
    // адресом внутри dwmapi.dll). Поэтому спрашиваем у системы, а не у процессора.
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof mbi) == 0 || mbi.State != MEM_COMMIT) {
        return false;
    }
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
        return false;
    }
    const auto* start = static_cast<const std::uint8_t*>(mbi.BaseAddress);
    return static_cast<const std::uint8_t*>(p) + n <= start + mbi.RegionSize;
}

}  // namespace graft::script::detail
