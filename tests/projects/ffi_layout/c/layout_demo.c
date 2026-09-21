#include <stdint.h>

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t flags;
    uint16_t pad;
    uint32_t size;
} FileHdr;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    uint8_t a;
    uint32_t b;
} Packed5;
#pragma pack(pop)

typedef struct {
    int64_t QuadPart;
} LARGE_INTEGER;

typedef struct __declspec(align(16)) {
    int32_t x;
} Align16;

uint32_t ffi_hdr_magic(FileHdr h) {
    return h.magic;
}

int32_t ffi_packed5_b(Packed5 p) {
    return (int32_t)p.b;
}

int64_t ffi_li_quad(LARGE_INTEGER li) {
    return li.QuadPart;
}

int32_t ffi_align16_x(Align16 a) {
    return a.x;
}
