// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// Compiler 析构子系统：块级帧栈、默认 dtor、Array/Rc/enum 释放。
// 仅由 compiler.h 在 class Compiler 体内、定义 RIU_COMPILER_MEMBERS 后 include。

#ifdef RIU_COMPILER_MEMBERS
// clang-format off

    // ==================== 作用域管理（§5.7 块级帧栈）====================
    // 每帧 = 一个 statementBlock / fn 顶层 / loop-init 内需析构的局部变量。
    // 块出口 / 每轮 loop 尾 pop 当前帧；break  unwind 到 loop 进入前深度；ret 清空全部帧。
    struct ScopeVar {
        string name;
        TypeInfo type;
        llvm::Value* prevPtr = nullptr; // 同名遮蔽时保存外层 alloca，出块恢复
        bool needsDtor = false;
    };
    vector<vector<ScopeVar>> _scopeFrames;
    void pushScopeFrame();
    void popScopeFrameAndDestroy();          // 支配性块出口：发射析构并 pop
    void popScopeFrameNoDestroy();           // 仅 pop（loop 编译结束后清 init 帧）
    void emitDestructorsAbove(size_t depth); // 仅发射析构 IR，不 pop（break/ret）
    void unwindScopeFramesTo(size_t depth);  // 仅 pop 到 depth，不发射析构
    [[nodiscard]] size_t scopeFrameDepth() const { return _scopeFrames.size(); }
    // 登记局部：写入 _localVarPtrs，并入当前帧（支持同名遮蔽恢复）
    void registerLocalVar(const string& name, llvm::Value* alloca, const TypeInfo& type);
    void pushScopeVar(const string& name, const TypeInfo& type, llvm::Value* prevPtr, bool needsDtor);
    void eraseScopeVar(const string& name); // 从所有帧摘除（move-out / ret 移出）
    // 局部变量符号：优先 from 的词法 scope（块作用域），再回退 FnNode（参数 / 旧路径）
    SymbolInfo* lookupVarSymbol(const string& name, Node* from);

    // ==================== 析构函数 ====================
    void callDestructor(const string& varName, const TypeInfo& varType);        // 调用单个变量的析构函数
    void callDestructorsForScope();                                             // 调用当前作用域所有变量的析构函数
    void callFieldDestructor(llvm::Value* structPtr, const string& structName); // 调用结构体字段的析构函数
    void generateDefaultDestructor(const string& structName);                   // 生成默认析构函数
    void releaseArrayElements(llvm::Value* arrayPtr, const TypeInfo& arrayType, llvm::Value* beginIndex,
                              llvm::Value* endIndex);                              // 逆序析构 Array 的 [begin, end)
    void releaseArrayAtPtr(llvm::Value* arrayPtr, const TypeInfo& arrayType);      // 析构有效元素并释放缓冲区
    llvm::Function* getOrCreateArrayDestructorFunction(const TypeInfo& arrayType); // Rc<Array<T>> 共用析构入口
    bool typeNeedsDestructor(const TypeInfo& type);                                // 检查类型是否需要析构
    bool structNeedsDestructor(const string& structName); // 检查结构体是否需要析构（decl / 实例 key）
    bool structNeedsDestructor(const TypeInfo& type);     // 泛型用 baseStructName 仅查 decl，实例身份走 mangle
    // Phase 3d: 释放槽位（变量 / 字段 / 元素地址）当前持有的 RC 值
    // Rc/Array/Weak: load handle 后调对应 release；含 RC 字段 struct: 调其析构（字段逆序 release）
    // 内置 / 引用 / 指针 / 平凡 struct: no-op
    void releaseAtPtr(llvm::Value* slotPtr, const TypeInfo& type);
    // Phase B-2: 获取或创建 Rc<T> 的 typed release 函数
    // 若 rcType 内层 T 无需析构则返回 generic _box_release；
    // 否则生成特化版 _box_release_T（strong==0 时先调 T::~() 再走 weak/free）
    llvm::Function* getOrCreateRcTypedReleaseFn(const TypeInfo& rcType);
    // Phase B-1: #NoCopy / move 辅助
    [[nodiscard]] bool isNoCopyType(const TypeInfo& type) const; // 查 struct decl 的 #NoCopy 注解
    bool enumNeedsDestructor(const string& enumName);            // Phase 5: 任一 variant payload 需析构则枚举需析构
    bool enumNeedsDestructor(const TypeInfo& type);              // 非 Normal 直接 false；identity 不走裸名重建
    bool enumDeclNeedsDestructor(EnumDeclNode* decl);            // Phase 5: 同上，按声明节点
    llvm::Function* getEnumDestructorFunction(const string& enumName,
                                              const string& ownerModuleHint = {}); // Phase 5: 获取或创建 enum dtor
    void generateEnumDestructor(EnumDeclNode* decl, FileNode* owner); // Phase 5: 合成 __enum_drop_<E>(p*) 实现
    void compileEnumDtors(); // Phase 5: 在主流水线中为本文件 enum 生成 dtor 定义
// clang-format on
#endif
