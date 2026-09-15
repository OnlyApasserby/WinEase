# ============================================================================
#  P2-03 重复文件查找（file.duplicate_finder）
#
#  DuplicateFinder 引擎是纯函数（四级流水线各自可单独调用）：插件与端到端自检
#  共用同一份源码，自检才能逐级断言"抽样确实没全量读盘"（stats.partialHashBytes）。
# ============================================================================

winease_add_plugin(we_file_duplicate_finder
    CLASS_NAME DuplicateFinderPlugin
    SOURCES
        duplicate_finder_plugin.h
        duplicate_finder_plugin.cpp
        duplicate_finder_plugin.json

        "${CMAKE_SOURCE_DIR}/plugins/common/DuplicateFinder.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/DuplicateFinder.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/common/TaskCancel.h"
)
