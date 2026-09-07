# examples

从 `yux.toml` 到可执行文件：

```powershell
cd <本目录下某个项目>
yux build
./build/<name>.exe
```

`entry` 相对源根 `src/`。产物在 `build/<name>.exe`。

| 目录 | 做什么 |
|------|--------|
| [echo](echo/) | 打印命令行参数（`args()`） |
| [cat](cat/) | 读文件到 stdout（`yux.io.read_file`） |
| [guess](guess/) | stdin 猜数字（`read_line` + `parse_i32`） |
| [ls](ls/) | 列目录（`cwd` / `exists` / `list_dir`） |
| [test](test/) | 语法 demo + `yux test` |
| [ffi](ffi/) | yux 调 C / C 读 yux 对象（先 `./prebuild.ps1`） |

逐步说明见 [docs/第一个程序.md](../docs/第一个程序.md)。
