#include "SharedPch.h"

#include "RegistrySanity.h"

#include <iterator>

namespace Shared::RegistrySanity {
namespace {

enum class ValueKind : DWORD { Dword, DwordBool, Text };

constexpr REGSAM kViews[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };

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
    { L"LastAppliedThemeDwmFingerprint", ValueKind::Text },
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

// Только значения OpenGlass в DWM — имена и типы из каталога SettingsCatalog v3. Базовые цвета
// Windows (ColorizationColor/Afterglow/…/EnableTransparency) в схему не входят.
// Audited against ALTaleX531/OpenGlass fb145fcd391b274cb91e642c93b3fc2b0e30e61c,
// Scripts/OpenGlass.iss::DeleteConfig: 54 names minus five protected Windows
// values (ColorizationColor, ColorizationAfterglow, ColorizationColorBalance,
// ColorizationAfterglowBalance, ColorizationBlurBalance). The remaining 49
// names are removed in every user hive and HKLM by CleanOwnedSettings.
// Do NOT copy upstream's five Windows values into our uninstall allowlist.
constexpr Rule kDwmOpenGlassRules[] = {
    { L"ColorizationColorOverride", ValueKind::Dword },
    { L"ColorizationAfterglowOverride", ValueKind::Dword },
    { L"ColorizationColorBalanceOverride", ValueKind::Dword },
    { L"ColorizationAfterglowBalanceOverride", ValueKind::Dword },
    { L"ColorizationBlurBalanceOverride", ValueKind::Dword },
    { L"ColorizationColorInactive", ValueKind::Dword },
    { L"GlassOpacity", ValueKind::Dword },
    { L"GlassOpacityInactive", ValueKind::Dword },
    { L"ColorizationColorCaption", ValueKind::Dword },
    { L"ColorizationColorCaptionInactive", ValueKind::Dword },
    { L"ColorizationColorCaptionMaximized", ValueKind::Dword },
    { L"ColorizationColorCaptionInactiveMaximized", ValueKind::Dword },
    { L"ColorizationOpaqueBlend", ValueKind::Dword },
    { L"ColorizationBaseTransparent", ValueKind::Dword },
    { L"ColorizationBaseMaximized", ValueKind::Dword },
    { L"ColorizationBaseOpaque", ValueKind::Dword },
    { L"ColorizationOpaqueBlendPriority", ValueKind::Dword },
    { L"ColorizationOpacity", ValueKind::Dword },
    { L"ColorizationOpacityInactive", ValueKind::Dword },
    { L"ColorizationOpacityMaximized", ValueKind::Dword },
    { L"ColorizationOpacityInactiveMaximized", ValueKind::Dword },
    { L"GlassType", ValueKind::Dword },
    { L"GlassOverrideAccent", ValueKind::DwordBool },
    { L"ColorizationGlassReflectionIntensity", ValueKind::Dword },
    { L"ColorizationGlassReflectionOpacity", ValueKind::Dword },
    { L"ColorizationGlassReflectionOpacityInactive", ValueKind::Dword },
    { L"ColorizationGlassReflectionOpacityMaximized", ValueKind::Dword },
    { L"ColorizationGlassReflectionOpacityInactiveMaximized", ValueKind::Dword },
    { L"ColorizationGlassReflectionParallaxIntensity", ValueKind::Dword },
    { L"ColorizationGlassReflectionPolicy", ValueKind::Dword },
    { L"BlurDeviation", ValueKind::Dword },
    { L"BlurOptimization", ValueKind::Dword },
    { L"RoundRectRadius", ValueKind::Dword },
    { L"MaterialOpacity", ValueKind::Dword },
    { L"UseDirect3DRendering", ValueKind::DwordBool },
    { L"CaptionButtons", ValueKind::DwordBool },
    { L"CenterCaption", ValueKind::DwordBool },
    { L"TextGlowMode", ValueKind::Dword },
    { L"DisableModernBorders", ValueKind::DwordBool },
    { L"DisableGlassOnBattery", ValueKind::DwordBool },
    { L"DisabledHooks", ValueKind::Dword },
    { L"GlassSafetyZoneMode", ValueKind::Dword },
    { L"MINMAXBUTTONGLOWid", ValueKind::Dword },
    { L"CLOSEBUTTONGLOWid", ValueKind::Dword },
    { L"TOOLCLOSEBUTTONGLOWid", ValueKind::Dword },
    { L"EnableUxMitigation", ValueKind::DwordBool },
    { L"CustomThemeReflection", ValueKind::Text },
    { L"CustomThemeMaterial", ValueKind::Text },
    { L"CustomThemeAtlas", ValueKind::Text },
};

bool IsIncorrect(HKEY key, const wchar_t* valueName, ValueKind kind) noexcept
{
    // Сначала только тип/размер: длинные строки не должны считаться повреждёнными
    // из-за нехватки буфера (ERROR_MORE_DATA).
    DWORD type = 0;
    DWORD size = 0;
    const LSTATUS status = RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND)
        return false; // отсутствие — легальное умолчание
    if (status != ERROR_SUCCESS)
        return true; // нечитаемое значение — некорректно
    if (kind == ValueKind::Text)
        return type != REG_SZ && type != REG_EXPAND_SZ;
    if (type != REG_DWORD)
        return true;
    if (kind == ValueKind::DwordBool)
    {
        DWORD value = 0;
        DWORD cb = sizeof(value);
        if (RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<BYTE*>(&value), &cb) != ERROR_SUCCESS)
            return true;
        return value > 1;
    }
    return false;
}

// Проверка одной ветки: значения схемы, тип/ограниченный диапазон.
void AppendName(std::wstring& names, const wchar_t* valueName) noexcept
{
    if (!names.empty())
        names += L", ";
    names += valueName;
}

void ValidateKey(
    HKEY root,
    const wchar_t* subKey,
    REGSAM view,
    const Rule* rules,
    size_t ruleCount,
    bool applyFixes,
    DWORD& issues,
    std::wstring& names) noexcept
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
        AppendName(names, rules[i].valueName);
        if (applyFixes)
            RegDeleteValueW(key, rules[i].valueName); // умолчания в реестр не пишутся
    }
    RegCloseKey(key);
}

template <size_t N>
void ValidateKeyT(HKEY root, const wchar_t* subKey, REGSAM view, const Rule (&rules)[N], bool applyFixes, DWORD& issues, std::wstring& names) noexcept
{
    ValidateKey(root, subKey, view, rules, N, applyFixes, issues, names);
}

} // namespace

Stats Validate(bool applyFixes) noexcept
{
    Stats stats;
    for (REGSAM view : kViews)
    {
        ValidateKeyT(HKEY_CURRENT_USER, L"Software\\StanislavKa\\GlassPlatform\\GlassSettings", view,
            kGlassSettingsRules, applyFixes, stats.glassIssues, stats.glassValueNames);
        ValidateKeyT(HKEY_CURRENT_USER, L"Software\\StanislavKa\\GlassPlatform", view,
            kGlassPlatformRootRules, applyFixes, stats.glassIssues, stats.glassValueNames);
        ValidateKeyT(HKEY_LOCAL_MACHINE, L"Software\\StanislavKa\\GlassPlatform", view,
            kGlassPlatformRootRules, applyFixes, stats.glassIssues, stats.glassValueNames);
        ValidateKeyT(HKEY_CURRENT_USER, L"Software\\StanislavKa\\OpenGlass", view,
            kOpenGlassRootRules, applyFixes, stats.openGlassIssues, stats.openGlassValueNames);
        ValidateKeyT(HKEY_LOCAL_MACHINE, L"Software\\StanislavKa\\OpenGlass", view,
            kOpenGlassRootRules, applyFixes, stats.openGlassIssues, stats.openGlassValueNames);
        ValidateKeyT(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", view,
            kDwmOpenGlassRules, applyFixes, stats.openGlassIssues, stats.openGlassValueNames);
        ValidateKeyT(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\DWM", view,
            kDwmOpenGlassRules, applyFixes, stats.openGlassIssues, stats.openGlassValueNames);
    }
    stats.checked = static_cast<DWORD>(std::size(kGlassSettingsRules) + std::size(kGlassPlatformRootRules) +
        std::size(kOpenGlassRootRules) + std::size(kDwmOpenGlassRules));
    stats.removed = applyFixes ? stats.glassIssues + stats.openGlassIssues : 0;
    return stats;
}

void RemoveKeyTree(HKEY root, const wchar_t* parent, const wchar_t* subKey, REGSAM view) noexcept
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, parent, 0, KEY_WRITE | view, &key) != ERROR_SUCCESS)
        return;
    RegDeleteTreeW(key, subKey);
    RegCloseKey(key);
}

void RemoveUserSettings() noexcept
{
    for (REGSAM view : kViews)
    {
        // Вся ветка GlassPlatform текущего пользователя (оба представления WOW64 —
        // 32-битная GlassSettings пишет в зеркало).
        RemoveKeyTree(HKEY_CURRENT_USER, L"Software", L"StanislavKa\\GlassPlatform", view);
        // Кастомные значения OpenGlass в DWM (HKCU+HKLM); базовые значения Windows
        // (Colorization* «без суффиксов», EnableTransparency) не входят в схему.
        for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE })
        {
            HKEY dwm = nullptr;
            if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\DWM", 0, KEY_SET_VALUE | view, &dwm) != ERROR_SUCCESS)
                continue;
            for (const auto& rule : kDwmOpenGlassRules)
                RegDeleteValueW(dwm, rule.valueName);
            RegCloseKey(dwm);
        }
    }
}


namespace {
bool VerifyTreeAccess(HKEY key, unsigned depth) {
    if(depth>64)return false;
    for(DWORD index=0;;++index) {
        wchar_t name[256]{};DWORD size=256;
        const LONG st=RegEnumKeyExW(key,index,name,&size,nullptr,nullptr,nullptr,nullptr);
        if(st==ERROR_NO_MORE_ITEMS)return true;
        if(st!=ERROR_SUCCESS)return false;
        HKEY child=nullptr;
        if(RegOpenKeyExW(key,name,0,KEY_ALL_ACCESS,&child)!=ERROR_SUCCESS)return false;
        const bool ok=VerifyTreeAccess(child,depth+1);RegCloseKey(child);if(!ok)return false;
    }
}
bool InstallMetadata(const wchar_t* name) {
    for(const auto& rule:kGlassPlatformRootRules)if(_wcsicmp(name,rule.valueName)==0)return true;
    return false;
}
}
static bool CleanSettingsImpl(HKEY root, bool machine, bool apply, std::wstring& detail, bool includeOpenGlass) {
    for(REGSAM view:kViews) {
        HKEY glass=nullptr;
        LONG st=RegOpenKeyExW(root,L"Software\\StanislavKa\\GlassPlatform",0,KEY_ALL_ACCESS|view,&glass);
        if(st!=ERROR_SUCCESS&&st!=ERROR_FILE_NOT_FOUND){detail=L"GlassPlatform access: "+std::to_wstring(st);return false;}
        if(glass) {
            if(!VerifyTreeAccess(glass,0)){RegCloseKey(glass);detail=L"GlassPlatform child access";return false;}
            if(apply) {
                for(;;) {
                    wchar_t child[256]{};DWORD size=256;
                    st=RegEnumKeyExW(glass,0,child,&size,nullptr,nullptr,nullptr,nullptr);
                    if(st==ERROR_NO_MORE_ITEMS)break;
                    if(st==ERROR_SUCCESS)st=RegDeleteTreeW(glass,child);
                    if(st!=ERROR_SUCCESS){RegCloseKey(glass);detail=L"GlassPlatform child cleanup: "+std::to_wstring(st);return false;}
                }
                for(DWORD index=0;;) {
                    wchar_t name[16384]{};DWORD size=16384;
                    st=RegEnumValueW(glass,index,name,&size,nullptr,nullptr,nullptr,nullptr);
                    if(st==ERROR_NO_MORE_ITEMS)break;
                    if(st!=ERROR_SUCCESS){RegCloseKey(glass);detail=L"GlassPlatform value enumeration";return false;}
                    if(InstallMetadata(name)){++index;continue;}
                    st=RegDeleteValueW(glass,name);
                    if(st!=ERROR_SUCCESS){RegCloseKey(glass);detail=L"GlassPlatform value cleanup: "+std::to_wstring(st);return false;}
                }
            }
            RegCloseKey(glass);
        }
        if(!includeOpenGlass)continue; // OG config is owned by its official uninstaller.
        HKEY dwm=nullptr;
        st=RegOpenKeyExW(root,L"Software\\Microsoft\\Windows\\DWM",0,KEY_QUERY_VALUE|KEY_SET_VALUE|view,&dwm);
        if(st!=ERROR_SUCCESS&&st!=ERROR_FILE_NOT_FOUND){detail=L"DWM access: "+std::to_wstring(st);return false;}
        if(dwm) {
            if(apply) {
                for(const auto& rule:kDwmOpenGlassRules) {
                    st=RegDeleteValueW(dwm,rule.valueName);
                    if(st!=ERROR_SUCCESS&&st!=ERROR_FILE_NOT_FOUND) {
                        RegCloseKey(dwm);
                        detail=L"DWM cleanup: "+std::wstring(rule.valueName)+L" ("+std::to_wstring(st)+L")";
                        return false;
                    }
                }
                // A retained writer can recreate values even after a successful
                // RegDeleteValue. Do not report a complete registry cleanup in
                // that case. Query presence only: type/size never exempt an owned
                // name from explicit uninstall cleanup. Unknown Windows values
                // and the DWM key itself are deliberately preserved.
                for(const auto& rule:kDwmOpenGlassRules) {
                    st=RegQueryValueExW(dwm,rule.valueName,nullptr,nullptr,nullptr,nullptr);
                    if(st!=ERROR_FILE_NOT_FOUND) {
                        RegCloseKey(dwm);
                        detail=L"DWM cleanup verification: "+std::wstring(rule.valueName)+L" ("+std::to_wstring(st)+L")";
                        return false;
                    }
                }
            }
            RegCloseKey(dwm);
        }
    }
    (void)machine; // Same whitelist; the caller supplies HKLM only once.
    return true;
}

bool CleanOwnedSettings(HKEY root, bool machine, bool apply, std::wstring& detail) {
    return CleanSettingsImpl(root,machine,apply,detail,true);
}
bool CleanPlatformSettings(HKEY root, bool machine, bool apply, std::wstring& detail) {
    return CleanSettingsImpl(root,machine,apply,detail,false);
}

} // namespace Shared::RegistrySanity
