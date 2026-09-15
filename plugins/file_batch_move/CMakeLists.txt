# ============================================================================
#  file_batch_move —— 批量移动文件（正则匹配）
#
#  引擎 BatchMoveEngine 住在 plugins/common，被**插件与自检编译同一份**
#  （计划/预演/撤销的口径必须只有一个，否则自检验的是另一套实现）。
# ============================================================================

winease_add_plugin(we_file_batch_move
    CLASS_NAME BatchMovePlugin
    SOURCES
        batch_move_plugin.h
        batch_move_plugin.cpp
        batch_move_plugin.json

        "${CMAKE_SOURCE_DIR}/plugins/common/BatchMoveEngine.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/BatchMoveEngine.cpp"
)
