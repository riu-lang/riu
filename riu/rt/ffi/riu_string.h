// riurt — String C API（D2）
// String = { _buf: Rc<Array<u32>> }。码点 u32，无 NUL。
// extern 把 String 自动转 Ptr 得到的是码点缓冲，没有长度，也不足以 retain。
// 要读 len / retain：传 String& → Ptr（指向 {handle}），再用本 API。
#ifndef RIU_STRING_H
#define RIU_STRING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct riu_string_view {
    const uint32_t* data; /* 码点；可空（空串 _data 常为 null） */
    size_t len;
} riu_string_view;

/* string_struct 指向 riu String（8 字节 handle）。来自 String& → Ptr。 */
void* riu_string_block(const void* string_struct);

riu_string_view riu_string_view_from_block(const void* block);
riu_string_view riu_string_as_view(const void* string_struct);

void riu_string_retain(void* string_struct);
void riu_string_release(void* string_struct);

#ifdef __cplusplus
}
#endif

#endif /* RIU_STRING_H */
