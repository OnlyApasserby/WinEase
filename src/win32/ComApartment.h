#pragma once

// ============================================================================
//  ComApartment.h —— COM 套间初始化守卫（RAII）
//
//  为什么需要它：
//      Core Audio、DDC/CI、Shell、WinRT 全都要求调用线程已初始化 COM，
//      且**套间模型必须正确**。Qt 的 Windows 平台插件可能已经调用过
//      OleInitialize（隐式 STA），此时 CoInitializeEx 返回 S_FALSE，
//      这属于"已初始化"，依然是成功路径——很容易被误判为失败。
//
//  使用约定（务必遵守）：
//      1. main() 中、创建 QApplication **之后**尽早调用一次：
//             auto com = WinEase::Win32::ComApartment::initialize();
//         并让返回值存活到程序结束（RAII 自动平衡 CoUninitialize）
//      2. 插件在自己的工作线程调用 COM 前，也要为该线程初始化一次
//      3. 绝不要在未检查 result() 的情况下继续调用 COM API
//
//  返回值语义：
//      S_OK       —— 本次调用完成了初始化，析构时需要 CoUninitialize
//      S_FALSE    —— 线程已初始化过同型号套间；仍成对 CoUninitialize 保持计数平衡
//      RPC_E_CHANGED_MODE —— 线程已处于**另一种**套间（通常是 MTA），
//                    无法切换为 STA。这是**不可恢复**冲突，必须记录并按能力降级。
// ============================================================================

#include <QString>

#include <windows.h>

namespace WinEase::Win32 {

/// 请求的套间模型
enum class ApartmentModel {
    SingleThreaded, ///< STA（COINIT_APARTMENTTHREADED）—— WinRT UI 类、Shell 扩展推荐
    MultiThreaded   ///< MTA（COINIT_MULTITHREADED）—— 后台工作线程推荐
};

/// 当前线程的套间状态（只读探测结果）
enum class ApartmentState {
    NotInitialized, ///< 尚未初始化 COM
    SingleThreaded, ///< STA（含 Main STA）
    MultiThreaded,  ///< MTA
    Unknown         ///< 查询失败
};

/// COM 套间 RAII 守卫（不可拷贝，可移动）
class ComApartment
{
public:
    ComApartment() = default;
    ~ComApartment();

    ComApartment(const ComApartment &) = delete;
    ComApartment &operator=(const ComApartment &) = delete;
    ComApartment(ComApartment &&other) noexcept;
    ComApartment &operator=(ComApartment &&other) noexcept;

    /// 初始化当前线程的 COM 套间
    static ComApartment initialize(ApartmentModel model = ApartmentModel::SingleThreaded);

    /// 本次调用是否成功（S_OK 或 S_FALSE）
    bool succeeded() const;

    /// 是否为"线程早已初始化"的情况（S_FALSE）
    bool alreadyInitialized() const;

    /// 原始 HRESULT
    HRESULT result() const { return m_result; }

    /// 中文诊断信息，可直接写日志或提示用户
    QString message() const;

    /// 是否持有初始化计数（析构时需要 CoUninitialize）
    bool ownsInitialization() const { return m_ownsInitialization; }

private:
    HRESULT m_result = E_PENDING;
    bool m_ownsInitialization = false;
};

/// 探测当前线程的套间状态（不改变任何状态）
ApartmentState currentApartmentState();

/// 当前线程是否已满足 COM 调用前提（STA 或 MTA 均可）
bool isThreadComReady();

/// 套间状态的中文描述，便于日志排障
QString apartmentStateText(ApartmentState state);

} // namespace WinEase::Win32
