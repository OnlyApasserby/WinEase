# ============================================================================
#  net_lan_transfer —— 局域网跨平台文件传输
#
#  引擎 LanTransferEngine 住在 plugins/common，被**插件与自检编译同一份**：
#  HTTP 头解析、路径穿越拦截、multipart 边界这些最容易出安全问题的地方，
#  必须能被自检用构造数据反复捶（而不是只能靠真手机试）。
# ============================================================================

winease_add_plugin(we_net_lan_transfer
    CLASS_NAME LanTransferPlugin
    SOURCES
        lan_transfer_plugin.h
        lan_transfer_plugin.cpp
        lan_transfer_plugin.json
        lan_http_server.h
        lan_http_server.cpp

        "${CMAKE_SOURCE_DIR}/plugins/common/LanTransferEngine.h"
        "${CMAKE_SOURCE_DIR}/plugins/common/LanTransferEngine.cpp"
)

# 服务端要用到网络模块（QTcpServer / QUdpSocket / QNetworkInterface）
target_link_libraries(we_net_lan_transfer PRIVATE Qt6::Network)
