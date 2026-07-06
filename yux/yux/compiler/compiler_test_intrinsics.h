// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 测试断言内建函数共享常量
//
// 用于 yux/yux/compiler/compiler_test_intrinsics.cpp（IR 合成端）与
// yux/yux/main.cpp 的 sehExceptionName（SEH 显示端）共享 ASSERT_FAILED 异常码。
// 详见 docs/spec/11-编译期注解.md §11.3.5。

#ifndef YUX_LANG_COMPILER_TEST_INTRINSICS_H
#define YUX_LANG_COMPILER_TEST_INTRINSICS_H

namespace test_intrinsics {

// 断言失败用的自定义 SEH 异常码：客户位段 0xE + "FA17ED" 字面
// yux test 的 SEH wrapper 据此码翻译为 "ASSERT_FAILED"
constexpr unsigned long ASSERT_FAILED_CODE = 0xE0FA17EDu;

}

#endif //YUX_LANG_COMPILER_TEST_INTRINSICS_H
