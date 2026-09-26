// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_ANNO_CALL_H
#define RIU_LANG_ANNO_CALL_H

#include "types.h"

#include <optional>

class ExprNode;

struct AnnoArg {
    std::optional<string> field;
    ExprNode* expr = nullptr;
    // .ud 缓存单参文本；无 expr 时 getAnnoArg / writeAnnos 走此槽。
    string text;
};

struct AnnoCall {
    string name;
    vector<AnnoArg> args;
    int line = 0;
    int col = 0;
};

[[nodiscard]] string annoArgText(const AnnoArg& arg);
[[nodiscard]] string annoCallFirstArgText(const AnnoCall& call);

#endif // RIU_LANG_ANNO_CALL_H
