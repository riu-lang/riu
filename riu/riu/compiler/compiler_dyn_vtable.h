// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// Compiler Dyn 子系统：per-(U, D) vtable 与内置类型方法 thunk。
// 仅由 compiler.h 在 class Compiler 体内、定义 RIU_COMPILER_MEMBERS 后 include。

#ifdef RIU_COMPILER_MEMBERS
// clang-format off

    // Phase 3a：为 (concreteType U, specQualified D) 获取或合成 vtable 全局
    // 符号：`__riu_vtable.<U全限定>.<D全限定>`，linkonce_odr。
    // 布局：i8* 数组，长度 = 1 + D.signatures().size()
    //   - 槽 0：U 的析构函数指针；U 无需析构 → null
    //   - 槽 1..N：U 实现 D 各方法的 fn ptr（按 D 声明序），跨文件 impl 块定位
    // 调用方负责 U 满足 D（boundSatisfied 已在构造 / 边界匹配时校验）。
    llvm::GlobalVariable* getOrEmitDynVTable(const TypeInfo& concreteType, const std::string& specQualified,
                                             class SpecDeclNode* draft);

    // Phase 3d 配套: 为「内置类型 U 在 Dyn<D> 下的方法 m」生成 (ptr) → (by-value) 适配 thunk.
    // 用于解决 Dyn 调用约定统一为 (ptr receiver, ...) 但 SDK 内置类型方法实际签名是
    // (<U> by-value, ...) 的 ABI 不匹配。thunk 仅 load 一次 receiver, tail-call 真实 SDK 函数。
    // 仅在 vtable 槽内调用; 普通直接方法调用走 compileStructMethodCall 的 by-value 分支.
    llvm::Function* getOrEmitDynPrimitiveThunk(const TypeInfo& concreteType, const std::string& specQualified,
                                               class FnHeaderNode* sig, const std::string& sdkMangled);
// clang-format on
#endif
