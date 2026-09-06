// yuxrt — Nullable<T> 读 payload（不持有、不析构）
#include "ffi/yux_nullable.h"

size_t yux_nullable_value_offset(size_t value_align) {
    size_t off = 1; /* _has 占 1 字节 */
    size_t rem;
    if (value_align < 1) {
        value_align = 1;
    }
    rem = off % value_align;
    if (rem) {
        off += value_align - rem;
    }
    return off;
}

int yux_nullable_has(const void* n) {
    if (!n) {
        return 0;
    }
    return *(const unsigned char*)n != 0;
}

void* yux_nullable_value(void* n, size_t value_align) {
    if (!n) {
        return 0;
    }
    return (char*)n + yux_nullable_value_offset(value_align);
}
