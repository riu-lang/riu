# examples

从 `riu.toml` 到可执行文件：

```powershell
cd <本目录下某个项目>
riu build
./build/<name>.exe
```

`entry` 相对源根 `src/`。产物在 `build/<name>.exe`。

| 目录 | 做什么 |
|------|--------|
| [echo](echo/) | 打印命令行参数（`args()`） |
| [cat](cat/) | 读文件到 stdout（`riu.io.read_file`） |
| [guess](guess/) | stdin 猜数字（`read_line` + `parse_i32`） |
| [ls](ls/) | 列目录（`cwd` / `exists` / `list_dir`） |
| [tree](tree/) | 目录树（`File.list`，软链不递归） |
| [grep](grep/) | 按子串过滤行（`contains`；`-i` 忽略大小写） |
| [wc](wc/) | 行 / 词 / UTF-8 字节（ASCII 空白分词） |
| [freq](freq/) | 词频（`Map<String, i32>`，按次数降序） |
| [wln](wln/) | 创建软链 / 硬链 / junction（`riu.io.symlink` 等） |
| [ffi](ffi/) | riu 调 C / C 读 riu 对象（先 `./prebuild.ps1`） |

逐步说明见 [docs/第一个程序.md](../docs/第一个程序.md)。
