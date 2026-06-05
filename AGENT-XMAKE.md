# AGENT-XMAKE — xmake 构建系统文档速查

**用法**：需要查 xmake API 时，按下方表格找到对应页面，用 `WebFetch` 抓取 `https://xmake.io` + `href` 列的值（`.md` 为 LLM 优化格式，内容与 `.html` 等价）。仅 LLM 使用，不给人看。

> 来源：`xmkae.html`（xmake.io 侧边栏导航 HTML 片段 → 后缀换 `.md`）

## 描述域 API（xmake.lua 项目描述）

| 页面 | href |
|------|------|
| 接口规范（命名约定） | `/zh/api/description/specification.md` |
| 全局接口 | `/zh/api/description/global-interfaces.md` |
| 条件判断 | `/zh/api/description/conditions.md` |
| 辅助接口 | `/zh/api/description/helper-interfaces.md` |
| 工程目标 | `/zh/api/description/project-target.md` |
| 配置选项 | `/zh/api/description/configuration-option.md` |
| 插件任务 | `/zh/api/description/plugin-and-task.md` |
| 自定义规则 | `/zh/api/description/custom-rule.md` |
| 自定义工具链 | `/zh/api/description/custom-toolchain.md` |
| 包依赖 | `/zh/api/description/package-dependencies.md` |
| 内置变量 | `/zh/api/description/builtin-variables.md` |
| 内置规则 | `/zh/api/description/builtin-rules.md` |
| 内置策略 | `/zh/api/description/builtin-policies.md` |
| XPack 打包接口 | `/zh/api/description/xpack-interfaces.md` |
| XPack 组件接口 | `/zh/api/description/xpack-component-interfaces.md` |

## 脚本域 API（待补）

> `xmkae.html` 当前仅含描述域导航。脚本域页面（target 实例接口、脚本模块等）需从 `https://xmake.io/zh/api.html` 侧边栏抓取后补入。

## 相关本地文件

- [`xmake.lua`](xmake.lua) — 项目构建脚本
- [`DEPS.json`](DEPS.json) — 第三方依赖声明
- [`.claude/skills/yux-lang-dev/SKILL.md`](.claude/skills/yux-lang-dev/SKILL.md) — 引用本文件
