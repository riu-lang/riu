#include "riu_ffi.h"
#include "ffi/riu_nullable.h"
#include "ffi/riu_rc.h"
#include "ffi/riu_string.h"

#include <stdint.h>

typedef struct {
    int32_t x;
    int32_t y;
} Point;

typedef struct {
    int32_t a;
    int32_t b;
    int32_t c;
} Triple;

int32_t ffi_add(int32_t a, int32_t b) {
    return a + b;
}

_Bool ffi_gt(int32_t a, int32_t b) {
    return a > b;
}

Point ffi_make_point(int32_t x, int32_t y) {
    Point p;
    p.x = x;
    p.y = y;
    return p;
}

int32_t ffi_point_sum(Point p) {
    return p.x + p.y;
}

void ffi_point_bump(Point* p) {
    p->x += 1;
    p->y += 1;
}

Triple ffi_make_triple(int32_t a, int32_t b, int32_t c) {
    Triple t;
    t.a = a;
    t.b = b;
    t.c = c;
    return t;
}

int32_t ffi_triple_sum(Triple t) {
    return t.a + t.b + t.c;
}

static int32_t g_ids[8];
static int g_used[8];

riu_ptr ffi_handle_open(int32_t id) {
    int i = 0;
    while (i < 8) {
        if (!g_used[i]) {
            g_used[i] = 1;
            g_ids[i] = id;
            return &g_ids[i];
        }
        i += 1;
    }
    return 0;
}

int32_t ffi_handle_id(riu_ptr p) {
    if (!p) {
        return -1;
    }
    return *(int32_t*)p;
}

void ffi_handle_close(riu_ptr p) {
    int i = 0;
    while (i < 8) {
        if (g_used[i] && &g_ids[i] == p) {
            g_used[i] = 0;
            return;
        }
        i += 1;
    }
}

void ffi_write_i32(int32_t* p, int32_t v) {
    *p = v;
}

int32_t ffi_nlen(const char* s) {
    int32_t n = 0;
    if (!s) {
        return 0;
    }
    while (s[n]) {
        n += 1;
    }
    return n;
}

static const char k_hello[] = "hi";

riu_ptr ffi_hello(void) {
    return (riu_ptr)k_hello;
}

/* D2：payload = ptr_of(Rc<i32>)。riu 仍持有，只读写，不 free。 */
int32_t ffi_rc_i32_get(void* payload) {
    if (!payload) {
        return 0;
    }
    return *(int32_t*)payload;
}

void ffi_rc_i32_set(void* payload, int32_t v) {
    if (payload) {
        *(int32_t*)payload = v;
    }
}

int32_t ffi_rc_i32_peek_retain(void* payload) {
    int32_t v;
    riu_rc_retain_payload(payload);
    v = ffi_rc_i32_get(payload);
    riu_rc_release_payload(payload); /* extra ref；riu 仍持有 */
    return v;
}

/* n = i32?& → Ptr */
int32_t ffi_nullable_i32_or(void* n, int32_t fallback) {
    if (!riu_nullable_has(n)) {
        return fallback;
    }
    return *(int32_t*)riu_nullable_value(n, 4);
}

/* p = String& → Ptr（指向 {handle}） */
int32_t ffi_string_len(void* string_struct) {
    riu_string_view v = riu_string_as_view(string_struct);
    return (int32_t)v.len;
}

int32_t ffi_string_first(void* string_struct) {
    riu_string_view v = riu_string_as_view(string_struct);
    if (!v.data || v.len == 0) {
        return 0;
    }
    return (int32_t)v.data[0];
}
