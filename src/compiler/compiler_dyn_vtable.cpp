// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// Dyn<D> / Dyn<D&> 运行时多态：per-(U, D) vtable 生成
//
// 本文件实现 DRAFT-dyn-draft Phase 3a：为每个具体类型 U 与 draft D 的组合
// 合成一份静态 vtable，作为 fat-ptr `{ vtable, data }` 的第一槽。
//
// vtable 布局（i8* 数组）：
//   [0]   U 的析构函数指针（fn(ptr) void）；U 无字段需析构 → null
//   [1..] D 的方法实现，按 D 声明序，指向 U 在 `Type:D { ... }` 或
//         普通方法块 `Type { ... }` 中提供的具体 FnNode 对应 LLVM Function
//
// 符号：`__yux_vtable_<U_module>_<U_struct>__<D_qualified>`，
//       linkonce_odr，允许多 TU 共享去重。
//
// 调用方先经 DraftImplChecker 的 boundSatisfied / E1133 校验，
// 这里假定 U 满足 D 的全部签名；找不到方法实现视为编译器内部一致性失败。

#include "compiler.h"
#include "ast/mangler.h"
#include "ast/yux.h"
#include "analyzer/draft_impl_checker.h"
#include "analyzer/draft_registry.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>

namespace {

// 把 draft 完全限定名（"yux.core.ToString" / "Greet"）转成符号安全形式：
// '.' → '_'，其它 ASCII 标识符字符直通；非常规字符按 '_' 兜底。
std::string sanitizeForSymbol(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '.') {
            out += '_';
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_') {
            out += c;
        } else {
            out += '_';
        }
    }
    return out;
}

// 找 U 在 Yux 全局视野下的所属模块名（与 getDestructorFunction 同一规则的简化版本）。
// 普通 struct → 声明该 struct 的文件模块；SDK 内置 → sdkFile 模块；未登记 → 空串。
std::string findStructOwnerModule(Yux* yux, const std::string& structName) {
    if (!yux) return {};
    auto* sdk = yux->sdkFile();
    if (sdk && sdk->getStructDecl(structName)) {
        return sdk->moduleName();
    }
    for (auto& f : yux->files()) {
        if (f->getStructDecl(structName)) {
            return f->moduleName();
        }
    }
    return {};
}

// 跨 SDK / 用户文件，遍历 U 的所有 StructImplNode（普通方法块 + draft 实现块）。
// 返回第一个名字匹配 methodName 的 FnNode + 该实现块所在文件的模块名
// （v1：DraftImplChecker 已确保签名等价，这里不再二次校验签名，仅按名取首条）。
// 优先匹配带 draftRefs 的实现块（显式 Type:D{}）；找不到再回落到普通方法块。
// 内置类型（i32 / bool / ...）的 ToString 等 impl 写在 SDK base.yux 里，
// 必须用 impl 所在文件的模块名（如 `yux.core`）才能拿到正确的链接符号。
struct ImplLookup {
    p<FnNode> method;
    std::string ownerModule;
};

ImplLookup findImplMethod(Yux* yux,
                         const std::string& structName,
                         const std::string& methodName) {
    if (!yux) return {};

    auto scanFile = [&](FileNode* file, bool preferDraftImpl) -> ImplLookup {
        if (!file) return {};
        for (auto& impl : file->getStructImpls()) {
            if (impl->structName() != structName) continue;
            bool isDraftImpl = !impl->draftRefs().empty();
            if (preferDraftImpl != isDraftImpl) continue;
            for (auto& m : impl->methods()) {
                if (m->header()->name().getText() == methodName) {
                    return {m, file->moduleName()};
                }
            }
        }
        return {};
    };

    // 两轮：先找 `Type:D { ... }` 块，再回落到 `Type { ... }` 普通方法块。
    for (bool preferDraft : {true, false}) {
        if (auto sdk = yux->sdkFile()) {
            if (auto r = scanFile(sdk, preferDraft); r.method) return r;
        }
        for (auto& f : yux->files()) {
            if (auto r = scanFile(f, preferDraft); r.method) return r;
        }
    }
    return {};
}

} // namespace

llvm::GlobalVariable* Compiler::getOrEmitDynVTable(
    const TypeInfo& concreteType,
    const std::string& draftQualified,
    DraftDeclNode* draft) {
    if (!draft) return nullptr;

    const std::string& uStruct = concreteType.name;
    std::string uModule = findStructOwnerModule(_yux, uStruct);

    // 符号名：__yux_vtable_<uMod>_<uStruct>__<dQualified>
    // uMod 为空（罕见，类型未在已加载模块中找到）时退化为仅 struct 名。
    std::string symName = "__yux_vtable_";
    if (!uModule.empty()) {
        symName += sanitizeForSymbol(uModule);
        symName += "_";
    }
    symName += sanitizeForSymbol(uStruct);
    symName += "__";
    symName += sanitizeForSymbol(draftQualified);

    if (auto* existing = _module->getNamedGlobal(symName)) {
        return existing;
    }

    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto nullPtr = llvm::ConstantPointerNull::get(ptrTy);

    const auto& sigs = draft->signatures();
    const size_t slotCount = 1 + sigs.size();
    auto arrTy = llvm::ArrayType::get(ptrTy, slotCount);

    std::vector<llvm::Constant*> slots;
    slots.reserve(slotCount);

    // 槽 0：U 的析构函数指针
    // 复用现有 struct dtor 入口；trivial 类型 → null（release 路径据此跳过 dispatch）
    llvm::Constant* dtorSlot = nullPtr;
    if (structNeedsDestructor(uStruct)) {
        auto* dtorFn = getDestructorFunction(uStruct);
        if (dtorFn) {
            dtorSlot = dtorFn;
        }
    }
    slots.push_back(dtorSlot);

    // 槽 1..N：D 每个方法 → U 的具体实现 fn ptr
    for (auto& sig : sigs) {
        const std::string methodName = sig->name().getText();
        auto impl = findImplMethod(_yux, uStruct, methodName);
        auto implMethod = impl.method;
        llvm::Constant* slot = nullPtr;
        if (implMethod) {
            // 收集 U 的具体形参类型（从 U 自己的 impl 方法 header 取，
            // 而非 D 的 sig，避免泛型 / 自身类型透传带来的不一致）。
            std::vector<TypeInfo> paramTypes;
            for (auto& param : implMethod->header()->params()) {
                paramTypes.push_back(param->type()->getType());
            }
            bool isPriv = !methodName.empty() && methodName[0] == '_';
            // 用 impl 所在文件的模块名（内置类型的 impl 在 SDK 模块里，
            // findStructOwnerModule 拿到空 uModule 时会错指）。
            std::string mangled = Mangler::method(impl.ownerModule, uStruct, methodName,
                                                  paramTypes, isPriv);
            auto* fn = _module->getFunction(mangled);
            if (!fn) {
                // 跨模块引用：方法定义在 U 的 owner 模块，本模块仅做 forward declare。
                // 签名按 D 自己的 sig 还原（对象安全确保 D 的 sig 不含 Self / 自身名，
                // 因此 D.sig 形参/返回类型与 U.impl 一致）：
                //   (ptr receiver, P1, ..., Pn) -> R
                std::vector<llvm::Type*> llvmParamTypes;
                llvmParamTypes.push_back(ptrTy);  // receiver
                for (auto& p : sig->params()) {
                    if (!p || !p->type()) continue;
                    auto pt = p->type()->getType();
                    if (pt.isPtr() || pt.isRef()) {
                        llvmParamTypes.push_back(ptrTy);
                    } else {
                        llvmParamTypes.push_back(getLLVMType(pt));
                    }
                }
                llvm::Type* llvmRetType = _builder.getVoidTy();
                if (sig->retType()) {
                    auto rt = sig->retType()->getType();
                    if (!rt.empty()) llvmRetType = getLLVMType(rt);
                }
                auto fnTy = llvm::FunctionType::get(llvmRetType, llvmParamTypes, false);
                fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                            mangled, _module);
            }
            slot = fn;
        }
        // TODO: implMethod 找不到说明 DraftImplChecker 未拦截的内部不一致；
        // 当前留 null 兜底，调用站点（Phase 3d）会以"加载到 null 函数指针"指示问题。
        slots.push_back(slot);
    }

    auto* init = llvm::ConstantArray::get(arrTy, slots);

    auto* gv = new llvm::GlobalVariable(
        *_module,
        arrTy,
        /*isConstant=*/true,
        llvm::GlobalValue::LinkOnceODRLinkage,
        init,
        symName);
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    return gv;
}
