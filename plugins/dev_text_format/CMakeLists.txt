# ============================================================================
#  P1-10 JSON / XML 格式化（dev.text_format）
# ============================================================================

winease_add_plugin(we_dev_text_format
    CLASS_NAME TextFormatPlugin
    SOURCES
        text_format_plugin.h
        text_format_plugin.cpp
        text_format_plugin.json

        # 插件间共享的实现（另见 plugins/common/：三个插件与自检用同一份源码）
        "${CMAKE_SOURCE_DIR}/plugins/common/TextTools.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/TextTools.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/common/ClipboardTools.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/ClipboardTools.cpp"
)
