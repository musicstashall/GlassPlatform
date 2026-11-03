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

} // namespace Shared::RegistrySanity
