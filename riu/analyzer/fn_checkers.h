// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_FN_CHECKERS_H
#define RIU_LANG_FN_CHECKERS_H

class FileNode;

// SemaPass::run 之后、codegen 之前由 PassManager 调用（addAnalysisPasses）。
// 遍历本文件自由函数、struct 方法与析构：borrow + const-mut 单趟 walk，
// `#NoReturn` 按需另走。跳过 #Builtin。仍是 riu_analyzer，不是新库。
void runFnCheckers(FileNode* file);

#endif // RIU_LANG_FN_CHECKERS_H
