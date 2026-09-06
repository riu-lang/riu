// yuxrt — String：从 Rc<Array<u32>> block 读码点 view；retain/release 走 yux_rc_*
#include "ffi/yux_string.h"
#include "ffi/yux_rc.h"

void* yux_string_block(const void* string_struct) {
    void* block;
    if (!string_struct) {
        return 0;
    }
    block = *(void* const*)string_struct;
    return block;
}

yux_string_view yux_string_view_from_block(const void* block) {
    yux_string_view v;
    const char* payload;
    v.data = 0;
    v.len = 0;
    if (!block) {
        return v;
    }
    payload = (const char*)block + YUX_RC_HEADER_SIZE;
    v.data = *(const uint32_t* const*)payload;
    v.len = (size_t)(*(const int64_t*)(payload + sizeof(void*)));
    return v;
}

yux_string_view yux_string_as_view(const void* string_struct) {
    return yux_string_view_from_block(yux_string_block(string_struct));
}

void yux_string_retain(void* string_struct) {
    yux_rc_retain(yux_string_block(string_struct));
}

void yux_string_release(void* string_struct) {
    yux_rc_release(yux_string_block(string_struct));
}
