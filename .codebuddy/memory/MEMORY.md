# WinEase 项目记忆索引

Windows 易用性增强工具集（C++20 + Qt 6.8.4 Widgets + CMake + **MSVC**，插件化架构，中文界面/注释）。工作区 `f:/develop/1919810`。

本文件**只做索引**。细则按主题 / 任务分文件放在 `.codebuddy/memories/`。

---

## 入口文档

| 文件 | 用途 |
|---|---|
| `CODEBUDDY.md`（根目录） | 新会话上手：构建与自检命令、架构约定（需读多个文件才能理解的"大局"） |
| `docs/ROADMAP.md` | 50 项任务 + 勾选进度 + 决策 + 裁剪 + 变更记录 + 每项"产出/验证/自检断言" |
| `docs/traps.md` | 实现踩坑记录（编号全工程共享；源码注释里"踩坑 #NN"指它） |
| `docs/ENV-SETUP.md` | 依赖补齐、WinRT 约束、PawnIO 策略、构建约定（§6）、各套自检运行注意 |

## 主题记忆（`.codebuddy/memories/`）

| 主题 | 文件 |
|---|---|
| 环境硬约束 + 本机配置 | `.codebuddy/memories/env.md` |
| 构建与自检要点 | `.codebuddy/memories/build.md` |
| 架构约定（插件模型 / 分层 / 平台层 / 提权 / 信息层级） | `.codebuddy/memories/architecture.md` |
| 已交付模块（P0 / P1 / P2 / P3 概要） | `.codebuddy/memories/modules.md` |

> **2026-09-15 状态（第四批追加）**：交付链打通 —— `cmake --build build --target winease_installer`
> 出一条 **21.3 MB 单文件离线安装程序**（内含 Qt + VC++ 运行库 + 38 个插件 + 桥接），
> 自检 `installer_smoke`（17 项，含**逐 PE 依赖审计**）。设计与实测边界见 `docs/DISTRIBUTION.md`；
> ⚠ .NET 默认**不随包**（自包含 IJW 在原生宿主里 fail-fast 0xC0000409），安装器改为主动检测并说明。
>
> **2026-09-15 状态**：P3 按用户要求裁剪为**已完成的 7 项**（P3-01/02/03/07/09/11/14）+
> 新增 **F1 `file.batch_move`**（正则批量移动）与 **F2 `net.lan_transfer`**（局域网跨平台传输）；
> 未完成的 9 项已**删除条目**。P3-07 的温度采集改走 **C++/CLI 桥接 LibreHardwareMonitor**
> （`src/bridge/` 是全工程唯一允许 `/clr` 的目录），PawnIO 路线作废。
| 已确认决策 D1~D12 | `.codebuddy/memories/decisions.md` |
| P3 技术预研 + 进度与下一步 | `.codebuddy/memories/p3-spike.md` |
| 高频踩坑（触发式） | `.codebuddy/memories/traps.md` |
| 文档约定（推进时必须回填） | `.codebuddy/memories/docs.md` |
| 界面信息层级（用户插单） | `.codebuddy/memories/ui-info-hierarchy.md` |

## 任务记忆（`.codebuddy/memories/tasks/`）

按 `YYYY-MM-DD__<任务标识>.md` 命名，一任务一文件。

| 任务 | 文件 |
|---|---|
| 单文件自解压安装程序（F3：`winease_installer` + `installer_smoke`） | `.codebuddy/memories/tasks/2026-09-15__packaging-installer.md` |
| P3 裁剪 + P3-07 改 C++/CLI 桥接 + 两项新功能（F1 批量移动 / F2 局域网传输） | `.codebuddy/memories/tasks/2026-09-15__p3-prune-bridge-and-two-features.md` |
| P3-11 `media.player_panel` 媒体控制面板交付 | `.codebuddy/memories/tasks/2026-09-15__p3-11-media-player-panel.md` |
| P3-09 `media.mixer` 音量混合器交付 | `.codebuddy/memories/tasks/2026-09-15__p3-09-media-mixer.md` |
| P3-07 第一批 `monitor.hardware_hud` 交付 | `.codebuddy/memories/tasks/2026-09-15__p3-07-first-batch.md` |
| P3-03 `file.convert` 批量格式转换交付 | `.codebuddy/memories/tasks/2026-09-14__p3-03-file-convert.md` |
| 建立 `CODEBUDDY.md` | `.codebuddy/memories/tasks/2026-09-14__establish-codebuddy.md` |
| MEMORY.md 拆分（本次） | `.codebuddy/memories/tasks/2026-09-14__memory-split.md` |

## 当日日志

`.codebuddy/memory/YYYY-MM-DD.md`（**注意是单数 `memory`**，与复数 `memories/` 区分）—— 当日追加的工作笔记（**追加，不覆盖**）。

## 写入规则（推进时同步回填）

1. 完成一个**任务级**工作 → 在 `.codebuddy/memories/tasks/` 建独立任务记忆，并在当日日志记一笔。
2. 跨会话长期事实（约定、决策、硬约束）→ 落入 `.codebuddy/memories/` 下对应主题文件；新增主题才往本索引加一行。
3. **本索引只列路径，不列内容**——保持薄到秒加载。