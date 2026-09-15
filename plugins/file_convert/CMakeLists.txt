# ============================================================================
#  P3-03 批量格式转换（file.convert）
#
#  两套引擎与插件、端到端自检**共用同一份源码**：
#    * ImageConvertEngine —— 计划/命名/冲突/缩放/水印/落盘
#    * TextEncodingTools  —— 字符集探测/解码/编码
#  "预览与实际写出的文件必须一致"这类断言才有意义（见 ImageConvertEngine.h 说明）。
#
#  依赖只多一个 Qt6::Gui（QImage/QPainter 是 Gui 模块），不引入任何新三方库。
# ============================================================================

winease_add_plugin(file_convert
    CLASS_NAME FileConvertPlugin
    SOURCES
        convert_plugin.h
        convert_plugin.cpp
        convert_plugin.json

        "${CMAKE_SOURCE_DIR}/plugins/common/ImageConvertEngine.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/ImageConvertEngine.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/common/TextEncodingTools.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/TextEncodingTools.cpp"

        # 面板"从剪贴板取路径"入口
        "${CMAKE_SOURCE_DIR}/plugins/common/ClipboardTools.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/ClipboardTools.cpp"

        "${CMAKE_SOURCE_DIR}/plugins/common/TaskCancel.h"
)
