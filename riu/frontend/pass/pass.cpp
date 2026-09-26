// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// PassManager + 分析表。Sema / fn checkers 包成 Pass；新分析在 addAnalysisPasses 加一行。

#include "pass/pass.h"

#include "analyzer/fn_checkers.h"
#include "pass/cfg_strip_pass.h"
#include "sema/sema_pass.h"
#include "types.h"

namespace {

class SemaAnalysisPass final : public Pass {
public:
    [[nodiscard]] const char* name() const override { return "sema"; }
    void run(FileNode* file, Riu* riu) override { SemaPass(file, riu).run(); }
};

class FnCheckersPass final : public Pass {
public:
    [[nodiscard]] const char* name() const override { return "fn-checkers"; }
    void run(FileNode* file, Riu*) override { runFnCheckers(file); }
};

} // namespace

void PassManager::addPass(std::unique_ptr<Pass> pass) {
    if (pass) _passes.push_back(std::move(pass));
}

void PassManager::run(FileNode* file, Riu* riu) {
    for (auto& p : _passes) {
        DEBUG_LOG_VAL("Running pass", p->name());
        p->run(file, riu);
    }
}

void addAnalysisPasses(PassManager& pm) {
    pm.addPass(std::make_unique<CfgStripPass>());
    pm.addPass(std::make_unique<SemaAnalysisPass>());
    pm.addPass(std::make_unique<FnCheckersPass>());
}
