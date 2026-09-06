// yuxrt — Rc Block 实现（与 codegen 共用；不要在 IR 里再写一套计数协议）
#include "ffi/yux_rc.h"
#include "mem/mem.h"

int64_t __yux_rc_block_count = 0;

static yux_rc_header* hdr(void* block) {
    return (yux_rc_header*)block;
}

static int is_sentinel(const void* block) {
    return hdr((void*)block)->strong == YUX_RC_SENTINEL;
}

void* yux_rc_alloc(size_t payload_size) {
    void* block = yuxrt_alloc(YUX_RC_HEADER_SIZE + payload_size);
    hdr(block)->strong = 1;
    hdr(block)->weak = 1;
    __yux_rc_block_count += 1;
    return block;
}

void* yux_rc_payload(void* block) {
    if (!block) {
        return 0;
    }
    return (char*)block + YUX_RC_HEADER_SIZE;
}

void* yux_rc_block(void* payload) {
    if (!payload) {
        return 0;
    }
    return (char*)payload - YUX_RC_HEADER_SIZE;
}

void yux_rc_retain(void* block) {
    if (!block || is_sentinel(block)) {
        return;
    }
    hdr(block)->strong += 1;
}

int yux_rc_release_need_dtor(void* block) {
    uint32_t next;
    if (!block || is_sentinel(block)) {
        return 0;
    }
    next = hdr(block)->strong - 1;
    hdr(block)->strong = next;
    return next == 0;
}

void yux_rc_drop_block(void* block) {
    uint32_t next;
    if (!block) {
        return;
    }
    next = hdr(block)->weak - 1;
    hdr(block)->weak = next;
    if (next == 0) {
        yuxrt_free(block);
        __yux_rc_block_count -= 1;
    }
}

void yux_rc_release(void* block) {
    if (yux_rc_release_need_dtor(block)) {
        yux_rc_drop_block(block);
    }
}

void yux_rc_retain_payload(void* payload) {
    yux_rc_retain(yux_rc_block(payload));
}

void yux_rc_release_payload(void* payload) {
    yux_rc_release(yux_rc_block(payload));
}

int yux_rc_is_sentinel(const void* block) {
    if (!block) {
        return 0;
    }
    return is_sentinel(block);
}

uint32_t yux_rc_strong(const void* block) {
    if (!block) {
        return 0;
    }
    return hdr((void*)block)->strong;
}
