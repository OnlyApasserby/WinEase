#include "win32/RegistryUtils.h"

#include "win32/Win32Error.h"

#include <QDir>

#include <utility>
#include <vector>

namespace WinEase::Win32 {

namespace {

HKEY rootToNative(RegistryRoot root)
{
    return (root == RegistryRoot::CurrentUser) ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
}

REGSAM viewToNative(RegistryView view)
{
    switch (view) {
    case RegistryView::Force64:
        return KEY_WOW64_64KEY;
    case RegistryView::Force32:
        return KEY_WOW64_32KEY;
    case RegistryView::Default:
        break;
    }
    return 0;
}

/// 把 QVariant 推断为注册表写入类型
DWORD resolveWriteType(const QVariant &value, RegistryValueType requested)
{
    if (requested != RegistryValueType::Auto) {
        switch (requested) {
        case RegistryValueType::String:
            return REG_SZ;
        case RegistryValueType::ExpandString:
            return REG_EXPAND_SZ;
        case RegistryValueType::DWord:
            return REG_DWORD;
        case RegistryValueType::QWord:
            return REG_QWORD;
        case RegistryValueType::Binary:
            return REG_BINARY;
        case RegistryValueType::MultiString:
            return REG_MULTI_SZ;
        case RegistryValueType::Auto:
            break;
        }
    }

    switch (value.metaType().id()) {
    case QMetaType::Bool:
    case QMetaType::Int:
    case QMetaType::UInt:
        return REG_DWORD;
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return REG_QWORD;
    case QMetaType::QByteArray:
        return REG_BINARY;
    case QMetaType::QStringList:
        return REG_MULTI_SZ;
    default:
        break;
    }
    return REG_SZ;
}

/// 把原始字节流按类型转成 QVariant
QVariant decodeValue(DWORD type, const std::vector<BYTE> &data)
{
    switch (type) {
    case REG_SZ:
    case REG_EXPAND_SZ:
        // 已经保证以双字节 0 结尾，直接按 wchar_t 读
        if (data.size() < sizeof(wchar_t)) {
            return QString();
        }
        return QString::fromWCharArray(reinterpret_cast<const wchar_t *>(data.data()),
                                       static_cast<int>(data.size() / sizeof(wchar_t)) - 1);

    case REG_DWORD: {
        if (data.size() < sizeof(DWORD)) {
            return QVariant();
        }
        DWORD raw = 0;
        ::memcpy(&raw, data.data(), sizeof(raw));
        return static_cast<uint>(raw);
    }

    case REG_QWORD: {
        if (data.size() < sizeof(ULONGLONG)) {
            return QVariant();
        }
        ULONGLONG raw = 0;
        ::memcpy(&raw, data.data(), sizeof(raw));
        return static_cast<qulonglong>(raw);
    }

    case REG_BINARY:
        return QByteArray(reinterpret_cast<const char *>(data.data()),
                          static_cast<qsizetype>(data.size()));

    case REG_MULTI_SZ: {
        QStringList items;
        if (data.size() < sizeof(wchar_t) * 2U) {
            return items;
        }
        const auto *cursor = reinterpret_cast<const wchar_t *>(data.data());
        const auto total = static_cast<qsizetype>(data.size() / sizeof(wchar_t));
        qsizetype index = 0;
        while (index < total && cursor[index] != L'\0') {
            const QString item = QString::fromWCharArray(cursor + index);
            items.append(item);
            index += item.size() + 1;
        }
        return items;
    }

    default:
        break;
    }
    return QVariant();
}

} // namespace

// ============================================================================
//  RegistryKey
// ============================================================================

RegistryKey::RegistryKey(RegistryKey &&other) noexcept
    : m_key(other.m_key)
    , m_error(std::move(other.m_error))
{
    other.m_key = nullptr;
}

RegistryKey &RegistryKey::operator=(RegistryKey &&other) noexcept
{
    if (this != &other) {
        if (m_key != nullptr) {
            ::RegCloseKey(m_key);
        }
        m_key = other.m_key;
        m_error = std::move(other.m_error);
        other.m_key = nullptr;
    }
    return *this;
}

RegistryKey::~RegistryKey()
{
    if (m_key != nullptr) {
        ::RegCloseKey(m_key);
        m_key = nullptr;
    }
}

RegistryKey RegistryKey::open(RegistryRoot root,
                              const QString &subKey,
                              bool readOnly,
                              RegistryView view,
                              QString *errorOut)
{
    RegistryKey result;

    REGSAM access = readOnly ? KEY_READ : (KEY_READ | KEY_WRITE);
    access |= viewToNative(view);

    HKEY handle = nullptr;
    const LSTATUS status = ::RegOpenKeyExW(rootToNative(root),
                                           reinterpret_cast<LPCWSTR>(subKey.utf16()),
                                           0,
                                           access,
                                           &handle);
    if (status != ERROR_SUCCESS) {
        result.m_error = describeFailure(QStringLiteral("打开注册表项 %1").arg(subKey), status);
        if (errorOut != nullptr) {
            *errorOut = result.m_error;
        }
        return result;
    }

    result.m_key = handle;
    return result;
}

RegistryKey RegistryKey::create(RegistryRoot root,
                                const QString &subKey,
                                RegistryView view,
                                QString *errorOut)
{
    RegistryKey result;

    REGSAM access = KEY_READ | KEY_WRITE | viewToNative(view);

    HKEY handle = nullptr;
    DWORD disposition = 0;
    const LSTATUS status = ::RegCreateKeyExW(rootToNative(root),
                                             reinterpret_cast<LPCWSTR>(subKey.utf16()),
                                             0,
                                             nullptr,
                                             REG_OPTION_NON_VOLATILE,
                                             access,
                                             nullptr,
                                             &handle,
                                             &disposition);
    if (status != ERROR_SUCCESS) {
        result.m_error = describeFailure(QStringLiteral("创建注册表项 %1").arg(subKey), status);
        if (errorOut != nullptr) {
            *errorOut = result.m_error;
        }
        return result;
    }

    result.m_key = handle;
    return result;
}

QVariant RegistryKey::value(const QString &name, const QVariant &defaultValue) const
{
    if (m_key == nullptr) {
        return defaultValue;
    }

    const auto *valueName = reinterpret_cast<LPCWSTR>(name.isEmpty() ? nullptr : name.utf16());

    DWORD type = 0;
    DWORD size = 0;
    LSTATUS status = ::RegQueryValueExW(m_key, valueName, nullptr, &type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        return defaultValue;
    }
    if (status != ERROR_SUCCESS) {
        m_error = describeFailure(QStringLiteral("查询注册表值 %1").arg(name), status);
        return defaultValue;
    }

    std::vector<BYTE> buffer(static_cast<size_t>(size) + sizeof(wchar_t) * 2U, 0);
    status = ::RegQueryValueExW(m_key,
                                valueName,
                                nullptr,
                                &type,
                                buffer.data(),
                                &size);
    if (status != ERROR_SUCCESS) {
        m_error = describeFailure(QStringLiteral("读取注册表值 %1").arg(name), status);
        return defaultValue;
    }

    buffer.resize(static_cast<size_t>(size));
    const QVariant decoded = decodeValue(type, buffer);
    return decoded.isValid() ? decoded : defaultValue;
}

QString RegistryKey::expandedValue(const QString &name, const QString &defaultValue) const
{
    const QVariant raw = value(name);
    if (!raw.isValid() || raw.metaType().id() != QMetaType::QString) {
        return defaultValue;
    }

    const QString text = raw.toString();
    if (!text.contains(QLatin1Char('%'))) {
        return text;
    }

    const DWORD required = ::ExpandEnvironmentStringsW(reinterpret_cast<LPCWSTR>(text.utf16()),
                                                       nullptr,
                                                       0);
    if (required == 0U) {
        return text;
    }

    std::vector<wchar_t> expanded(static_cast<size_t>(required), L'\0');
    const DWORD written = ::ExpandEnvironmentStringsW(reinterpret_cast<LPCWSTR>(text.utf16()),
                                                      expanded.data(),
                                                      required);
    if (written == 0U) {
        return text;
    }
    return QString::fromWCharArray(expanded.data());
}

bool RegistryKey::hasValue(const QString &name) const
{
    if (m_key == nullptr) {
        return false;
    }
    const auto *valueName = reinterpret_cast<LPCWSTR>(name.isEmpty() ? nullptr : name.utf16());
    return ::RegQueryValueExW(m_key, valueName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

bool RegistryKey::setValue(const QString &name,
                           const QVariant &value,
                           RegistryValueType type,
                           QString *errorOut)
{
    if (m_key == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("注册表键无效，无法写入");
        }
        return false;
    }

    const DWORD nativeType = resolveWriteType(value, type);
    const auto *valueName = reinterpret_cast<LPCWSTR>(name.isEmpty() ? nullptr : name.utf16());

    LSTATUS status = ERROR_SUCCESS;

    switch (nativeType) {
    case REG_DWORD: {
        const DWORD raw = static_cast<DWORD>(value.toUInt());
        status = ::RegSetValueExW(m_key, valueName, 0, REG_DWORD,
                                  reinterpret_cast<const BYTE *>(&raw), sizeof(raw));
        break;
    }
    case REG_QWORD: {
        const ULONGLONG raw = static_cast<ULONGLONG>(value.toULongLong());
        status = ::RegSetValueExW(m_key, valueName, 0, REG_QWORD,
                                  reinterpret_cast<const BYTE *>(&raw), sizeof(raw));
        break;
    }
    case REG_BINARY: {
        const QByteArray bytes = value.toByteArray();
        status = ::RegSetValueExW(m_key, valueName, 0, REG_BINARY,
                                  reinterpret_cast<const BYTE *>(bytes.constData()),
                                  static_cast<DWORD>(bytes.size()));
        break;
    }
    case REG_MULTI_SZ: {
        const QStringList items = value.toStringList();
        std::vector<wchar_t> buffer;
        // 注意：不能直接用 QString 的迭代器插入 std::vector<wchar_t>——
        // QString 的 value_type 是 char16_t，与 wchar_t 不是同一类型，无隐式转换。
        for (const QString &item : items) {
            for (const QChar character : item) {
                buffer.push_back(static_cast<wchar_t>(character.unicode()));
            }
            buffer.push_back(L'\0');
        }
        buffer.push_back(L'\0'); // 双 0 结尾
        status = ::RegSetValueExW(m_key, valueName, 0, REG_MULTI_SZ,
                                  reinterpret_cast<const BYTE *>(buffer.data()),
                                  static_cast<DWORD>(buffer.size() * sizeof(wchar_t)));
        break;
    }
    case REG_EXPAND_SZ:
    case REG_SZ:
    default: {
        const QString text = value.toString();
        status = ::RegSetValueExW(m_key, valueName, 0, nativeType,
                                  reinterpret_cast<const BYTE *>(text.utf16()),
                                  static_cast<DWORD>((text.size() + 1) * static_cast<qsizetype>(sizeof(wchar_t))));
        break;
    }
    }

    if (status != ERROR_SUCCESS) {
        m_error = describeFailure(QStringLiteral("写入注册表值 %1").arg(name), status);
        if (errorOut != nullptr) {
            *errorOut = m_error;
        }
        return false;
    }
    return true;
}

bool RegistryKey::removeValue(const QString &name, QString *errorOut)
{
    if (m_key == nullptr) {
        return false;
    }
    const auto *valueName = reinterpret_cast<LPCWSTR>(name.isEmpty() ? nullptr : name.utf16());
    const LSTATUS status = ::RegDeleteValueW(m_key, valueName);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        m_error = describeFailure(QStringLiteral("删除注册表值 %1").arg(name), status);
        if (errorOut != nullptr) {
            *errorOut = m_error;
        }
        return false;
    }
    return true;
}

QStringList RegistryKey::valueNames() const
{
    QStringList names;
    if (m_key == nullptr) {
        return names;
    }

    DWORD subKeyCount = 0;
    DWORD valueCount = 0;
    if (::RegQueryInfoKeyW(m_key, nullptr, nullptr, nullptr, &subKeyCount, nullptr, nullptr,
                           &valueCount, nullptr, nullptr, nullptr, nullptr)
        != ERROR_SUCCESS) {
        return names;
    }

    names.reserve(static_cast<qsizetype>(valueCount));
    for (DWORD index = 0; index < valueCount; ++index) {
        std::vector<wchar_t> buffer(1024, L'\0');
        DWORD length = static_cast<DWORD>(buffer.size());
        if (::RegEnumValueW(m_key, index, buffer.data(), &length, nullptr, nullptr, nullptr, nullptr)
            == ERROR_SUCCESS) {
            names.append(QString::fromWCharArray(buffer.data(), static_cast<int>(length)));
        }
    }
    return names;
}

QStringList RegistryKey::subKeyNames() const
{
    QStringList names;
    if (m_key == nullptr) {
        return names;
    }

    DWORD subKeyCount = 0;
    if (::RegQueryInfoKeyW(m_key, nullptr, nullptr, nullptr, &subKeyCount, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr)
        != ERROR_SUCCESS) {
        return names;
    }

    names.reserve(static_cast<qsizetype>(subKeyCount));
    for (DWORD index = 0; index < subKeyCount; ++index) {
        wchar_t buffer[512] = {};
        DWORD length = 512;
        if (::RegEnumKeyExW(m_key, index, buffer, &length, nullptr, nullptr, nullptr, nullptr)
            == ERROR_SUCCESS) {
            names.append(QString::fromWCharArray(buffer, static_cast<int>(length)));
        }
    }
    return names;
}

bool RegistryKey::removeRecursively(QString *errorOut)
{
    if (m_key == nullptr) {
        return false;
    }
    // RegDeleteTreeW 会一并删除子键与值（Vista 起可用）
    const LSTATUS status = ::RegDeleteTreeW(m_key, nullptr);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        m_error = describeFailure(QStringLiteral("删除注册表项及其子项"), status);
        if (errorOut != nullptr) {
            *errorOut = m_error;
        }
        return false;
    }
    return true;
}

// ============================================================================
//  便捷函数
// ============================================================================

QVariant readValue(RegistryRoot root,
                   const QString &subKey,
                   const QString &name,
                   const QVariant &defaultValue,
                   RegistryView view)
{
    const RegistryKey key = RegistryKey::open(root, subKey, true, view);
    if (!key.isValid()) {
        return defaultValue;
    }
    return key.value(name, defaultValue);
}

bool writeValue(RegistryRoot root,
                const QString &subKey,
                const QString &name,
                const QVariant &value,
                RegistryValueType type,
                RegistryView view,
                QString *errorOut)
{
    RegistryKey key = RegistryKey::create(root, subKey, view, errorOut);
    if (!key.isValid()) {
        return false;
    }
    return key.setValue(name, value, type, errorOut);
}

bool deleteValue(RegistryRoot root,
                 const QString &subKey,
                 const QString &name,
                 RegistryView view,
                 QString *errorOut)
{
    RegistryKey key = RegistryKey::open(root, subKey, false, view);
    if (!key.isValid()) {
        // 键不存在视为"已删除"，不算失败
        return true;
    }
    return key.removeValue(name, errorOut);
}

bool keyExists(RegistryRoot root, const QString &subKey, RegistryView view)
{
    const RegistryKey key = RegistryKey::open(root, subKey, true, view);
    return key.isValid();
}

bool deleteKeyRecursively(RegistryRoot root,
                          const QString &subKey,
                          RegistryView view,
                          QString *errorOut)
{
    // 先以可写方式打开父键，再删除目标子键
    const int separator = subKey.lastIndexOf(QLatin1Char('\\'));
    if (separator < 0) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("无法删除根键");
        }
        return false;
    }

    const QString parentPath = subKey.left(separator);
    const QString leafName = subKey.mid(separator + 1);

    RegistryKey parent = RegistryKey::open(root, parentPath, false, view, errorOut);
    if (!parent.isValid()) {
        return true; // 父键不存在，视作已删除
    }

    const LSTATUS status = ::RegDeleteTreeW(parent.nativeHandle(),
                                            reinterpret_cast<LPCWSTR>(leafName.utf16()));
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        if (errorOut != nullptr) {
            *errorOut = describeFailure(QStringLiteral("删除注册表项 %1").arg(subKey), status);
        }
        return false;
    }
    return true;
}

// ============================================================================
//  变更通知
// ============================================================================

void broadcastSettingChange(const QString &area)
{
    const std::wstring payload = area.toStdWString();
    DWORD_PTR result = 0;
    ::SendMessageTimeoutW(HWND_BROADCAST,
                          WM_SETTINGCHANGE,
                          0,
                          reinterpret_cast<LPARAM>(payload.empty() ? nullptr : payload.c_str()),
                          SMTO_ABORTIFHUNG | SMTO_NORMAL,
                          3000,
                          &result);
}

void refreshDesktop()
{
    // 通知资源管理器重新应用桌面设置（壁纸、主题变化后使用）
    const std::wstring payload = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes";
    DWORD_PTR result = 0;
    ::SendMessageTimeoutW(HWND_BROADCAST,
                          WM_SETTINGCHANGE,
                          0,
                          reinterpret_cast<LPARAM>(payload.c_str()),
                          SMTO_ABORTIFHUNG | SMTO_NORMAL,
                          3000,
                          &result);
}

} // namespace WinEase::Win32
