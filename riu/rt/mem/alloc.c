// riurt — 内存分配实现
#include "mem.h"

// 当前直接透传到平台层。
// 后续可在此层加：
//   - 全局分配计数（leak 检测）
//   - 对齐分配（riurt_aligned_alloc）
//   - 分配钩子（调试/asan 风格）

void* riurt_alloc(size_t size) {
    return riurt_plat_alloc(size);
}

void* riurt_alloc_zeroed(size_t size) {
    return riurt_plat_alloc_zeroed(size);
}

void* riurt_realloc(void* ptr, size_t new_size) {
    return riurt_plat_realloc(ptr, new_size);
}

void riurt_free(void* ptr) {
    riurt_plat_free(ptr);
}
