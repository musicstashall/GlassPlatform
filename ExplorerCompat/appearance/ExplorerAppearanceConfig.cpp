#include "ExplorerPch.h"
#include "ExplorerAppearanceConfig.hpp"

void ExplorerAppearanceConfig::SetAccent(ExplorerAccent accent)
{
    m_Accent = accent;
}

void ExplorerAppearanceConfig::SetColor(Util::Color color)
{
    m_Color = color;
}

void ExplorerAppearanceConfig::SetBlurRadius(float radius)
{
    m_BlurRadius = radius;
}

void ExplorerAppearanceConfig::SetShowBorder(bool show)
{
    m_ShowBorder = show;
}

ExplorerAppearance ExplorerAppearanceConfig::Resolve(const ExplorerWindowInfo& info) const
{
    ExplorerAppearance result;

    // Базовый акцент
    result.Accent = m_Accent;
    result.Color = m_Color;
    result.BlurRadius = m_BlurRadius;
    result.ShowBorder = m_ShowBorder;

    // Пример расширяемой логики:
    // Активное окно — чуть ярче
    if (info.IsActive())
    {
        result.Color.A = std::min<int>(result.Color.A + 20, 255);
    }

    // Максимизированное окно — можно менять поведение
    if (info.IsMaximized())
    {
        // Например, отключить border
        result.ShowBorder = false;
    }

    // Будущие расширения:
    // - разные профили для Settings/ControlPanel
    // - разные профили для Ribbon/CommandBarFlyout
    // - правила по имени процесса/класса окна

    return result;
}
