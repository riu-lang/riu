# 位运算改整数方法

运算符 `& | ^ << >> ~` 与复合赋值 `&= |= ^= <<= >>=` 删除。整数走已有 `#Builtin` 方法。

- 形态：`a.and(b)` / `a.or(b)` / `a.xor(b)` / `a.shl(n)` / `a.shr(n)` / `a.inv()`
- 仅 INT（`i8`/`u8`/…/`isize`/`usize`）。`bool` 仅 `not` / `!` / `&&` / `||`
- `&` 仍是 `T&` / `&obj`；`~` 仍是 `fn ~()`
- lexer：`SymbolOr` / `SymbolXor` 注释；`SymbolAndEq` / `SymbolOrEq` / `SymbolXorEq` 删除
- parser：无 `exprShift` / `exprBinOp`；`exprUnary` 只 `-` `!`；`opAssign` 只 `= += -= *= /= %=`
- const-eval：折 `recv.and/or/xor/shl/shr(arg)` 与 `recv.inv()`
- 负数接收者：`(-5).and(3)`，因为 `-5.and(3)` 是 `-(5.and(3))`
- codegen：一元 `inv`/`neg`/`not` 先于 `args[0]`；移位次数 LLVM 类型与左操作数对齐
- `#Cval`：`const_mut_checker` 放行整数位方法
- 规范：§4.4.2 / §1.7 / §5.2.1.2 / 附录 A·B / §7.2.3.2 / §9.1.4
