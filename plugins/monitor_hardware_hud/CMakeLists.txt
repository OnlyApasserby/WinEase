# ============================================================================
#  硬件监控悬浮窗（P3-07 第一批）
#
#  悬浮层 HUD：CPU / 内存 / 网速 / GPU 利用率 / 磁盘温度（五项零依赖指标）。
#  CPU 温度需要 PawnIOLib.dll，本批如实报不可用，留给第二里程碑。
#
#  HudMetrics.{h,cpp} 是与 feature_smoke 共用的显示模型（插在这里显式列出，
#  与 display_focus_highlight 引用 OverlayGeometry.h 的写法一致）：
#  "读不到就必须显示原因"这条逻辑由插件与自检编译同一份源码。
# ============================================================================

winease_add_plugin(we_monitor_hardware_hud
    CLASS_NAME HardwareHudPlugin
    SOURCES
        hardware_hud_plugin.h
        hardware_hud_plugin.cpp
        hud_overlay.h
        hud_overlay.cpp
        hardware_hud_plugin.json
        "${CMAKE_SOURCE_DIR}/plugins/common/HudMetrics.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/HudMetrics.cpp"
)
