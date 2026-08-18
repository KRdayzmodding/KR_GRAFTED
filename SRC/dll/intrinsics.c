/* clang-cl эмитит вызовы __movsb/__stosb (из memcpy/memset в MinHook), но не даёт
 * символ. Даём совместимую реализацию — обычный ABI-вызов, линкер её подхватывает. */
#include <stddef.h>

void __movsb(unsigned char* dst, const unsigned char* src, size_t count) {
    while (count--) {
        *dst++ = *src++;
    }
}
