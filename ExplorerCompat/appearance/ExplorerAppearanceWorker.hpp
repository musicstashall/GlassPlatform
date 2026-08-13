#pragma once
#include <windows.h>
#include <unordered_map>
#include "../util/ExplorerWindowInfo.hpp"
#include "../util/DCompUtils.hpp"
#include "ExplorerAppearanceConfig.hpp"

// Variant A: ExplorerCompat НЕ сабклассит окна.
// Только DComp-слой + XAML-острова + профили оформления.

class ExplorerAppearanceWorker
{
public:
    ExplorerAppearanceWorker() = default;
    ~ExplorerAppearanceWorker();

    void ApplyConfig(const ExplorerAppearanceConfig& config);

    // Уведомление: найдено окно Explorer
    void OnExplorerWindowDetected(const ExplorerWindowInfo& info);

    // Уведомление: окно Explorer закрыто
    void OnExplorerWindowClosed(HWND hwnd);

private:
    ExplorerAppearanceConfig m_Config{};
    std::unordered_map<HWND, DCompHandle> m_Layers{};
};
