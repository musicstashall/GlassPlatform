#pragma once

#include <Windows.h>
#include <cstdint>

namespace Shared::Log
{
    inline constexpr wchar_t kLogPipeName[] = L"\\\\.\\pipe\\GlassPlatform.Log";

    enum class MessageType : std::uint16_t
    {
        Handshake = 1,
        LogLine = 2,
        Flush = 3,
        Shutdown = 4,
    };

#pragma pack(push, 1)
    struct MessageHeader
    {
        std::uint32_t size;
        std::uint16_t type;
        std::uint16_t flags;
        std::uint32_t threadId;  // реальный TID из клиентского процесса
    };
#pragma pack(pop)

    enum : std::uint16_t
    {
        LogFlag_WideChar = 0x0001,
        LogFlag_Utf8 = 0x0002,
    };

    inline constexpr std::uint16_t LogFlag_LevelShift = 8;
    inline constexpr std::uint16_t LogFlag_LevelMask = 0xFF00;

    inline constexpr std::uint16_t EncodeLevel(int level)
    {
        return static_cast<std::uint16_t>(level << LogFlag_LevelShift);
    }

    inline constexpr int DecodeLevel(std::uint16_t flags)
    {
        return static_cast<int>((flags & LogFlag_LevelMask) >> LogFlag_LevelShift);
    }
}
