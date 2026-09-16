// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_PASS_H
#define RIU_LANG_PASS_H

#include <memory>
#include <vector>

class FileNode;
class Riu;

// 薄 Pass：对已构建的 AST 跑一次。parse / build 不进表。
// 不要复制 LLVM PassManager 的 AnalysisUsage / PassRegistry / 注册宏。
class Pass {
public:
    virtual ~Pass() = default;
    [[nodiscard]] virtual const char* name() const { return "pass"; }
    virtual void run(FileNode* file, Riu* riu) = 0;
};

class PassManager {
public:
    void addPass(std::unique_ptr<Pass> pass);
    void run(FileNode* file, Riu* riu);

private:
    std::vector<std::unique_ptr<Pass>> _passes;
};

// 分析表：Sema → borrow / const-mut / #NoReturn。
// 新分析 = 新 .cpp + 这里一行 addPass。codegen 由 Compiler 再挂，不进本表。
void addAnalysisPasses(PassManager& pm);

inline void runAnalysisPasses(FileNode* file, Riu* riu) {
    PassManager pm;
    addAnalysisPasses(pm);
    pm.run(file, riu);
}

#endif // RIU_LANG_PASS_H
