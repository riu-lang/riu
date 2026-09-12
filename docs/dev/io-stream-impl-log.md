# `yux.io` 分层文件 API 实施日志

本文件记录 `Path`、`File`、`FileInfo`、`FileInputStream` 与 `FileOutputStream` 的实现决策。用户可观察契约以 `docs/spec/10-模块系统.md` §10.4.1.4 为准。

## 分层与所有权

- `Path` 只保存路径值，`File` 只保存 `Path`；两者构造时不访问文件系统，也不持有 OS 资源。
- `FileInfo` 是 `GetFileAttributesExW` 查询结果的值快照。文件大小按 Win32 高低 32 位组合为 `u64`；reparse point 的类别优先于 directory。
- 输入流和输出流各自独占一个 Win32 `HANDLE`，均标记 `#NoCopy`。工厂返回 fresh 值，调用方只能通过 `move` 转交已有流。
- `_handle` 是无注解的默认可变实例字段，供 `close()` 和析构置空；没有在字段上使用 `#Mut`。路径和快照字段用 `#Val` 阻止重新绑定。
- 下划线字段及 helper 依照模块可见性规则保持在 `yux.io` 内；公开 API 不暴露裸句柄。

## I/O 边界

- 路径只在 Win32 调用边界转 UTF-16；`read_to_string()` 和字符串写入统一使用 UTF-8。
- UTF-8 解码拒绝过长编码、surrogate 区间和大于 U+10FFFF 的码点。
- 读取 helper 不拥有句柄。`read(n)` 最多读 `n` 字节，EOF 返回空数组；`read_to_end()` 读取到 EOF。
- 写入 helper 以固定块循环，直到全部字节写完；系统成功但报告零字节写入时返回 `Other(0)`，避免死循环。
- `flush()` 显式调用 `FlushFileBuffers`。`close()` 先把字段置空，再报告 `CloseHandle` 错误，保证重复关闭不会再次使用同一句柄；析构只做 best-effort 关闭。

## 兼容层

- `read_file` / `write_file` / `append_file` 保留原签名，分别经 `Path` 与具体文件流实现。
- 既有路径、条目查询、目录和链接函数继续公开；对象方法与这些函数共享底层 helper。
- `File.list()` 返回含完整子路径的 `Array<File>`；旧 `list_dir()` 继续返回裸名字，避免静默改变已有调用方。

## 编译器配套修正

- 修复 fallible 泛型实例方法的错误类型挂载，避免 `Array<T>` 返回类型被重复套用泛型实参。
- 分支表达式结果的所有权归一化从仅 RC 扩展到所有需要析构的类型，使 `#NoCopy` 文件流能安全穿过 `try` / `if` / `match`。
- `String.to_string()` 显式 retain 底层 `Rc<Array<u32>>` 后转移到返回值，保证 `Path` 保存的是独立有效的字符串值。
- `yux-check` 批量测试新增 `require-sdk-modules` 指令，使依赖完整 `yux.io` 模块表的诊断用例可隔离运行。

## File/Path 缺口（2026-09-12）

- `Path` / `File` 补 `#Impl(ToString)`，插值与 `print` 可直接吃路径值。
- `Path.is_empty` / `Path.same_as`：后者绝对化后大小写不敏感，供链接工具判断「链接路径就是目标自己」。
- `File.is_dir` / `is_file` / `is_link` 对齐 facade（junction 的 `is_dir` 为真）；`info().kind` 仍把 reparse 记为 `Link`。
- `File.symlink_to` / `hardlink_to` / `junction_to`：`$` 是要创建的链接，参数是目标条目；内部转调既有 facade。

## 后续边界

- 暂不抽象 `InputStream` / `OutputStream`，也不加入 buffering、seek、随机读写、异步 I/O 或 POSIX backend；出现第二种真实流实现后再提炼公共接口。
- 暂不暴露时间戳、ACL 或 owner；这些能力需要先确定跨平台的稳定值类型。
- 静态 fallible 方法的直接 `Type::method(...)!` 形态目前不能通过语法；实现与测试使用 `try` 包装，不在本任务修改语法。
