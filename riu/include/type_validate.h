// Copyright (c) 2025-2026. Yin-Jinlong@github
// MPL-2.0

#pragma once

#include "type_info.h"

void validateNoNestedHeap(const TypeInfo& t, int line, int col);
void validateNoDynInRcWeak(const TypeInfo& t, int line, int col);
void validateRcContainerBans(const TypeInfo& t, int line, int col);
[[nodiscard]] bool typeHoldsBorrowedValue(const TypeInfo& t);
void validateReturnTypeBorrowPolicy(const TypeInfo& t, int line, int col);
void validateTypeArgRefPolicy(const TypeInfo& t, int line, int col, bool allowDynBorrow);
void validateOwnedTypeArgs(const string& host, const vector<TypeInfo>& typeArgs, int line, int col);
