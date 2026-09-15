# ============================================================================
#  P1-12 端口占用查看（dev.port_viewer）
# ============================================================================

winease_add_plugin(we_dev_port_viewer
    CLASS_NAME PortViewerPlugin
    SOURCES
        port_viewer_plugin.h
        port_viewer_plugin.cpp
        port_viewer_plugin.json

        "${CMAKE_SOURCE_DIR}/plugins/common/ClipboardTools.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/ClipboardTools.cpp"
)
