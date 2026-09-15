// ============================================================================
//  WinEaseLiteMonitorBridge —— C++/CLI 桥接层（P3-07 硬件监控）
//
//  【为什么需要这一层】
//  WinEase 是原生 C++20 / Qt 程序，而硬件传感器读数这件事上，
//  `refrences/LiteMonitor`（.NET 8 + LibreHardwareMonitorLib）已经把
//  CPU / GPU / 主板（SuperIO）/ 磁盘 / 网卡 / 电池 的采集与命名逻辑做全了：
//  `Computer` 开关 → `Open()` → 逐硬件 `Update()` → 遍历 `ISensor`。
//  C++ 侧没有等价物（要拿到 CPU 温度得自己写 MSR / SuperIO / NVML），
//  所以这里用 **C++/CLI 混合模式程序集** 桥接：
//
//      原生 C++20（WinEaseWin32 / 插件）
//          │  LoadLibrary + 纯 C ABI（本文件导出）
//          ▼
//      WinEaseLiteMonitorBridge.dll（/clr，加载时会把 CLR 带进进程）
//          │  直接引用托管类型（同一进程里就是普通函数调用）
//          ▼
//      LibreHardwareMonitorLib.dll（与 LiteMonitor 同一个 NuGet 包 0.9.6）
//
//  【三条硬纪律】
//  1. **导出的一律是纯 C ABI**：只传 POD 与 `wchar_t*`，绝不跨边界传托管对象 /
//     `String^` / 委托。跨 ABI 传托管类型是这类桥接最常见的崩法。
//  2. **字符串由本层分配、由本层释放**：快照里的 `wchar_t*` 用 `Marshal::StringToHGlobalUni`
//     分配，下一次 refresh / close 时统一归还；原生侧**只读取、不释放**。
//  3. **任何异常都不许越过 ABI 边界**：CLR 异常必须在这里转成返回值 + 中文原因串，
//     否则原生侧看到的是进程级的 `SEHException`（插件会被崩溃隔离）。
//
//  【构建约定】
//  * 用 `/clr`（混合模式，.NET Framework 4.x 的 IJW）—— 原生 exe 直接
//    `LoadLibrary` 即可把 CLR 拉起来，不需要宿主自己托管运行时。
//  * LibreHardwareMonitorLib 通过 `/FU`（强制 `#using`）引用，
//    其程序集由 `src/bridge/managed/` 下的 csproj 还原后复制到 `build/bin`。
//  * 与全局的 `/EHsc` 冲突：`/clr` 只接受 `/EHa`，且不接受 `/permissive-` / `/RTC1`，
//    这些在 CMake 里对本目标单独改写。
// ============================================================================

#include <cstdlib>
#include <cstring>

#using <System.dll>
#using <System.Core.dll>

using namespace System;
using namespace System::Collections::Generic;
using namespace System::Runtime::InteropServices;
using namespace System::Reflection;
using namespace LibreHardwareMonitor::Hardware;

// ---------------------------------------------------------------------------
//  跨 ABI 的数据结构（纯 POD，原生侧头文件里有一份一致的声明）
// ---------------------------------------------------------------------------
#pragma pack(push, 8)
struct WeLmSensor {
    const wchar_t* hardware;    // 所属硬件名（如 "AMD Ryzen 7 ..." / "NVIDIA ..."）
    const wchar_t* name;        // 传感器名（如 "CPU Package" / "GPU Core"）
    const wchar_t* identifier;  // LHM 的唯一标识（原生侧据此做稳定排序/选择）
    int            kind;        // 传感器类型（见 LiteMonitorBridge.h 的 SensorKind）
    int            hasValue;    // 1 = 读到了值；0 = 该传感器当前没给值
    double         value;       // hasValue 为 1 时有效
};
#pragma pack(pop)

#define WELM_EXPORT extern "C" __declspec(dllexport)

// ---------------------------------------------------------------------------
//  托管侧会话：一个 Computer 实例 + 一份本机快照 + 最后一次错误原因
// ---------------------------------------------------------------------------
// 注意：本文件以 UTF-8 保存，必须用 /utf-8 编译（否则中文字符串字面量会被
// 按代码页 936 解释，UTF-8 字节里出现的 0x5C 会把字面量提前闭合，报 C2001）。
ref class LmSession {
public:
    // 中间结构：先把托管字符串攒起来，最后一次性拷到非托管内存。
    // （原生 struct 不能做 List<> 的泛型参数，所以走这个托管中间层。）
    value struct Mraw {
        String^ Hardware;
        String^ Name;
        String^ Identifier;
        int    Kind;
        int    HasValue;
        double Value;
    };

    Computer^   computer;
    bool        opened;
    bool        openedFailed;
    String^     lastError;
    WeLmSensor* rows;          // 非托管内存，由本对象负责释放
    int         count;

    LmSession() {
        rows = nullptr;
        count = 0;
        opened = false;
        openedFailed = false;
        lastError = nullptr;
        computer = gcnew Computer();
        // 与 LiteMonitor 的 HardwareMonitor 相同的开关策略：
        // CPU / GPU / 内存 / 主板 / 存储 / 网络 / 电池 常开；
        // Controller（USB 类风扇控制器）默认关（避免 USB 设备冲突，且我们只要读数）；
        // PSU（电源模块）关（高端 Corsair 电源才用得上）。
        computer->IsCpuEnabled = true;
        computer->IsGpuEnabled = true;
        computer->IsMemoryEnabled = true;
        computer->IsMotherboardEnabled = true;
        computer->IsStorageEnabled = true;
        computer->IsNetworkEnabled = true;
        computer->IsBatteryEnabled = true;
        computer->IsControllerEnabled = false;
        computer->IsPsuEnabled = false;
    }

    void SetError(String^ message) {
        lastError = message;
    }

    void SetError(Exception^ ex) {
        if (ex == nullptr) return;
        String^ inner = ex->InnerException == nullptr ? nullptr : ex->InnerException->Message;
        lastError = inner == nullptr ? ex->Message : ex->Message + L"（内部异常：" + inner + L"）";
    }

    void ReleaseRows() {
        if (rows != nullptr) {
            for (int i = 0; i < count; ++i) {
                if (rows[i].hardware != nullptr) Marshal::FreeHGlobal(IntPtr((void*)rows[i].hardware));
                if (rows[i].name != nullptr) Marshal::FreeHGlobal(IntPtr((void*)rows[i].name));
                if (rows[i].identifier != nullptr) Marshal::FreeHGlobal(IntPtr((void*)rows[i].identifier));
            }
            std::free(rows);
            rows = nullptr;
        }
        count = 0;
    }

    static const wchar_t* Dup(String^ value) {
        if (String::IsNullOrEmpty(value)) return nullptr;
        return (const wchar_t*)Marshal::StringToHGlobalUni(value).ToPointer();
    }

    // 与 LiteMonitor 的 HardwareScanner.GenerateSmartName 同思路：
    // SuperIO 芯片名（"Nuvoton NCT6798D"）对用户没有意义，换成主板名。
    String^ SmartName(IHardware^ hardware, IComputer^ computerRef) {
        String^ hwName = hardware->Name == nullptr ? String::Empty : hardware->Name;
        if (hardware->HardwareType == HardwareType::SuperIO) {
            for each (IHardware^ h in computerRef->Hardware) {
                if (h->HardwareType == HardwareType::Motherboard) {
                    if (!String::IsNullOrEmpty(h->Name)) hwName = h->Name;
                    break;
                }
            }
        }
        return hwName;
    }

    // ⚠ 托管类的成员函数**必须就地定义**：C++/CLI 不允许像原生类那样
    //   把函数体写在类外（写了会得到一堆莫名其妙的 "未声明的标识符"）。
    void Collect(IHardware^ hardware, List<Mraw>^ sink) {
        if (hardware->Sensors == nullptr) return;
        String^ hwName = SmartName(hardware, computer);
        for each (ISensor^ s in hardware->Sensors) {
            Mraw m;
            m.Hardware = hwName;
            m.Name = s->Name == nullptr ? String::Empty : s->Name;
            m.Identifier = s->Identifier == nullptr ? String::Empty : s->Identifier->ToString();
            m.Kind = (int)s->SensorType;
            m.HasValue = s->Value.HasValue ? 1 : 0;
            m.Value = s->Value.HasValue ? (double)s->Value.Value : 0.0;
            sink->Add(m);
        }
    }

    int Refresh() {
        try {
            if (openedFailed) return -1;
            if (!opened) {
                computer->Open();
                opened = true;
            }

            List<Mraw>^ collected = gcnew List<Mraw>();
            for each (IHardware^ hw in computer->Hardware) {
                UpdateTree(hw, collected);
            }

            ReleaseRows();
            count = collected->Count;
            if (count > 0) {
                rows = (WeLmSensor*)std::calloc((size_t)count, sizeof(WeLmSensor));
                if (rows == nullptr) {
                    count = 0;
                    SetError(L"桥接层内存不足，无法生成传感器快照。");
                    return -1;
                }
                for (int i = 0; i < count; ++i) {
                    Mraw m = collected[i];
                    rows[i].hardware = Dup(m.Hardware);
                    rows[i].name = Dup(m.Name);
                    rows[i].identifier = Dup(m.Identifier);
                    rows[i].kind = m.Kind;
                    rows[i].hasValue = m.HasValue;
                    rows[i].value = m.Value;
                }
            }
            lastError = nullptr;
            return count;
        }
        catch (Exception^ ex) {
            SetError(ex);
            return -1;
        }
    }

    void UpdateTree(IHardware^ hardware, List<Mraw>^ sink) {
        if (hardware == nullptr) return;
        try {
            hardware->Update();
        }
        catch (Exception^) {
            // 单个硬件刷新失败（显卡掉线 / 驱动拒绝）不该拖垮整份快照
        }
        Collect(hardware, sink);
        if (hardware->SubHardware == nullptr) return;
        for each (IHardware^ sub in hardware->SubHardware) {
            UpdateTree(sub, sink);
        }
    }

    void Close() {
        ReleaseRows();
        if (computer != nullptr) {
            try {
                if (opened) computer->Close();
            }
            catch (Exception^ ex) {
                SetError(ex);
            }
        }
        opened = false;
    }
};

// ---------------------------------------------------------------------------
//  导出的纯 C ABI（见文件头"三条硬纪律"）
// ---------------------------------------------------------------------------
static GCHandle HandleOf(void* handle) {
    return GCHandle::FromIntPtr(IntPtr(handle));
}

WELM_EXPORT void* we_lm_open() {
    LmSession^ session = nullptr;
    try {
        session = gcnew LmSession();
        GCHandle h = GCHandle::Alloc(session);
        return GCHandle::ToIntPtr(h).ToPointer();
    }
    catch (Exception^) {
        return nullptr;
    }
}

WELM_EXPORT void we_lm_close(void* handle) {
    if (handle == nullptr) return;
    GCHandle h = HandleOf(handle);
    try {
        LmSession^ session = safe_cast<LmSession^>(h.Target);
        session->Close();
    }
    catch (Exception^) {
    }
    h.Free();
}

WELM_EXPORT int we_lm_refresh(void* handle) {
    if (handle == nullptr) return -1;
    try {
        LmSession^ session = safe_cast<LmSession^>(HandleOf(handle).Target);
        return session->Refresh();
    }
    catch (Exception^) {
        return -1;
    }
}

WELM_EXPORT int we_lm_sensor_count(void* handle) {
    if (handle == nullptr) return 0;
    try {
        LmSession^ session = safe_cast<LmSession^>(HandleOf(handle).Target);
        return session->count;
    }
    catch (Exception^) {
        return 0;
    }
}

WELM_EXPORT int we_lm_sensor_at(void* handle, int index, WeLmSensor* outRow) {
    if (handle == nullptr || outRow == nullptr) return 0;
    try {
        LmSession^ session = safe_cast<LmSession^>(HandleOf(handle).Target);
        if (index < 0 || index >= session->count || session->rows == nullptr) return 0;
        *outRow = session->rows[index];
        return 1;
    }
    catch (Exception^) {
        return 0;
    }
}

WELM_EXPORT int we_lm_last_error(void* handle, wchar_t* buffer, int capacity) {
    String^ message = nullptr;
    if (handle != nullptr) {
        try {
            LmSession^ session = safe_cast<LmSession^>(HandleOf(handle).Target);
            message = session->lastError;
        }
        catch (Exception^) {
            message = nullptr;
        }
    }
    if (String::IsNullOrEmpty(message)) {
        if (buffer != nullptr && capacity > 0) buffer[0] = L'\0';
        return 0;
    }
    array<wchar_t>^ chars = message->ToCharArray();
    int need = chars->Length + 1;
    if (buffer != nullptr && capacity > 0) {
        int copy = need <= capacity ? chars->Length : capacity - 1;
        pin_ptr<wchar_t> pinned = &chars[0];
        std::memcpy(buffer, pinned, (size_t)copy * sizeof(wchar_t));
        buffer[copy] = L'\0';
    }
    return need;
}

WELM_EXPORT int we_lm_runtime_info(wchar_t* buffer, int capacity) {
    String^ info = nullptr;
    try {
        String^ lhm = L"未知";
        try {
            Assembly^ asm_ = Assembly::GetAssembly(Computer::typeid);
            if (asm_ != nullptr) lhm = asm_->GetName()->Version->ToString();
        }
        catch (Exception^) {
        }
        // 注意：/clr:netcore 下 Environment::Version 报的是 CoreCLR 的版本，
        // 写 ".NET Framework" 会误导排查，这里如实按"CLR"表述。
        info = L"C++/CLI 桥接（CLR " + Environment::Version->ToString() +
               L" / LibreHardwareMonitorLib " + lhm + L"）";
    }
    catch (Exception^) {
        info = L"C++/CLI 桥接（运行时信息读取失败）";
    }
    array<wchar_t>^ chars = info->ToCharArray();
    int need = chars->Length + 1;
    if (buffer != nullptr && capacity > 0) {
        int copy = need <= capacity ? chars->Length : capacity - 1;
        pin_ptr<wchar_t> pinned = &chars[0];
        std::memcpy(buffer, pinned, (size_t)copy * sizeof(wchar_t));
        buffer[copy] = L'\0';
    }
    return need;
}
