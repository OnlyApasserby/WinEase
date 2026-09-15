# ============================================================================
#  src 目录：平台能力层 + 插件 SDK + 主程序
# ============================================================================
add_subdirectory(win32)   # WinEaseWin32：Windows 平台能力（无依赖，先建）
add_subdirectory(sdk)     # WinEaseSdk：插件契约
add_subdirectory(app)     # WinEase：主程序
add_subdirectory(helper)  # WinEaseHelper：提权助手（P0-4，独立进程）
