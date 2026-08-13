#pragma once
#include <optional>
#include <windows.h>
#include "../util/ExplorerWindowInfo.hpp"
#include "../util/Color.hpp"

enum class ExplorerAccent
{
    None,
    Blur,
    Acrylic,
    MicaLike
};

struct ExplorerAppearance
{
    ExplorerAccent Accent = ExplorerAccent::None;
    Util::Color Color = Util::Color(0, 0, 0, 0);
    float BlurRadius = 0.0f;
    bool ShowBorder = false;
};

class ExplorerAppearanceConfig
{
public:
    ExplorerAppearanceConfig() = default;

    // Установка глобальных параметров
    void SetAccent(ExplorerAccent accent);
    void SetColor(Util::Color color);
    void SetBlurRadius(float radius);
    void SetShowBorder(bool show);

    // Вычисление оформления для конкретного окна
    ExplorerAppearance Resolve(const ExplorerWindowInfo& info) const;

private:
    ExplorerAccent m_Accent = ExplorerAccent::None;
    Util::Color m_Color = Util::Color(0, 0, 0, 0);
    float m_BlurRadius = 0.0f;
    bool m_ShowBorder = false;
};
