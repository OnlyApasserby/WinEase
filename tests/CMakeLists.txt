# ============================================================================
#  自检程序
#
#  这些不是单元测试框架下的用例，而是**可执行的运行时验证程序**：
#  平台能力层（WinEaseWin32）里的很多 API 无法靠"编译通过"证明可用，
#  必须真正调用一次并观察结果（窗口样式、抓屏像素、注册表往返等）。
#
#  执行方式：
#      .\build\bin\win32_smoke.exe
#  退出码 0 表示全部通过。
# ============================================================================

if (NOT WINEASE_BUILD_SMOKE_TESTS)
    message(STATUS "[WinEase] 已跳过自检程序构建（WINEASE_BUILD_SMOKE_TESTS=OFF）")
    return()
endif ()

add_subdirectory(win32_smoke)
add_subdirectory(hook_smoke)
add_subdirectory(overlay_smoke)
add_subdirectory(theme_smoke)
add_subdirectory(elevation_smoke)
add_subdirectory(plugin_smoke)
add_subdirectory(ui_smoke)       # 主界面呈现（卡片两行 + 帮助栏目）
add_subdirectory(feature_smoke)
add_subdirectory(installer_smoke) # 打包链路（自解压安装程序：完整性 / 依赖审计 / 安装注册 / 卸载）
add_subdirectory(p3_spike)   # P3 技术预研探针（只读；结论回写 ROADMAP 的 P3 各项）
add_subdirectory(scratch_repro)  # ⚠ 临时排障目标，定案后删除
