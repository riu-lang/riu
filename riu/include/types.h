// Copyright (c) 2025-2026. Yin-Jinlong@github
// MPL-2.0

#pragma once

// 分层头：改 type_validate.h / type_info.cpp 时不牵连全部 TU。
// 需要 validate* 的站点单独 `#include "type_validate.h"`。
#include "primitives.h"
#include "riu_error.h"
#include "source_location.h"
#include "token.h"
#include "type_info.h"
