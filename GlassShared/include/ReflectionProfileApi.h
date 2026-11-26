#pragma once
#include <Windows.h>

// Runtime-only ABI. Optional GlassSettings.Native; no import-library dependency.
namespace Shared::ReflectionProfiles
{
    inline constexpr DWORD Version = 1;
    inline constexpr wchar_t RegistryRoot[] = L"Software\\StanislavKa\\GlassPlatform\\GlassSettings\\Textured";
    inline constexpr wchar_t MutexName[] = L"Local\\GlassPlatform.ReflectionProfiles";
    struct Monitor { LONG x, y, width, height; UINT dpiX, dpiY; };
    struct Snapshot
    {
        DWORD size;
        DWORD version;
        LONG width, height;
        UINT count;
        Monitor monitors[32];
        wchar_t key[128];
    };
    using Capture = HRESULT (WINAPI*)(Snapshot*);
    using Signal = void (WINAPI*)();
    using Tick = HRESULT (WINAPI*)();

    // One decision table shared by GlassSettings and the optional platform helper.
    // Both sides must reach the same conclusion for the same registry state, so the
    // rules live here instead of being duplicated (and drifting) per module.
    enum class Action
    {
        None,    // leave the applied value alone (external/default selection)
        Adopt,   // write the verified ready canvas of the current profile
        Clear    // our canvas is gone for this profile: drop the stale applied value
    };

    struct Decision
    {
        Action action;
    };

    /// <param name="managed">HKCU Textured\Managed DWORD equals 1.</param>
    /// <param name="activeEmpty">CustomThemeReflection absent or empty.</param>
    /// <param name="activeWasOurs">active points at a canvas published for some profile
    /// (inside ProgramData\OpenGlass\Presets\Textured); proves earlier ownership even
    /// if the Managed flag was reset or the platform never ran.</param>
    /// <param name="readyProfile">a verified ready canvas exists for the current profile.</param>
    inline Decision Decide(bool managed, bool activeEmpty, bool activeWasOurs,
        bool readyProfile) noexcept
    {
        if (activeEmpty) return { managed && readyProfile ? Action::Adopt : Action::None };
        if (readyProfile && (managed || activeWasOurs))
            return { Action::Adopt };
        // Nothing to adopt: the applied value provably points at one of our published
        // canvases, so it is a canvas of another topology - drop it instead of leaving
        // a picture of a previous display configuration applied. A managed flag alone
        // is not enough here: only a path we published may be removed.
        if (!readyProfile && activeWasOurs)
            return { Action::Clear };
        return { Action::None };
    }
}
