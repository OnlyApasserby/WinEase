# ============================================================================
#  P1-07 屏幕标尺（display.ruler）
# ============================================================================

winease_add_plugin(we_display_ruler
    CLASS_NAME RulerPlugin
    SOURCES
        ruler_plugin.h
        ruler_plugin.cpp
        ruler_overlay.h
        ruler_overlay.cpp
        ruler_plugin.json

        "${CMAKE_SOURCE_DIR}/plugins/common/OverlayGeometry.h"
)
