# ============================================================================
#  src 目录：平台能力层 + 插件 SDK + 主程序
# ============================================================================
add_subdirectory(win32)   # WinEaseWin32：Windows 平台能力（无依赖，先建）
add_subdirectory(sdk)     # WinEaseSdk：插件契约
add_subdirectory(app)     # WinEase：主程序
add_subdirectory(helper)  # WinEaseHelper：提权助手（P0-4，独立进程）

# C++/CLI 桥接层（P3-07）：唯一使用 /clr 的目录。
# 依赖缺失（无 .NET SDK / 无 ijwhost.lib）时内部会自行跳过，
# 由 src/win32/LiteMonitorBridge 在运行时如实报"桥接不可用"。
add_subdirectory(bridge)
