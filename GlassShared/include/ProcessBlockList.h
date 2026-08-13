#pragma once
#include <string>
#include <unordered_set>

namespace ProcessBlockList
{
    // �������������: ����������� ������ + �������� �� �������
    void Initialize();

    // Заблокирован: инъекция GlassCompat не выполняется для exeName.
    bool IsBlocked(const std::wstring& exeName);

    /// GlassSettings — локальный GlassCore без глобального NoBackdrop (WinUI host).
    [[nodiscard]] bool IsCurrentProcessGlassToolHost() noexcept;

    /// LogViewer — изолированный tool-host: NoBackdrop=0, backdrop не следует за старт/стоп платформы.
    [[nodiscard]] bool IsCurrentProcessLogViewerHost() noexcept;

    // ������������ ����� (lowercase)
    std::wstring Normalize(const std::wstring& exeName);

    // ���������� exeName � ��������� BlockList � � ����������� g_blockList.
    // ������������. noexcept — ������ ����� ����� OutputDebugStringW.
    void AddToRegistry(const std::wstring& exeName) noexcept;
}
