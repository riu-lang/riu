// Copyright (c) 2025-2026. Yin-Jinlong@github
// MPL-2.0

#include "type_validate.h"

#include "riu_error.h"

void validateNoNestedHeap(const TypeInfo& t, int line, int col) {
    if (t.isRc()) {
        if (auto e = t.rcElementType()) {
            if (e->isHeap()) {
                auto inner = e->heapElementType();
                throw RiuError(line, col, ErrorCode::E4025, std::string("Rc"), inner ? inner->name : std::string("?"));
            }
            validateNoNestedHeap(*e, line, col);
        }
        return;
    }
    if (t.isWeak()) {
        if (auto e = t.weakElementType()) {
            if (e->isHeap()) {
                auto inner = e->heapElementType();
                throw RiuError(line, col, ErrorCode::E4025, std::string("Weak"),
                               inner ? inner->name : std::string("?"));
            }
            validateNoNestedHeap(*e, line, col);
        }
        return;
    }
    if (t.isArrayGeneric()) {
        if (auto e = t.arrayGenericElementType()) {
            if (e->isHeap()) {
                auto inner = e->heapElementType();
                throw RiuError(line, col, ErrorCode::E4025, std::string("Array"),
                               inner ? inner->name : std::string("?"));
            }
            validateNoNestedHeap(*e, line, col);
        }
        return;
    }
    if (t.isHeap()) {
        if (auto e = t.heapElementType()) validateNoNestedHeap(*e, line, col);
        return;
    }
    for (const auto& g : t.genericArgs) {
        if (g) validateNoNestedHeap(*g, line, col);
    }
}

// E1132: Rc / Weak 禁止直接内嵌 Dyn（§12.9.3.2）。递归下钻，覆盖
// `Rc<Rc<Dyn<D>>>`。Array<Dyn<D>> 合法，不在此禁。sema 与 getLLVMType 共用。
void validateNoDynInRcWeak(const TypeInfo& t, int line, int col) {
    if (t.isRc()) {
        if (auto e = t.rcElementType()) {
            if (e->isDyn()) {
                throw RiuError(line, col, ErrorCode::E1132, std::string("Rc<") + e->getFullName() + ">");
            }
            validateNoDynInRcWeak(*e, line, col);
        }
        return;
    }
    if (t.isWeak()) {
        if (auto e = t.weakElementType()) {
            if (e->isDyn()) {
                throw RiuError(line, col, ErrorCode::E1132, std::string("Weak<") + e->getFullName() + ">");
            }
            validateNoDynInRcWeak(*e, line, col);
        }
        return;
    }
    if (t.isArrayGeneric()) {
        if (auto e = t.arrayGenericElementType()) validateNoDynInRcWeak(*e, line, col);
        return;
    }
    if (t.isHeap()) {
        if (auto e = t.heapElementType()) validateNoDynInRcWeak(*e, line, col);
        return;
    }
    for (const auto& g : t.genericArgs) {
        if (g) validateNoDynInRcWeak(*g, line, col);
    }
}

void validateRcContainerBans(const TypeInfo& t, int line, int col) {
    validateNoNestedHeap(t, line, col);
    validateNoDynInRcWeak(t, line, col);
}

// 裸 T& / Array<T&> / [T& * N] / 含它们的元组或 Nullable：持有位（字段 / 别名 /
// 全局 / enum payload）报 E4039。Function 是 owned fat-ptr，即使槽里有 T& 也不算持有。
bool typeHoldsBorrowedValue(const TypeInfo& t) {
    if (t.isFn()) return false;
    if (t.isRef()) return true;
    if (t.isArray() && t.elementType) return typeHoldsBorrowedValue(*t.elementType);
    if (t.isArrayGeneric()) {
        if (auto e = t.arrayGenericElementType()) return typeHoldsBorrowedValue(*e);
    }
    if (t.isTuple()) {
        for (const auto& e : t.tupleElements()) {
            if (e && typeHoldsBorrowedValue(*e)) return true;
        }
    }
    if (t.isNullable()) {
        if (auto inner = t.nullableInnerType()) return typeHoldsBorrowedValue(*inner);
    }
    return false;
}

// 返回类型：裸 T& 由 borrow checker 管；Array<T&> / [T& * N] / 含它们的元组不可作为返回值。
void validateReturnTypeBorrowPolicy(const TypeInfo& t, int line, int col) {
    if (t.isRef()) return;
    if (typeHoldsBorrowedValue(t)) {
        throw RiuError(line, col, ErrorCode::E4040)
            .withHint("返回单个 `T&`（方法 `$` 或静态 `$rodata`）；容器里的借用不能随返回值逃逸");
    }
}

// `<>` 内 T&：Function 形参 / 返回允许；Dyn<D&> 仅临时位；
// Array<T&> 仅临时位；Rc / Weak / Heap / 用户泛型的实参必须 owned（E4037）。
void validateTypeArgRefPolicy(const TypeInfo& t, int line, int col, bool allowDynBorrow) {
    if (t.isFn()) {
        for (const auto& p : t.fnParamTypes()) {
            if (p) validateTypeArgRefPolicy(*p, line, col, true);
        }
        if (auto ret = t.fnReturnType()) validateTypeArgRefPolicy(*ret, line, col, true);
        return;
    }
    if (!allowDynBorrow && typeHoldsBorrowedValue(t)) {
        throw RiuError(line, col, ErrorCode::E4039)
            .withHint("`T&` / `Array<T&>` / `[T& * N]` 只能出现在形参、局部 `let` 和返回类型；"
                      "`Function<…>` 里的 `T&` 槽是 owned fat-ptr，可作字段");
    }
    if (t.isDyn()) {
        if (t.isDynBorrow() && !allowDynBorrow) {
            throw RiuError(line, col, ErrorCode::E4038)
                .withHint("`Dyn<D&>` 只出现在形参 / 返回 / `let` 类型位；字段、别名和容器元素用 `Dyn<D>`");
        }
        if (auto spec = t.dynSpecType()) {
            TypeInfo inner = *spec;
            if (inner.isRef()) {
                if (auto peeled = inner.refElementType()) inner = *peeled;
            }
            validateTypeArgRefPolicy(inner, line, col, false);
        }
        return;
    }
    if (t.isRef()) {
        // 临时位的 `U&` 其 referent 仍按临时位查（`[Field& * N]&` 是局部 T&）。
        if (auto inner = t.refElementType()) validateTypeArgRefPolicy(*inner, line, col, allowDynBorrow);
        return;
    }
    if (t.isTuple()) {
        for (const auto& e : t.tupleElements()) {
            if (e) validateTypeArgRefPolicy(*e, line, col, allowDynBorrow);
        }
        return;
    }
    if (t.isArray()) {
        if (t.elementType) validateTypeArgRefPolicy(*t.elementType, line, col, allowDynBorrow);
        return;
    }

    const bool ownedSlots =
        t.isArrayGeneric() || t.isRc() || t.isWeak() || t.isHeap() || t.isNullable() || t.isGeneric() || t.isPtr();
    if (!ownedSlots) return;

    for (const auto& g : t.genericArgs) {
        if (!g) continue;
        if (g->isRef()) {
            // Array<T&> 临时位放行；持有位已由 typeHoldsBorrowedValue / E4039 拒。
            if (t.isArrayGeneric() && allowDynBorrow) {
                validateTypeArgRefPolicy(*g, line, col, true);
                continue;
            }
            throw RiuError(line, col, ErrorCode::E4037, t.name)
                .withHint("类型实参须为 owned（值类型 / 堆句柄 / Ptr）；借用写在形参上，如 `fn f<T>(x T&)`。"
                          "`Function` 形参和临时位的 `Dyn<D&>` / `Array<T&>` 可以写 `&`");
        }
        validateTypeArgRefPolicy(*g, line, col, false);
    }
}

// 用户泛型 fn / 泛型 struct turbofish：每个实参须 owned（Dyn:<D&> 走构造节点，不走这里）。
void validateOwnedTypeArgs(const string& host, const vector<TypeInfo>& typeArgs, int line, int col) {
    for (const auto& a : typeArgs) {
        if (a.isRef()) {
            throw RiuError(line, col, ErrorCode::E4037, host)
                .withHint("类型实参须为 owned（值类型 / 堆句柄 / Ptr）；借用写在形参上，如 `fn f<T>(x T&)`");
        }
        validateTypeArgRefPolicy(a, line, col, false);
    }
}
