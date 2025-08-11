// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/3/30.
//

#ifndef YUX_LANG_STD_H
#define YUX_LANG_STD_H
#include <stdint.h>

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

extern "C" {
extern void yux_main();
int mainStartup();

void yux_print_i8(i8 n);
void yux_print_i16(i16 n);
void yux_print_i32(i32 n);
void yux_print_i64(i64 n);
void yux_print_u8(u8 n);
void yux_print_u16(u16 n);
void yux_print_u32(u32 n);
void yux_print_u64(u64 n);
void yux_print_f32(float n);
void yux_print_f64(double n);
void yux_print_bool(bool b);

void yux_println_i8(i8 n);
void yux_println_i16(i16 n);
void yux_println_i32(i32 n);
void yux_println_i64(i64 n);
void yux_println_u8(u8 n);
void yux_println_u16(u16 n);
void yux_println_u32(u32 n);
void yux_println_u64(u64 n);
void yux_println_f32(float n);
void yux_println_f64(double n);
void yux_println_bool(bool b);
}


#endif //YUX_LANG_STD_H
