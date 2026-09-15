# ============================================================================
#  P1-11 编码转换（dev.encoder）
# ============================================================================

winease_add_plugin(we_dev_encoder
    CLASS_NAME EncoderPlugin
    SOURCES
        encoder_plugin.h
        encoder_plugin.cpp
        encoder_plugin.json

        "${CMAKE_SOURCE_DIR}/plugins/common/TextTools.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/TextTools.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/common/ClipboardTools.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/ClipboardTools.cpp"
)
