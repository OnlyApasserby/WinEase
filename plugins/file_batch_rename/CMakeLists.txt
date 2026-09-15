# ============================================================================
#  P2-01 批量重命名（file.batch_rename）
#
#  RenameEngine 是纯函数引擎（无 QObject、无界面）：插件与端到端自检
#  **共用同一份源码**，"预览算出来的名字"与"自检断言的名字"必然一致。
# ============================================================================

winease_add_plugin(we_file_batch_rename
    CLASS_NAME BatchRenamePlugin
    SOURCES
        batch_rename_plugin.h
        batch_rename_plugin.cpp
        batch_rename_plugin.json

        "${CMAKE_SOURCE_DIR}/plugins/common/RenameEngine.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/RenameEngine.cpp"
)
