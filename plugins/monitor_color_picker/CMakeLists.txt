# ============================================================================
#  P1-06 屏幕取色器（monitor.color_picker）
# ============================================================================

winease_add_plugin(we_monitor_color_picker
    CLASS_NAME ColorPickerPlugin
    SOURCES
        color_picker_plugin.h
        color_picker_plugin.cpp
        color_picker_plugin.json

        # 插件间共享的实现（与 dev_text_format / dev_encoder / 自检用同一份源码）
        "${CMAKE_SOURCE_DIR}/plugins/common/ClipboardTools.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/ClipboardTools.cpp"
)
