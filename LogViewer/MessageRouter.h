#pragma once
#include <Windows.h>
#include <cstdint>

// Команды для LogViewer
enum class LogViewerCommand : std::uint32_t
{
    Clear = 1,
    RebuildView,
    PlatformStarted,
    PlatformStopped
};

// Сообщение для команд в главное окно
inline constexpr UINT WM_APP_COMMAND = WM_APP + 100;

class MessageRouter
{
public:
    static void Send(LogViewerCommand cmd, void* payload = nullptr);
};
