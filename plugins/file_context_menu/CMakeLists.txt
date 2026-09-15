# ============================================================================
#  P1-05 右键菜单扩展（file.context_menu）
# ============================================================================

winease_add_plugin(we_file_context_menu
    CLASS_NAME ContextMenuPlugin
    SOURCES
        context_menu_plugin.h
        context_menu_plugin.cpp
        context_menu_plugin.json
)
