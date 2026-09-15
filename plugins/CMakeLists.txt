# ============================================================================
#  功能插件目录
#
#  每个具体功能以独立子目录 + 独立动态库的形式存在，构建后统一输出到
#  <build>/bin/plugins，由主程序启动时通过 QPluginLoader 扫描加载。
#
#  新增插件步骤：
#    1. 复制 plugins/template 目录并重命名
#    2. 修改类名、id()、name() 等元信息
#    3. 在下方登记 add_subdirectory(<插件目录>)
# ============================================================================

# ---- 示例：架构确认后逐个下发并在此登记 ------------------------------------
# add_subdirectory(window_toolbox)      # 窗口管理
# add_subdirectory(clipboard_history)   # 输入效率
# add_subdirectory(file_quick_actions)  # 文件增强
# add_subdirectory(system_monitor)      # 系统监控
# add_subdirectory(launcher)            # 启动器

# 模板工程默认不参与构建，仅在编译期作为参考（去掉注释即可启用验证）
# # ---------------------------------------------------------------------------
#  P1 功能插件（按批次登记）
# ---------------------------------------------------------------------------

# P1-A 窗口组：四个插件共用"目标窗口"解析语义（src/win32/WindowTarget.h）
add_subdirectory(window_always_on_top)   # P1-01 窗口置顶
add_subdirectory(window_snap_layout)     # P1-02 窗口快速分屏
add_subdirectory(window_opacity)         # P1-03 窗口透明度
add_subdirectory(window_bottom)          # P1-04 窗口置底

# P1-B 工具组：都是"工具面板 + 数据转换"，不碰系统状态（唯 P1-12 的"结束进程"走提权助手）
add_subdirectory(monitor_color_picker)   # P1-06 屏幕取色器
add_subdirectory(dev_text_format)        # P1-10 JSON / XML 格式化
add_subdirectory(dev_encoder)            # P1-11 编码转换
add_subdirectory(dev_port_viewer)        # P1-12 端口占用查看

# P1-C 悬浮层组：基于 OverlayKit，两个插件都自己 new 悬浮层子类再交给 OverlayHost 托管
add_subdirectory(display_ruler)           # P1-07 屏幕标尺
add_subdirectory(display_focus_highlight) # P1-08 焦点高亮

# P1-D 系统组：都要**写系统状态**（注册表 / 桌面壁纸），硬指标是"停用即完整还原"
add_subdirectory(file_context_menu)       # P1-05 右键菜单扩展（HKCU 静态 verb）
add_subdirectory(launcher_web_search)     # P1-09 网页快速搜索
add_subdirectory(personal_theme_switch)   # P1-13 系统主题切换（注册表 + 广播）
add_subdirectory(personal_wallpaper)      # P1-14 壁纸自动切换（IDesktopWallpaper）

# ---------------------------------------------------------------------------
#  P2 功能插件（按批次登记）
# ---------------------------------------------------------------------------

# P2-A 文件组：处理"用户自己的文件"，纪律最硬 —— 能撤销的必须能撤销（P2-01），
# 不能撤销的必须把话说明白（P2-12），绝不做静默破坏
add_subdirectory(file_batch_rename)      # P2-01 批量重命名
add_subdirectory(file_quick_look)        # P2-02 快速文件预览
add_subdirectory(file_duplicate_finder)  # P2-03 重复文件查找
add_subdirectory(security_file_shredder) # P2-12 敏感文件粉碎

# P2-B 输入组：都盯着"用户正在输入的那一刻" ——
# 剪贴板历史的红线是**隐私**（标着"不记录"的内容一个字都不落盘），
# 滚轮增强的红线是**只吞自己该吞的事件**（不在任务栏上就绝不插手）。
add_subdirectory(input_clipboard_history) # P2-04 剪贴板历史（含剪贴板隐私清理，D4）
add_subdirectory(input_wheel_enhance)     # P2-05 滚轮增强（任务栏滚动调音量）

# P2-C 显示与媒体组：都直接改**系统显示/音频状态**，红线各不相同 ——
# 麦克风静音：绝不自己记一个状态（外部改动 / 会议软件 一定会和它分叉），
# 且停用时**不许**自动开麦（fail-closed）；
# 亮度色温：停用必须把 gamma 还原到原样；
# 放大镜：不许自抓屏（要 Magnification API）。
add_subdirectory(media_mic_mute)          # P2-10 麦克风一键静音
add_subdirectory(display_magnifier)       # P2-09 放大镜增强（Magnification API）
add_subdirectory(display_brightness)      # P2-08 亮度色温护眼（DDC/CI + WMI 亮度、gamma 色温）

# ---------------------------------------------------------------------------
#  P2-D 系统与媒体组：都"改变系统状态"且后果不可撤销，纪律是**先给反悔窗口**。
#  电源动作走提权助手（除锁定），但**取消**走本地实现 —— 撤销通道不能依赖另一个进程
#  还活着；定时任务的"到点"决策做成纯函数，休眠唤醒后错过的任务必须补触发。
# ---------------------------------------------------------------------------
add_subdirectory(monitor_power_panel)     # P2-06 快速关机（倒计时可取消）
add_subdirectory(monitor_scheduler)       # P2-07 定时任务（提醒/静音/电源，含补触发）

# ---------------------------------------------------------------------------
#  P2-E 安全与隐私：本功能**对注册表只读**（区别于其它批次"写系统状态"的形态），
#  纪律是"读不到 ≠ 没人用"，以及"同一个应用只提醒一次"（不然用户第一件事就是关掉它）
# ---------------------------------------------------------------------------
add_subdirectory(security_device_alert)   # P2-11 摄像头/麦克风使用提醒

# ---------------------------------------------------------------------------
#  P3 高风险波次（每项都已做过预研 spike，结论见 ROADMAP 的 P3 结论表）
# ---------------------------------------------------------------------------
add_subdirectory(file_unlock)            # P3-02 文件锁定解除（查出占用者 → 结束 → 移到回收站）
add_subdirectory(window_virtual_desktop) # P3-01 虚拟桌面增强（探测只读；切换靠模拟系统快捷键）
add_subdirectory(security_usb_control)   # P3-14 USB 设备管控（判据在平台层 StorageDevices）
add_subdirectory(file_convert)           # P3-03 批量格式转换（图片格式 + 文本字符集，进度可取消）
add_subdirectory(monitor_hardware_hud)   # P3-07 硬件监控悬浮窗（第一批：CPU/内存/网速/GPU/磁盘温度）
add_subdirectory(media_mixer)            # P3-09 音量混合器（会话音量/静音；按应用合并同类会话）
add_subdirectory(media_player_panel)     # P3-11 媒体控制面板（SMTC 会话控制 + 系统媒体键，两组分开）

# ---------------------------------------------------------------------------
#  P2-F 开发运维组：改的是"能让用户上不了网 / 找不到程序"的文件与变量，
#  所以纪律是**校验不过就绝不落笔**、**写前先备份**、**往返无损**
# ---------------------------------------------------------------------------
add_subdirectory(dev_hosts_editor)        # P2-13 Hosts 快速编辑（写前自动备份 + 可还原）
add_subdirectory(dev_env_manager)         # P2-14 环境变量管理（用户级直写 / 系统级走助手）

add_subdirectory(template)
