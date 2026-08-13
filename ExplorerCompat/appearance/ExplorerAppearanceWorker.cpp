#include "ExplorerPch.h"
#include "ExplorerAppearanceWorker.hpp"
#include <format>
#include <DebugLog.h>

ExplorerAppearanceWorker::~ExplorerAppearanceWorker()
{
    for (auto& [hwnd, layer] : m_Layers)
    {
        if (layer.IsValid())
            DCompUtils::DestroyLayer(layer);
    }
    m_Layers.clear();
}

void ExplorerAppearanceWorker::ApplyConfig(const ExplorerAppearanceConfig& config)
{
    m_Config = config;
}

void ExplorerAppearanceWorker::OnExplorerWindowDetected(const ExplorerWindowInfo& info)
{
    if (!info.Hwnd || !info.IsExplorerWindow())
        return;

    DebugLog(std::format(
        L"[ExplorerCompat][Appearance] OnExplorerWindowDetected hwnd=0x{:X} (DComp отключён — backdrop задаёт GlassCore/WndCompat)",
        (UINT_PTR)info.Hwnd).c_str());
}

void ExplorerAppearanceWorker::OnExplorerWindowClosed(HWND hwnd)
{
    auto it = m_Layers.find(hwnd);
    if (it == m_Layers.end())
        return;

    DebugLog(std::format(
        L"[ExplorerCompat][Appearance] OnExplorerWindowClosed hwnd=0x{:X}",
        (UINT_PTR)hwnd).c_str());

    if (it->second.IsValid())
        DCompUtils::DestroyLayer(it->second);

    m_Layers.erase(it);
}
