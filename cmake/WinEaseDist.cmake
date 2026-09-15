# ============================================================================
#  WinEaseDist.cmake —— 一键出"单文件离线安装程序"
#
#      cmake --build build --config RelWithDebInfo --target winease_installer
#
#  产物：build/dist/WinEase-<版本>-x64-Setup.exe
#        · 一个文件、离线可装、目标机**不需要预装任何运行库**
#        · 结构 = winease-setup.exe（静态 CRT 的 SFX 壳）+ payload.cab + 96 字节尾部
#        · 附带 build/dist/WinEase-<版本>-x64-Setup.manifest.txt（逐文件 SHA-256）
#
#  为什么编排放在脚本里（scripts/stage_dist.ps1）而不是全写在 CMake：
#      "收集依赖"这件事大量依赖工具自身的输出（windeployqt 决定拷哪些 Qt 插件、
#      dotnet publish 决定带哪些运行时文件），用脚本做**收集 + 校验 + 打包**更直白；
#      CMake 只负责"什么时候跑、按什么参数跑"。
#
#  三个外部前提（缺一个就给明确提示，不静默出一个残包）：
#      · windeployqt（随 Qt 安装，本目标从 Qt6::Core 的位置反推）
#      · makecab     （Windows 自带，System32）
#      · dotnet      （.NET 8 SDK；仅自包含打包 .NET 运行库时需要）
# ============================================================================

option(WINEASE_BUILD_INSTALLER "构建单文件自解压安装程序（winease_installer 目标）" ON)

if (NOT WINEASE_BUILD_INSTALLER)
    message(STATUS "[WinEase] 安装包：已按开关关闭（WINEASE_BUILD_INSTALLER=OFF）")
    return()
endif ()

if (NOT WIN32)
    return()
endif ()

find_program(WINEASE_MAKECAB_EXE NAMES makecab)
find_program(WINEASE_POWERSHELL_EXE NAMES powershell)

# windeployqt 与 Qt 同目录：从 Qt6::Core 的实际位置反推，避免写死 D:/Qt
get_target_property(_winease_qt_core_location Qt6::Core IMPORTED_LOCATION_RELWITHDEBINFO)
if (NOT _winease_qt_core_location)
    get_target_property(_winease_qt_core_location Qt6::Core IMPORTED_LOCATION_RELEASE)
endif ()
if (NOT _winease_qt_core_location)
    get_target_property(_winease_qt_core_location Qt6::Core IMPORTED_LOCATION)
endif ()
get_filename_component(WINEASE_QT_BIN_DIR "${_winease_qt_core_location}" DIRECTORY)

# .NET 运行库是否随包附带
#
# ⚠ 默认 **OFF**，这是实测结论不是偷懒：自包含布局（coreclr.dll + ijwhost +
#   includedFrameworks）在**原生宿主**里加载 IJW 程序集时 fail-fast（0xC0000409，
#   见 docs/DISTRIBUTION.md「已知边界」）；而"用目标机上的 .NET 8 运行时"这条
#   路是验证过的（自检枚举到 199 个传感器）。
#   开关保留下来供实验，但**不是受支持的配置**：安装器会主动检测 .NET 8 并把
#   结果写清楚（缺了就如实说"温度类指标不可用"，绝不假装成功）。
option(WINEASE_DIST_BUNDLE_DOTNET
       "把 .NET 8 运行库打进安装包（实验性：自包含 IJW 未验证通过，包体 +约 70MB）" OFF)

if (NOT WINEASE_MAKECAB_EXE)
    message(STATUS "[WinEase] 安装包：未找到 makecab，winease_installer 目标不可用")
    return()
endif ()
if (NOT WINEASE_POWERSHELL_EXE)
    message(STATUS "[WinEase] 安装包：未找到 powershell，winease_installer 目标不可用")
    return()
endif ()
if (NOT EXISTS "${WINEASE_QT_BIN_DIR}/windeployqt.exe")
    message(STATUS "[WinEase] 安装包：未找到 windeployqt（在 ${WINEASE_QT_BIN_DIR}），"
                   "winease_installer 目标不可用")
    return()
endif ()

if (WINEASE_DIST_BUNDLE_DOTNET)
    set(_winease_bundle_dotnet "true")
else ()
    set(_winease_bundle_dotnet "false")
endif ()

set(WINEASE_INSTALLER_NAME "WinEase-${PROJECT_VERSION}-x64-Setup.exe")

# ---------------------------------------------------------------------------
#  打包目标
#
#  ⚠ 依赖写全：主程序 + 提权助手 + 插件 + SFX 壳。少一个依赖就会出现
#    "插件还没编完就把 plugins 目录打进包里"这种最难发现的残包。
# ---------------------------------------------------------------------------
get_property(_winease_plugin_targets GLOBAL PROPERTY WINEASE_PLUGIN_TARGETS)

add_custom_target(winease_installer
    COMMAND "${WINEASE_POWERSHELL_EXE}" -ExecutionPolicy Bypass -NoProfile
            -File "${CMAKE_SOURCE_DIR}/scripts/stage_dist.ps1"
            -BuildDir "${CMAKE_BINARY_DIR}"
            -Config "$<CONFIG>"
            -Version "${PROJECT_VERSION}"
            -QtBin "${WINEASE_QT_BIN_DIR}"
            -StubPath "${CMAKE_BINARY_DIR}/bin/winease-setup.exe"
            -BundleDotnet ${_winease_bundle_dotnet}
            -InstallerName "${WINEASE_INSTALLER_NAME}"
    DEPENDS WinEase WinEaseHelper winease-setup ${_winease_plugin_targets}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "[WinEase] 打包单文件安装程序：build/dist/${WINEASE_INSTALLER_NAME}"
    USES_TERMINAL
    VERBATIM
)

# 只想"铺开目录"时用（调试安装内容，不出包）
add_custom_target(winease_dist_stage
    COMMAND "${WINEASE_POWERSHELL_EXE}" -ExecutionPolicy Bypass -NoProfile
            -File "${CMAKE_SOURCE_DIR}/scripts/stage_dist.ps1"
            -BuildDir "${CMAKE_BINARY_DIR}"
            -Config "$<CONFIG>"
            -Version "${PROJECT_VERSION}"
            -QtBin "${WINEASE_QT_BIN_DIR}"
            -StubPath "${CMAKE_BINARY_DIR}/bin/winease-setup.exe"
            -BundleDotnet ${_winease_bundle_dotnet}
            -InstallerName "${WINEASE_INSTALLER_NAME}"
    DEPENDS WinEase WinEaseHelper winease-setup ${_winease_plugin_targets}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "[WinEase] 铺设发布目录：build/dist/stage"
    USES_TERMINAL
    VERBATIM
)

message(STATUS "[WinEase] 安装包：启用（makecab=${WINEASE_MAKECAB_EXE}，"
               "Qt=${WINEASE_QT_BIN_DIR}，.NET 自包含=${_winease_bundle_dotnet}）")
