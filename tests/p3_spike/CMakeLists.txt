# ============================================================================
#  p3_spike —— P3 技术预研探针（第 8 套自检）
#
#  为什么单独立一套：ROADMAP 的 P3 章节写着一条硬约定 ——
#  **每项必须先做预研 spike、产出可行性结论，再决定「实现 / 降级 / 砍掉」**。
#  预研结论必须可重复验证（Windows 大版本更新、装了 PawnIO、换了麦克风，
#  结论都可能变），所以它不该是一次性的临时脚本，而是一套能随时重跑的探针。
#
#  探针**只读**：不改系统状态、不真切设备、不碰用户文件。
#  运行方式：
#      .\build\bin\p3_spike.exe        # 退出码 0 = 所有"可判定"的探针都跑通了
# ============================================================================

qt_add_executable(p3_spike
    main.cpp
)

target_include_directories(p3_spike PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/app"
)

target_link_libraries(p3_spike PRIVATE
    WinEaseWin32      # probeWinRtClass / ComApartment（探针只依赖平台层）
    Qt6::Core
    Qt6::Gui
)

if (WIN32)
    target_link_libraries(p3_spike PRIVATE
        ole32         # COM（IVirtualDesktopManager / IPolicyConfig）
        rstrtmgr      # Restart Manager（P3-02 文件锁定）
        setupapi      # 设备枚举（P3-14 可移动卷）
        mfplat        # Media Foundation（P3-08 录屏前置）
        shlwapi
    )
endif ()

set_target_properties(p3_spike PROPERTIES
    OUTPUT_NAME "p3_spike"
    WIN32_EXECUTABLE FALSE   # 控制台子系统：探针输出要能直接看到
)
