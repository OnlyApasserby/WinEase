# ============================================================================
#  P1-09 网页快速搜索（launcher.web_search）
# ============================================================================

winease_add_plugin(we_launcher_web_search
    CLASS_NAME WebSearchPlugin
    SOURCES
        web_search_plugin.h
        web_search_plugin.cpp
        web_search_plugin.json

        "${CMAKE_SOURCE_DIR}/plugins/common/ClipboardTools.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/ClipboardTools.cpp"
)
