#pragma once

#include <Windows.h>

#include <string>

// Проверка известных параметров реестра платформы на адекватность (значение
// используется «как есть» при чтении с умолчанием, поэтому повреждённое значение
// просто удаляется — умолчания в реестр не пишутся).
//
// Область: ветки GlassPlatform (HKCU/HKLM) и DWM — только значения OpenGlass.
// Неизвестные имена НЕ трогаются (их могут писать другие версии или программы);
// базовые значения Windows DWM (ColorizationColor/Afterglow/…, EnableTransparency)
// не входят в схему.
namespace Shared::RegistrySanity {

struct Stats
{
    DWORD checked = 0; // сверено известных значений
    DWORD removed = 0; // удалено как некорректных
    DWORD glassIssues = 0; // проблемы веток GlassPlatform (карточка GlassSettings)
    DWORD openGlassIssues = 0; // проблемы веток OpenGlass/DWM (карточка OpenGlass)
    // Имена некорректных значений через ", " — для причины на карточке
    // («какие именно параметры неверны»).
    std::wstring glassValueNames;
    std::wstring openGlassValueNames;
};

// applyFixes=false — только проверка (для обследования установки).
Stats Validate(bool applyFixes) noexcept;

// Полная очистка настроек текущего пользователя (деинсталлятор, по явному выбору):
// вся ветка Software\StanislavKa\GlassPlatform и кастомные значения OpenGlass в
// DWM (HKCU+HKLM). Базовые значения Windows DWM и чужие параметры не трогаются.
void RemoveUserSettings() noexcept;

// New maintenance contract. userRoot may be HKU\SID or a private offline app
// hive; machine=true handles only global keys. apply=false verifies access.
// Installation-path metadata and unknown DWM values are preserved. Returns the
// first failure; apply=true may already have cleared earlier values/profiles.
// DWM cleanup removes all 49 allowlisted OpenGlass values regardless of type,
// then checks their absence; a recreated value or query error is incomplete
// cleanup. This is not atomic against writers after the verification.
bool CleanOwnedSettings(HKEY userRoot, bool machine, bool apply, std::wstring& detail);

} // namespace Shared::RegistrySanity
