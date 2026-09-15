# ============================================================================
#  插件模板 —— 复制本目录并重命名即可创建新功能插件
#
#  步骤：
#    1. 复制 plugins/template -> plugins/<新功能名>
#    2. 重命名源文件与类名（类名需与 CLASS_NAME 一致）
#    3. 修改 id()/name()/description()/category()
#    4. 在 plugins/CMakeLists.txt 中 add_subdirectory(<新功能名>)
# ============================================================================

winease_add_plugin(we_template
    CLASS_NAME TemplatePlugin
    SOURCES
        template_plugin.h
        template_plugin.cpp
        template_plugin.json
)
