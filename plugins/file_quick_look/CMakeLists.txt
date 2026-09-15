# ============================================================================
#  P2-02 快速文件预览（file.quick_look）
#
#  FilePreview 是"取内容"的纯函数引擎（文本头部 / 缩放解码图片 / 单页 PDF /
#  系统缩略图）：插件与端到端自检共用同一份源码，自检才能直接断言
#  "只读了 512 KB"这类数字，而不是隔着窗口去数控件。
#
#  PDF 走 WinRT（Windows.Data.Pdf）：winrt 头文件与 windowsapp.lib 由
#  WinEaseWin32 以 PUBLIC 方式带出来，这里无需额外声明依赖。
# ============================================================================

winease_add_plugin(we_file_quick_look
    CLASS_NAME QuickLookPlugin
    SOURCES
        quick_look_plugin.h
        quick_look_plugin.cpp
        quick_look_plugin.json

        "${CMAKE_SOURCE_DIR}/plugins/common/FilePreview.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/FilePreview.cpp"
)
