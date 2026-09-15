# ============================================================================
#  P2-12 敏感文件粉碎（security.file_shredder）
#
#  FileShredder 引擎（覆写 → 校验 → 改名 → 删除 + 保护路径安全阀）与端到端自检
#  共用同一份源码：自检用 overwriteAndVerify() 直接验证"覆写实现本身"，
#  再用插件面板走一遍完整流程。
# ============================================================================

winease_add_plugin(we_security_file_shredder
    CLASS_NAME ShredderPlugin
    SOURCES
        shredder_plugin.h
        shredder_plugin.cpp
        shredder_plugin.json

        "${CMAKE_SOURCE_DIR}/plugins/common/FileShredder.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/FileShredder.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/common/TaskCancel.h"
)
