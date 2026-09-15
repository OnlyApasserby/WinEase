# ============================================================================
#  installer_smoke —— 打包链路自检（自解压安装程序）
#
#  与其它自检的区别：它**不加载插件、不碰真实系统状态**，而是拿"用户会拿到的
#  那个单文件 exe"当被测对象，验四件事：
#      ① 负载完整性（逐文件 SHA-256）
#      ② **依赖审计**（每个 PE 的每条非系统导入都能在负载内找到 —— 离线可用性的硬证据）
#      ③ 安装 / 注册 / 卸载的**真实状态**（临时目录 + HKCU 卸载项 + 开始菜单快捷方式）
#      ④ 桥接自检（证明 Qt/VC/.NET 三层依赖在目标目录里真能协同工作）
#
#  ⚠ 它需要**先有安装包**：先跑 `cmake --build build --target winease_installer`，
#     否则第一条断言（前置）就会红 —— 这是刻意的：包装不出来就等于交付链断了。
# ============================================================================

qt_add_executable(installer_smoke
    main.cpp
)

target_link_libraries(installer_smoke PRIVATE
    Qt6::Core
    shell32      # SHGetKnownFolderPath（开始菜单目录）
    ole32        # CoTaskMemFree
    advapi32     # 注册表（卸载项）
)

set_target_properties(installer_smoke PROPERTIES
    OUTPUT_NAME "installer_smoke"
    WIN32_EXECUTABLE FALSE   # 控制台子系统：直接看得到输出
)
