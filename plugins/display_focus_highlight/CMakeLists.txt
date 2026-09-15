# ============================================================================
#  P1-08 焦点高亮（display.focus_highlight）
# ============================================================================

winease_add_plugin(we_display_focus_highlight
    CLASS_NAME FocusHighlightPlugin
    SOURCES
        focus_highlight_plugin.h
        focus_highlight_plugin.cpp
        spotlight_overlay.h
        spotlight_overlay.cpp
        focus_highlight_plugin.json

        "${CMAKE_SOURCE_DIR}/plugins/common/OverlayGeometry.h"
)
