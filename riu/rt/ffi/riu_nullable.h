// riurt — Nullable<T> / T? C API（D2）
// 内存 layout：offset 0 = uint8_t has（riu bool 存 1 字节）；value 按 T 对齐。
// C 只通过指针读，不按值传 Nullable（LLVM i1 vs C _Bool 会对不齐）。
#ifndef RIU_NULLABLE_H
#define RIU_NULLABLE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* has != 0 表示有值。n 指向 riu Nullable<T> 的内存（通常来自 T& → Ptr）。 */
int riu_nullable_has(const void* n);

/* value 相对 n 的字节偏移 = align_up(1, value_align)。 */
size_t riu_nullable_value_offset(size_t value_align);

/* 指向 _value。T 的对齐须与 riu 一致（标量按其宽度；指针 8）。 */
void* riu_nullable_value(void* n, size_t value_align);

#ifdef __cplusplus
}
#endif

#endif /* RIU_NULLABLE_H */
