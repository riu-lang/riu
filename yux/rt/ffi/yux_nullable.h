// yuxrt — Nullable<T> / T? C API（D2）
// 内存 layout：offset 0 = uint8_t has（yux bool 存 1 字节）；value 按 T 对齐。
// C 只通过指针读，不按值传 Nullable（LLVM i1 vs C _Bool 会对不齐）。
#ifndef YUX_NULLABLE_H
#define YUX_NULLABLE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* has != 0 表示有值。n 指向 yux Nullable<T> 的内存（通常来自 T& → Ptr）。 */
int yux_nullable_has(const void* n);

/* value 相对 n 的字节偏移 = align_up(1, value_align)。 */
size_t yux_nullable_value_offset(size_t value_align);

/* 指向 _value。T 的对齐须与 yux 一致（标量按其宽度；指针 8）。 */
void* yux_nullable_value(void* n, size_t value_align);

#ifdef __cplusplus
}
#endif

#endif /* YUX_NULLABLE_H */
