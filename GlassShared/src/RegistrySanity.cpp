#include "SharedPch.h"

#include "RegistrySanity.h"

#include <iterator>

namespace Shared::RegistrySanity {
namespace {

enum class ValueKind : DWORD { Dword, DwordBool, Text };

struct Rule
{
    const wchar_t* valueName;
    ValueKind kind;
};

// Известные значения GlassSettings/платформы. Неизвестные имена не трогаются.
constexpr Rule kGlassSettingsRules[] = {
    { L"EnableSystemFont", ValueKind::DwordBool },
    { L"AppTheme", ValueKind::Dword },           // enum AppThemeMode; точные границы не навязываем
    { L"ValidateSettingsOnStartup", ValueKind::DwordBool },
    { L"WindowBackdrop", ValueKind::Dword },
    { L"WindowBusyOverlayMode", ValueKind::Dword },
    { L"OpenGlassUpdateLastCheckUtc", ValueKind::Dword },
    { L"UpdateCheckInterval", ValueKind::Dword },
    { L"LastAppliedThemeAccentColor", ValueKind::Dword },
    { L"LastAppliedThemeDwmFingerprint", ValueKind::Dword },
    { L"OpenGlassUpdatePendingVersion", ValueKind::Text },
    { L"OpenGlassUpdatePendingUrl", ValueKind::Text },
    { L"OpenGlassUpdateNoticeDismissedVersion", ValueKind::Text },
    { L"AppUiLanguage", ValueKind::Text },
    { L"ExecutableFilePickerLastPath", ValueKind::Text },
    { L"LastAppliedThemeFilePath", ValueKind::Text },
    { L"OpenGlassUpdateCachedTag", ValueKind::Text },
    { L"OpenGlassUpdateCachedUrl", ValueKind::Text },
    { L"OpenGlassUpdateDismissedTag", ValueKind::Text },
};

constexpr Rule kGlassPlatformRootRules[] = {
    { L"InstallPath", ValueKind::Text },
    { L"InstallPathX64", ValueKind::Text },
    { L"InstallPathX86", ValueKind::Text },
    { L"OpenGlassInstallPath", ValueKind::Text },
};

constexpr Rule kOpenGlassRootRules[] = {
    { L"InstallPath", ValueKind::Text },
};

// Только значения OpenGlass в DWM (каталог SettingsCatalog, v3). Базовые цвета
// Windows (ColorizationColor/Afterglow/…/EnableTransparency) в схему не входят.
constexpr Rule kDwmOpenGlassRules[] = {
    { L"ColorizationColorOverride", ValueKind::Dword },
    { L"ColorizationAfterglowOverride", ValueKind::Dword },
    { L"ColorizationColorInactive", ValueKind::Dword },
    { L"GlassOpacity", ValueKind::Dword },
    { L"GlassType", ValueKind::Dword },
    { L"GlassOverrideAccent", ValueKind::DwordBool },
    { L"CustomThemeReflection", ValueKind::DwordBool },
    { L"BlurDeviation", ValueKind::Dword },
    { L"BlurOptimization", ValueKind::Dword },
    { L"RoundRectRadius", ValueKind::Dword },
    { L"CustomThemeMaterial", ValueKind::Dword },
    { L"MaterialOpacity", ValueKind::Dword },
    { L"UseDirect3DRendering", ValueKind::DwordBool },
    { L"CaptionButtons", ValueKind::DwordBool },
    { L"CenterCaption", ValueKind::DwordBool },
    { L"TextGlowMode", ValueKind::Dword },
    { L"GlassSafetyZoneMode", ValueKind::Dword },
    { L"DisableModernBorders", ValueKind::DwordBool },
    { L"DisableGlassOnBattery", ValueKind::DwordBool },
    { L"DisabledHooks", ValueKind::Dword },
    { L"EnableUxMitigation", ValueKind::DwordBool },
    { L"CustomThemeAtlas", ValueKind::Text },
    { L"AtlasThemeImage", ValueKind::Text },
};

bool IsIncorrect(HKEY key, const wchar_t* valueName, ValueKind kind) noexcept
{
    DWORD type = 0;
    BYTE data[512]{};
    DWORD size = sizeof(data);
    const LSTATUS status = RegQueryValueExW(key, valueName, nullptr, &type, data, &size);
    if (status == ERROR_FILE_NOT_FOUND)
        return false; // отсутствие — легальное умолчание
    if (status != ERROR_SUCCESS)
        return true; // нечитаемое значение — некорректно
    if (kind == ValueKind::Text)
        return type != REG_SZ && type != REG_EXPAND_SZ;
    if (type != REG_DWORD)
        return true;
    const DWORD value = *reinterpret_cast<const DWORD*>(data);
    return kind == ValueKind::DwordBool && value > 1;
}

// Проверка одной ветки: значения схемы, тип/ограниченный диапазон.
void ValidateKey(
    HKEY root,
    const wchar_t* subKey,
    REGSAM view,
    const Rule* rules,
    size_t ruleCount,
    bool applyFixes,
    DWORD& issues) noexcept
{
    HKEY key = nullptr;
    const REGSAM access = applyFixes ? (KEY_QUERY_VALUE | KEY_SET_VALUE) : KEY_QUERY_VALUE;
    if (RegOpenKeyExW(root, subKey, 0, access | view, &key) != ERROR_SUCCESS)
        return;
    for (size_t i = 0; i < ruleCount; ++i)
    {
        if (!IsIncorrect(key, rules[i].valueName, rules[i].kind))
            continue;
        ++issues;
        if (applyFixes)
            RegDeleteValueW(key, rules[i].valueName); // умолчания в реестр не пишутся
    }
    RegCloseKey(key);
}

template <size_t N>
void ValidateKeyT(HKEY root, const wchar_t* subKey, REGSAM view, const Rule (&rules)[N], bool applyFixes, DWORD& issues) noexcept
{
    ValidateKey(root, subKey, view, rules, N, applyFixes, issues);
}

} // namespace

Stats Validate(bool applyFixes) noexcept
{
    Stats stats;
    const REGSAM views[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    for (REGSAM view : views)
    {
        DWORD before = stats.glassIssues;
        ValidateKeyT(HKEY_CURRENT_USER, L"Software\\StanislavKa\\GlassPlatform\\GlassSettings", view,
            kGlassSettingsRules, applyFixes, stats.glassIssues);
        ValidateKeyT(HKEY_CURRENT_USER, L"Software\\StanislavKa\\GlassPlatform", view,
            kGlassPlatformRootRules, applyFixes, stats.glassIssues);
        ValidateKeyT(HKEY_LOCAL_MACHINE, L"Software\\StanislavKa\\GlassPlatform", view,
            kGlassPlatformRootRules, applyFixes, stats.glassIssues);
        DWORD beforeOg = stats.openGlassIssues;
        ValidateKeyT(HKEY_CURRENT_USER, L"Software\\StanislavKa\\OpenGlass", view,
            kOpenGlassRootRules, applyFixes, stats.openGlassIssues);
        ValidateKeyT(HKEY_LOCAL_MACHINE, L"Software\\StanislavKa\\OpenGlass", view,
            kOpenGlassRootRules, applyFixes, stats.openGlassIssues);
        ValidateKeyT(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", view,
            kDwmOpenGlassRules, applyFixes, stats.openGlassIssues);
        ValidateKeyT(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\DWM", view,
            kDwmOpenGlassRules, applyFixes, stats.openGlassIssues);
        (void)before;
        (void)beforeOg;
    }
    stats.checked = static_cast<DWORD>(std::size(kGlassSettingsRules) + std::size(kGlassPlatformRootRules) +
        std::size(kOpenGlassRootRules) + std::size(kDwmOpenGlassRules));
    stats.removed = applyFixes ? stats.glassIssues + stats.openGlassIssues : 0;
    return stats;
}

} // namespace Shared::RegistrySanity
