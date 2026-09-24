# examples/ffi

riu → C：标量、C-layout struct 按值、`ptr_of` 传入、`Ptr` 当不透明 handle 回传 C。
D2：C 通过 riurt 头读 **riu 仍持有** 的 `Rc` / `Nullable` / `String`（不 free、不从 `Ptr` 重建 riu 类型）。

```powershell
uv run python prebuild.py   # clang-cl 编 c/ffi_demo.c → lib/ffi_demo.lib
riu build
./build/ffi.exe
```

`riu.toml` 把链接写在 `[[executable]]` 上：`path="./lib/ffi_demo"` 指向本树导入库（不写 `.lib` 后缀）。最终链接仍带编译器自带的 `riurt.lib`（`riu_rc_*` 等），以及 SDK 传来的 `//kernel32` / `//shell32`。

```toml
[[executable]]
entry="main.ut"
external_links=[
    { path="./lib/ffi_demo" },
]
```
