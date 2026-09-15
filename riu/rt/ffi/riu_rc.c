// riurt — Rc Block 实现（与 codegen 共用；不要在 IR 里再写一套计数协议）
#include "ffi/riu_rc.h"
#include "mem/mem.h"

int64_t __riu_rc_block_count = 0;

static riu_rc_header* hdr(void* block) {
    return (riu_rc_header*)block;
}

static int is_sentinel(const void* block) {
    return hdr((void*)block)->strong == RIU_RC_SENTINEL;
}

void* riu_rc_alloc(size_t payload_size) {
    void* block = riurt_alloc(RIU_RC_HEADER_SIZE + payload_size);
    hdr(block)->strong = 1;
    hdr(block)->weak = 1;
    __riu_rc_block_count += 1;
    return block;
}

void* riu_rc_payload(void* block) {
    if (!block) {
        return 0;
    }
    return (char*)block + RIU_RC_HEADER_SIZE;
}

void* riu_rc_block(void* payload) {
    if (!payload) {
        return 0;
    }
    return (char*)payload - RIU_RC_HEADER_SIZE;
}

void riu_rc_retain(void* block) {
    if (!block || is_sentinel(block)) {
        return;
    }
    hdr(block)->strong += 1;
}

int riu_rc_release_need_dtor(void* block) {
    uint32_t next;
    if (!block || is_sentinel(block)) {
        return 0;
    }
    next = hdr(block)->strong - 1;
    hdr(block)->strong = next;
    return next == 0;
}

void riu_rc_drop_block(void* block) {
    uint32_t next;
    if (!block) {
        return;
    }
    next = hdr(block)->weak - 1;
    hdr(block)->weak = next;
    if (next == 0) {
        riurt_free(block);
        __riu_rc_block_count -= 1;
    }
}

void riu_rc_release(void* block) {
    if (riu_rc_release_need_dtor(block)) {
        riu_rc_drop_block(block);
    }
}

void riu_rc_retain_payload(void* payload) {
    riu_rc_retain(riu_rc_block(payload));
}

void riu_rc_release_payload(void* payload) {
    riu_rc_release(riu_rc_block(payload));
}

int riu_rc_is_sentinel(const void* block) {
    if (!block) {
        return 0;
    }
    return is_sentinel(block);
}

uint32_t riu_rc_strong(const void* block) {
    if (!block) {
        return 0;
    }
    return hdr((void*)block)->strong;
}
