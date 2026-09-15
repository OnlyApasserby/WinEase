# ============================================================================
#  P2-05 滚轮增强（input.wheel_enhance）
#
#  VolumeSteps.h 是纯头文件的换算逻辑（格数 ↔ 音量、边界收敛）：
#  插件与端到端自检**共用同一份实现**，否则"一格到底涨几个百分点"
#  这种算术会两边各写一份、各错一半。
# ============================================================================

winease_add_plugin(we_input_wheel_enhance
    CLASS_NAME WheelEnhancePlugin
    SOURCES
        wheel_enhance_plugin.h
        wheel_enhance_plugin.cpp
        wheel_enhance_plugin.json

        "${CMAKE_SOURCE_DIR}/plugins/common/VolumeSteps.h"
)
