// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_VALIDATE_BUILTIN_ANNOS_H
#define RIU_LANG_VALIDATE_BUILTIN_ANNOS_H

class ConstEvaluator;
class FileNode;

namespace sema {

void validateBuiltinAnnos(FileNode* file, ConstEvaluator& cvalEv);

} // namespace sema

#endif // RIU_LANG_VALIDATE_BUILTIN_ANNOS_H
