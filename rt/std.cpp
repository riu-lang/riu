// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/3/30.
//

#include "std.h"

#include <windows.h>

static char buf[64];

static size_t str_len(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static void mem_copy(void* dest, const void* src, size_t count) {
    char* d = (char*)dest;
    const char* s = (const char*)src;
    while (count--) {
        *d++ = *s++;
    }
}

static void mem_move(void* dest, const void* src, size_t count) {
    char* d = (char*)dest;
    const char* s = (const char*)src;
    if (d < s) {
        while (count--) *d++ = *s++;
    } else {
        d += count;
        s += count;
        while (count--) *--d = *--s;
    }
}

static char* i64_to_str(int64_t value, char* buffer, int base) {
    if (base < 2 || base > 16) return buffer;
    char* p = buffer;
    int64_t v = value;
    if (v < 0) {
        *p++ = '-';
        v = -v;
    }
    char* start = p;
    do {
        int digit = v % base;
        *p++ = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
        v /= base;
    } while (v);
    *p = '\0';
    for (char *l = start, *r = p - 1; l < r; l++, r--) {
        char tmp = *l;
        *l = *r;
        *r = tmp;
    }
    return buffer;
}

static char* u64_to_str(uint64_t value, char* buffer, int base) {
    if (base < 2 || base > 16) return buffer;
    char* p = buffer;
    uint64_t v = value;
    char* start = p;
    do {
        int digit = v % base;
        *p++ = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
        v /= base;
    } while (v);
    *p = '\0';
    for (char *l = start, *r = p - 1; l < r; l++, r--) {
        char tmp = *l;
        *l = *r;
        *r = tmp;
    }
    return buffer;
}

static void print_str(const char* s) {
    DWORD written;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    WriteFile(h, s, (DWORD)str_len(s), &written, NULL);
}

static void print_newline() {
    DWORD written;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    WriteFile(h, "\n", 1, &written, NULL);
}

void yux_print_i8(i8 n) {
    i64_to_str(n, buf, 10);
    print_str(buf);
}

void yux_print_i16(i16 n) {
    i64_to_str(n, buf, 10);
    print_str(buf);
}

void yux_print_i32(i32 n) {
    i64_to_str(n, buf, 10);
    print_str(buf);
}

void yux_print_i64(i64 n) {
    i64_to_str(n, buf, 10);
    print_str(buf);
}

void yux_print_u8(u8 n) {
    u64_to_str(n, buf, 10);
    print_str(buf);
}

void yux_print_u16(u16 n) {
    u64_to_str(n, buf, 10);
    print_str(buf);
}

void yux_print_u32(u32 n) {
    u64_to_str(n, buf, 10);
    print_str(buf);
}

void yux_print_u64(u64 n) {
    u64_to_str(n, buf, 10);
    print_str(buf);
}

void yux_print_bool(bool b) {
    print_str(b ? "true" : "false");
}

void yux_print_f32(float n) {
    i32 ip = (i32)n;
    float fp = n - ip;
    if (n < 0 && ip == 0) print_str("-");
    i64_to_str(ip < 0 ? -ip : ip, buf, 10);
    print_str(buf);
    print_str(".");
    i64_to_str((i32)(fp * 1000000), buf, 10);
    int len = (int)str_len(buf);
    while (len < 6) {
        mem_move(buf + 1, buf, len + 1);
        buf[0] = '0';
        len++;
    }
    while (len > 1 && buf[len - 1] == '0') {
        buf[--len] = '\0';
    }
    print_str(buf);
}

void yux_print_f64(double n) {
    i64 ip = (i64)n;
    double fp = n - ip;
    if (n < 0 && ip == 0) print_str("-");
    i64_to_str(ip < 0 ? -ip : ip, buf, 10);
    print_str(buf);
    print_str(".");
    i64_to_str((i64)(fp * 1000000000000), buf, 10);
    int len = (int)str_len(buf);
    while (len < 12) {
        mem_move(buf + 1, buf, len + 1);
        buf[0] = '0';
        len++;
    }
    while (len > 1 && buf[len - 1] == '0') {
        buf[--len] = '\0';
    }
    print_str(buf);
}

void yux_println_i8(i8 n) {
    yux_print_i8(n);
    print_newline();
}

void yux_println_i16(i16 n) {
    yux_print_i16(n);
    print_newline();
}

void yux_println_i32(i32 n) {
    yux_print_i32(n);
    print_newline();
}

void yux_println_i64(i64 n) {
    yux_print_i64(n);
    print_newline();
}

void yux_println_u8(u8 n) {
    yux_print_u8(n);
    print_newline();
}

void yux_println_u16(u16 n) {
    yux_print_u16(n);
    print_newline();
}

void yux_println_u32(u32 n) {
    yux_print_u32(n);
    print_newline();
}

void yux_println_u64(u64 n) {
    yux_print_u64(n);
    print_newline();
}

void yux_println_f32(float n) {
    yux_print_f32(n);
    print_newline();
}

void yux_println_f64(double n) {
    yux_print_f64(n);
    print_newline();
}

void yux_println_bool(bool b) {
    yux_print_bool(b);
    print_newline();
}

int mainStartup() {
    yux_main();
    return 0;
}
