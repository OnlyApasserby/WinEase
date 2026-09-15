# ============================================================================
#  P2-13 Hosts 快速编辑（dev.hosts_editor）
#
#  共享件 plugins/common/HostsDocument.*（插件与端到端自检共用同一份源码）：
#  解析 / 校验 / 渲染全在这里，自检直接对纯函数断言。
#  写盘经提权助手的 writeHosts（含字节级 base64 通道），读盘用普通文件 IO。
# ============================================================================

winease_add_plugin(dev_hosts_editor
    CLASS_NAME HostsEditorPlugin
    SOURCES
        hosts_editor_plugin.h
        hosts_editor_plugin.cpp
        hosts_editor_plugin.json

        "${CMAKE_SOURCE_DIR}/plugins/common/HostsDocument.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/HostsDocument.cpp"
)
