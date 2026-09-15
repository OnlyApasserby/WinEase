# ============================================================================
#  overlay_smoke —— OverlayKit（P0-3）验收测试
#
#  说明：OverlayWindow 与 OverlayHost 都在 WinEaseSdk 中，随 SDK 一起链接。
#        （OverlayHost 原在 src/app/core/，P1-C 起移入 SDK：插件 DLL 也要调用它，
#          而主程序的符号对 DLL 不可见 —— 见 src/sdk/OverlayHost.h 的说明）
# ============================================================================
qt_add_executable(overlay_smoke
    main.cpp
)

target_include_directories(overlay_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/app"
)

target_link_libraries(overlay_smoke PRIVATE
    WinEaseSdk
    WinEaseWin32
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

set_target_properties(overlay_smoke PROPERTIES
    OUTPUT_NAME "overlay_smoke"
    WIN32_EXECUTABLE FALSE
)
