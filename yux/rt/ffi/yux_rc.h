// yuxrt — Rc Block C API（D2）
// C 操作 **yux 仍持有** 的 Rc：读 payload、extra retain/release。
// 不从 Ptr 重建 yux 类型；extern 仍不得返回 Rc。
#ifndef YUX_RC_H
#define YUX_RC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YUX_RC_HEADER_SIZE 8u
#define YUX_RC_SENTINEL 0xFFFFFFFFu

typedef struct yux_rc_header {
    uint32_t strong; /* offset 0 */
    uint32_t weak;   /* offset 4 */
} yux_rc_header;

/* 泄漏计数：每次真正 alloc Block +1，真正 free -1。rc_leak_count() 读此符号。 */
extern int64_t __yux_rc_block_count;

/* 分配 RC 头 + payload；strong=1, weak=1。返回 block（不是 payload）。 */
void* yux_rc_alloc(size_t payload_size);

/* payload = block + 8。ptr_of(Rc) / extern 自动转 Ptr 给 C 的就是这个。 */
void* yux_rc_payload(void* block);
/* payload - 8。指针算术，不是从 Ptr 重建 Rc<T>。 */
void* yux_rc_block(void* payload);

void yux_rc_retain(void* block);
/* 平凡 T：strong--，归零则 drop。不跑 payload 析构。 */
void yux_rc_release(void* block);

void yux_rc_retain_payload(void* payload);
void yux_rc_release_payload(void* payload);

/*
 * 编译器 typed release 用：
 * 非 0 → strong 刚归零，调用方析构 payload 后再 yux_rc_drop_block。
 * 0 → null / 哨兵 / strong 仍 >0，已处理完。
 */
int yux_rc_release_need_dtor(void* block);
/* weak--；weak==0 时 free block 并 --leak 计数。 */
void yux_rc_drop_block(void* block);

int yux_rc_is_sentinel(const void* block);
uint32_t yux_rc_strong(const void* block);

#ifdef __cplusplus
}
#endif

#endif /* YUX_RC_H */
