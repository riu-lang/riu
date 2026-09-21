// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 模块声明文件（.ud）
//
// 二进制接口 + 泛型体 / spec 默认体 / 全局 init 的源文本 skeleton。
// 失效键：kFormatVersion + 源码 FNV-1a-64。重编 riu.exe 不使 .ud 作废。
// v2：fn header 与 TypeInfo 写入 fallibleErr（`T ! E`），否则跨模块调用 mangle 丢 `!E`。
// v3：fn header 边界从裸名改为 SpecRef（基名 + 类型实参），#15 泛型 spec。
// v4：enum 二进制接口写入 typeParams（`enum E<T>`）；lookup 仍按裸名。
// v5：实例字段写入 #Align(N)（0 = 无）。
// v6：typeParam 默认类型与 typeParams 平行写入（#21）。

#ifndef RIU_LANG_MOD_DECL_H
#define RIU_LANG_MOD_DECL_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Riu;
class FileNode;
class Node;

namespace mod_decl {

inline constexpr uint32_t kFormatVersion = 6;

// 重建节点的所有者，生命周期跟 Riu。
class NodeOwner {
public:
    NodeOwner() = default;
    NodeOwner(const NodeOwner&) = delete;
    NodeOwner& operator=(const NodeOwner&) = delete;
    ~NodeOwner();

    template <typename T, typename... Args>
    T* make(Args&&... args) {
        auto* n = new T(std::forward<Args>(args)...);
        _nodes.push_back(n);
        return n;
    }

private:
    std::vector<Node*> _nodes;
};

// `<buildDir>/<src 相对 projectRoot 去掉扩展名>.ud`
std::string pathFor(const std::string& projectRoot, const std::string& buildDir, const std::string& srcAbs);

uint64_t sourceHash(const std::string& srcAbs);

// parse 后写出。写失败静默（只读目录等）。
void write(FileNode* file, const std::string& srcAbs, const std::string& declPath);

// 命中且版本/hash 匹配则重建并登记到 riu；否则返回 nullptr（调用方 parse + write）。
FileNode* tryLoad(Riu& riu, const std::string& declPath, const std::string& srcAbs, const std::string& moduleName);

} // namespace mod_decl

#endif
