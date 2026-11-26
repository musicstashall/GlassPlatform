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
    //
    // The purpose of GlassSettings is to keep OpenGlass on the image GlassSettings
    // knows about, so the applied value is reconciled towards the record of the
    // current profile - it is never left describing an image the record does not
    // know. An image that is not registered in the current profile is re-applied by
    // profile, regenerated from the stored recipe when the canvas is missing, or
    // dropped so OpenGlass falls back to its own default reflection.
    enum class Action
    {
        None,       // applied value already matches the ready canvas of this profile
        Adopt,      // write the verified ready canvas of the current profile
        Regenerate, // the profile has a recipe but no canvas: build it, then adopt
        Clear       // nothing of ours for this profile: drop it, OpenGlass uses default
    };

    struct Decision
    {
        Action action;
    };

    /// <param name="activeEmpty">CustomThemeReflection absent or empty.</param>
    /// <param name="activeWasOurs">active points at a canvas published for some profile
    /// (inside ProgramData\OpenGlass\Presets\Textured); any other image is external
    /// and is never rewritten or removed.</param>
    /// <param name="activeMatchesProfile">active is already the canvas recorded for the
    /// current profile, so there is nothing to reconcile.</param>
    /// <param name="readyProfile">a verified ready canvas exists for the current profile.</param>
    /// <param name="recipeProfile">the current profile has a stored recipe that can be
    /// rendered again (source and settings are present).</param>
    inline Decision Decide(bool activeEmpty, bool activeWasOurs, bool activeMatchesProfile,
        bool readyProfile, bool recipeProfile) noexcept
    {
        // A verified canvas of this profile is the target state of OpenGlass. It is
        // written unless it is already the applied value, whatever is applied now:
        // a canvas of another profile or an image GS does not know.
        if (readyProfile) return { activeMatchesProfile ? Action::None : Action::Adopt };
        // No canvas for this profile. Our own applied canvas must not stay on screen:
        // build it again from the recipe, otherwise drop it so OG falls back to its
        // default reflection.
        if (!activeEmpty && activeWasOurs) return { recipeProfile ? Action::Regenerate : Action::Clear };
        // Nothing applied or a foreign image and nothing to apply: leave it alone.
        return { Action::None };
    }
}
