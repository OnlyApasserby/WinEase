# ============================================================================
#  P2-04 剪贴板历史（input.clipboard_history）
#
#  两个共用件（插件与端到端自检**共用同一份源码**，断言才有意义）：
#    * plugins/common/ClipboardHistoryStore.*   存储引擎（分片文件 + 索引）
#    * src/win32/ClipboardPrivacy.*             剪贴板来源标记识别（隐私闸门）
#      —— 它是平台原语，已在 WinEaseWin32 里，插件自动链接，无需在此列出
# ============================================================================

winease_add_plugin(we_input_clipboard_history
    CLASS_NAME ClipboardHistoryPlugin
    SOURCES
        clipboard_history_plugin.h
        clipboard_history_plugin.cpp
        clipboard_history_plugin.json

        "${CMAKE_SOURCE_DIR}/plugins/common/ClipboardHistoryStore.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/ClipboardHistoryStore.cpp"

        # 写回剪贴板 + 读回校验 + 注入 Ctrl+V（与取词类插件共用，失败回退行为才一致）
        "${CMAKE_SOURCE_DIR}/plugins/common/ClipboardTools.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/ClipboardTools.cpp"
)
