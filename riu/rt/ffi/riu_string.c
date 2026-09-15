// riurt — String：从 Rc<Array<u32>> block 读码点 view；retain/release 走 riu_rc_*
#include "ffi/riu_string.h"
#include "ffi/riu_rc.h"

void* riu_string_block(const void* string_struct) {
    void* block;
    if (!string_struct) {
        return 0;
    }
    block = *(void* const*)string_struct;
    return block;
}

riu_string_view riu_string_view_from_block(const void* block) {
    riu_string_view v;
    const char* payload;
    v.data = 0;
    v.len = 0;
    if (!block) {
        return v;
    }
    payload = (const char*)block + RIU_RC_HEADER_SIZE;
    v.data = *(const uint32_t* const*)payload;
    v.len = (size_t)(*(const int64_t*)(payload + sizeof(void*)));
    return v;
}

riu_string_view riu_string_as_view(const void* string_struct) {
    return riu_string_view_from_block(riu_string_block(string_struct));
}

void riu_string_retain(void* string_struct) {
    riu_rc_retain(riu_string_block(string_struct));
}

void riu_string_release(void* string_struct) {
    riu_rc_release(riu_string_block(string_struct));
}
