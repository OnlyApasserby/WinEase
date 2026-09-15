# ============================================================================
#  P2-06 快速关机（monitor.power_panel）
#
#  没有额外链接需求：电源操作走提权助手（SDK 的 ElevationService），
#  锁定与"取消挂起关机"走平台层 PowerUtils —— 都在既有依赖里。
# ============================================================================

winease_add_plugin(monitor_power_panel
    CLASS_NAME PowerPanelPlugin
    SOURCES
        power_panel_plugin.h
        power_panel_plugin.cpp
        power_panel_plugin.json
        "${CMAKE_SOURCE_DIR}/plugins/common/PowerControl.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/PowerControl.cpp"
)
