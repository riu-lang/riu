# examples/ffi

yux → C：标量、C-layout struct 按值、`ptr_of` 传入、`Ptr` 当不透明 handle 回传 C。

```powershell
./prebuild.ps1   # clang-cl 编 c/ffi_demo.c → lib/ffi_demo.lib
yux build
./build/ffi.exe
```

`yux.toml` 的 `[link] lib_dirs` 指向 `lib/`。C 头 `c/yux_ffi.h` 只有 `typedef void* yux_ptr`。
