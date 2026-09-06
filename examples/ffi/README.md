# examples/ffi

yux → C：标量、C-layout struct 按值、`ptr_of` 传入、`Ptr` 当不透明 handle 回传 C。
D2：C 通过 yuxrt 头读 **yux 仍持有** 的 `Rc` / `Nullable` / `String`（不 free、不从 `Ptr` 重建 yux 类型）。

```powershell
./prebuild.ps1   # clang-cl 编 c/ffi_demo.c → lib/ffi_demo.lib
yux build
./build/ffi.exe
```

`yux.toml` 的 `[link] lib_dirs` 指向 `lib/`。最终链接仍带编译器自带的 `yuxrt.lib`（`yux_rc_*` 等）。
