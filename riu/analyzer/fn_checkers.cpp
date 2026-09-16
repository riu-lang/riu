// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "fn_checkers.h"

#include "borrow_checker.h"
#include "const_mut_checker.h"
#include "flow_terminate_checker.h"

#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/struct_node.h"

namespace {

bool skipFn(FnNode* fn) {
    return !fn || !fn->header() || fn->header()->hasAnno("Builtin");
}

void checkOne(FnNode* fn, const std::string& selfStructName) {
    if (skipFn(fn)) return;
    checkBorrows(fn, selfStructName);
    checkConstMut(fn);
    checkFlowTerminate(fn);
}

} // namespace

void runFnCheckers(FileNode* file) {
    if (!file) return;
    for (auto* fn : file->getFunctions()) {
        checkOne(fn, "");
    }
    for (auto* impl : file->getStructImpls()) {
        if (!impl) continue;
        const std::string& name = impl->structName();
        for (auto* m : impl->methods()) {
            checkOne(m, name);
        }
        if (impl->hasDestructor()) checkOne(impl->destructor(), name);
    }
}
