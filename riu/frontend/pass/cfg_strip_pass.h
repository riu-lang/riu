// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_CFG_STRIP_PASS_H
#define RIU_LANG_CFG_STRIP_PASS_H

#include "pass/pass.h"

// Sema 前按 `#If` 摘假分支（Rust `#[cfg]`）。句法错误已在 parse 期报。
class CfgStripPass final : public Pass {
public:
    [[nodiscard]] const char* name() const override { return "cfg-strip"; }
    void run(FileNode* file, Riu* riu) override;
};

#endif // RIU_LANG_CFG_STRIP_PASS_H
