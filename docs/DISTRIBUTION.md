# 打包与发布（单文件自解压安装程序）

> 一句话：**用户只需要一个 exe**。双击即装、离线可装、不用先装 Qt / VC++ 运行库 /
> 不用手动解压、也不用改任何系统设置（除了可选功能的权限提示）。
>
> 相关代码：`tools/sfx/`（自解压壳）、`scripts/stage_dist.ps1`（依赖收集 + 打包编排）、
> `cmake/WinEaseDist.cmake`（CMake 目标）、`tests/installer_smoke/`（链路自检）。

---

## 1. 一条命令出货

```powershell
# 全量构建 + 收集依赖 + 打成一个 exe
cmake --build build --config RelWithDebInfo --target winease_installer

# 产物
build\dist\WinEase-0.1.0-x64-Setup.exe              ← 交付物（单文件）
build\dist\WinEase-0.1.0-x64-Setup.manifest.txt     ← 逐文件 SHA-256（审计副本）
```

只想把"要装的东西"铺开看（不出包）：`--target winease_dist_stage` → `build\dist\stage`。

构建期依赖（缺哪个都会**明确报错**，不出残包）：`windeployqt`（随 Qt）、`makecab`（Windows 自带）、
`dotnet`（仅在开启 `.NET` 随包附带时需要）。

---

## 2. 安装程序是怎么拼出来的

```
┌──────────────────────────────────────────────┐  0
│ winease-setup.exe（SFX 壳，/MT 静态链接）     │  ← 不依赖任何运行库，干净机器双击就能跑
├──────────────────────────────────────────────┤  cabOffset
│ payload.cab（LZX 压缩，含全部文件 + 清单）    │  ← makecab 产出，Windows 自带工具
├──────────────────────────────────────────────┤
│ SfxFooter（96 字节：魔数/偏移/长度/CAB 的 SHA-256）│  ← 从文件末尾倒读 96 字节即可定位负载
└──────────────────────────────────────────────┘  EOF
```

* **解压**用 `setupapi.dll` 的 `SetupIterateCabinetW`（系统自带、回调语义明确，见 §6 踩坑）；
* **完整性**：安装前先校验整段 CAB 的 SHA-256（坏包/半截下载会立刻被拦下，不会装出半个程序）；
* **逐文件校验**：`--verify` 会把负载解到临时目录，按清单逐文件比对，然后清理（只读，不写注册表）；
* **SFX 壳用 `/MT`**：安装器的职责之一是把 VC++ 运行库装到目标机上，它自己若依赖
  `msvcp140.dll` 就成了鸡生蛋（干净机器上双击没反应，用户无从下手）。

---

## 3. 依赖是怎么"自动识别并复制"的

| 依赖 | 来源 | 做法 | 依据 |
|---|---|---|---|
| Qt 运行库 + Qt 插件 | `D:\Qt\bin\windeployqt.exe` | 由 Qt 官方工具决定拷什么（platforms / styles / imageformats / tls …），脚本再对 `qwindows.dll` 这类"缺了就打不开"的插件做兜底校验 | 手抄清单一定会随 Qt 版本失效 |
| VC++ 运行库 | `VC\Redist\MSVC\<ver>\x64\Microsoft.VC*.CRT` | **覆盖式拷 DLL**（应用本地部署），不跑系统级 redist、不改注册表 | 免管理员、可卸载、可追溯 |
| .NET 组件 | 目标机已安装的 .NET 8 | **主动检测**（`%ProgramFiles%\dotnet\shared\Microsoft.NETCore.App\8.*`），装了就报版本、没装就如实说明影响面 | 见 §4 的实测结论 |
| 本工程自己的东西 | `build\bin` | 主程序 / 提权助手 / 38 个插件 / C++/CLI 桥接 + 其托管依赖（与主程序**同目录**：CoreCLR 按应用基目录探测） | 缺一个插件就是少一个功能，用户看不出来 |
| 安装说明 | `packaging\README-install.txt` | 随包带上，用户双击前后都能看到边界与注意事项 | 把"已知边界"放在用户手边，而不是只写在仓库里 |

**离线保证**：以上全部是**构建期**从本机取件，安装过程只解压，不下载任何东西（断网可装）。

---

## 4. 已知边界（实测结论，不藏）

### 4.1 .NET：默认**不**随包附带（这是量出来的结论）

* 自包含布局（`coreclr.dll` + `ijwhost.dll` + `runtimeconfig` 的 `includedFrameworks`）
  在**原生宿主**里加载 IJW 程序集时 **fail-fast（0xC0000409）**，连一条可用错误信息都没有；
  排查过程中还修掉了两个前置 bug（见 §6），但自包含形态仍未走通 → **不作为受支持配置**
  （`-DWINEASE_DIST_BUNDLE_DOTNET=ON` 仍在，仅用于实验，脚本会打印警告）。
* **框架依赖 + 目标机 .NET 8** 这条路是**验证通过**的：`--selftest` 让桥接层真实枚举
  **199 个传感器**、退出码 0。
* 因此安装器**主动检测 .NET 8** 并把结果说清楚：
  * 检测到 →  "检测到本机 .NET 运行时 8.0.28（温度类指标可用）"；
  * 未检测到 → "**本机未检测到 .NET 8 运行时**。影响范围：只有「硬件监控」的
    CPU/主板/GPU 温度与风扇转速不可用（面板会如实写原因）；其它功能完全不受影响。"
    并给出下载指引。
* 这满足"目标机无需额外配置即可运行"：**主程序 + 全部插件**（Qt/VC 依赖齐全）照常运行；
  只有**可选的硬件温度模块**需要 .NET 8，而且缺了会**提前说清楚**，不会让用户对着面板猜。

### 4.2 其它

* **没有代码签名**：SmartScreen 会对未知发布者告警（`README-install.txt` 里写明了怎么处理）。
  正式发布需要购买证书，属于仓库之外的运维决定。
* 安装范围是 **当前用户**（`%LOCALAPPDATA%\Programs\WinEase` + HKCU 卸载项），
  全程不需要管理员；需要管理员的功能仍走 `WinEaseHelper.exe`（用户主动触发时才提权）。
* 安装器**不注册 COM / 服务 / 驱动**：右键菜单等由插件在启用时自己写 HKCU，停用即清
  （卸载器只负责文件 + 卸载项 + 快捷方式）。
* 明文 HTTP 的局域网传输等功能的边界，见 `README-install.txt` 与该插件帮助页。

---

## 5. 卸载

`开始菜单` 或 `设置 → 应用 → 已安装的应用` 里点卸载即可 —— 那里的卸载命令就是安装时写进
HKCU 卸载项的这一条（`UninstallString`）：

```text
"<当初那个 Setup.exe 的路径>" --uninstall --dir "<安装目录>"
```

也就是说**卸载用的是同一个 exe**（单文件交付物的好处之一：不用额外携带/维护一个卸载器）。
命令行手工调用也一样：

```powershell
& "path\to\WinEase-0.1.0-x64-Setup.exe" --uninstall --dir "$env:LOCALAPPDATA\Programs\WinEase"
# 装到临时目录时（自检/试装）就换成对应目录
```

卸载按 `install.manifest.txt` 删文件（**不去猜目录里有什么**），并回收
Qt 的 `platforms/ styles/ imageformats/ tls/ …` 空壳目录树、注册项、快捷方式。
⚠ 卸载前先退出 WinEase（运行中的程序会占住文件）。

---

## 6. 打包链路的自检与踩过的坑

```powershell
.\build\bin\installer_smoke.exe        # 17 项断言，退出码 0 = 全过
```

它验的是"**用户拿到的那一个 exe**"：`--verify` 逐文件 SHA-256、**依赖审计**
（把负载里每个 PE 的导入表都读出来，每条非系统导入都必须在负载内找得到 ——
这是"目标机无需额外配置"唯一的硬证据）、安装/注册/卸载的**真实状态**（含 HKCU 卸载项、
开始菜单快捷方式是否被删干净）、以及桥接自检。

踩过的坑（已进 `docs/traps.md`）：

| 现象 | 根因 | 解决 |
|---|---|---|
| `FDICreate` 直接返回 NULL，错误码是上一次调用残留的 183，**连一次分配回调都没调用** | cabinet.dll 的 FDI 那套在参数校验阶段就拒了，排查价值极低 | 改用 `SetupIterateCabinetW`（setupapi）：回调里改 `FullTargetName` 即改落点，语义白纸黑字 |
| `--verify` 报"**119 个文件全部缺失**"，且"文件名"前面挂着一串数字 | 清单是 `hash␠␠size␠␠路径`（两个空格），解析按**单个**空格切，把 size 当成了路径的一部分 | 解析改成"一个或多个空白"；这类"解析错位"的表现很像"功能没实现"，先怀疑格式 |
| 桥接加载即 **fail-fast 0xC0000409**，无任何日志 | ① 生成的 `runtimeconfig.json` 被写成了三行（PowerShell 数组字面量里的 `+` 拼接会**裂成多个元素**）；② 自包含布局还缺 `deps.json` | 用 `-f` 格式化替代 `+`；补 `WinEaseLiteMonitorBridge.deps.json`（同时保留 `ConvertFrom-Json` 自检，坏 JSON 不许出门） |
| 卸载后剩下一堆空目录（`platforms/ styles/ tls/ …`） | 只删了 `<dir>\plugins` 与根目录 | 按清单里每个文件的父目录**由深到浅**尝试删空目录 |
| "开始菜单快捷方式创建失败"，其它一切正常 | `IShellLink` 是 COM 组件，`CoCreateInstance` 前没 `CoInitializeEx` | 创建快捷方式前后配对 `CoInitializeEx` / `CoUninitialize` |
| `.ps1` 里的中文注释让脚本解析报错 | PowerShell 5.1 按 ANSI(936) 读 `.ps1`，UTF-8 中文在解析器里变成乱码 | **脚本一律纯 ASCII**（`stage_dist.ps1` 头部写明了这条硬约束） |

---

## 7. 变更记录

| 日期 | 变更 |
|---|---|
| 2026-09-15 | 初版：`winease-setup`（SFX 壳）+ `scripts/stage_dist.ps1`（收集 Qt/VC/插件/桥接 → 清单 → CAB → 追加负载）+ `winease_installer` 目标 + `installer_smoke`（17 项）。结论：**默认框架依赖**（自包含 IJW 未走通，安装器改为主动检测 .NET 8 并如实说明影响面）；交付物 21.3 MB 单文件、121 个文件、离线可装。 |
