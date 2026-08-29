# Plan: Fix GlassSettings build output paths for Debug/Release/x86/x64

## Problem
`GlassSettings.Native.vcxproj` outputs to drive `D:\` root instead of `Build\<platform>\<config>\` inside the repo. Additionally, x86 native output path (`Win32`) doesn't match where the managed project looks for the DLL (`x86`).

## Root cause
`GlassSettings.Native.vcxproj:44` computes `GlassProjectRoot` via `GetFullPath('$(MSBuildThisFileDirectory)..\..\')`. This is unreliable for vcxproj files and evaluates to `D:\` in the VS build context, causing `OutDir` to become `D:\Build\...`.

Secondary issue: `GlassSettings.csproj:133` looks for the native DLL using `$(Platform)` (`x86`), but the native project outputs to `PlatformShortName=Win32`.

## Fix

### 1. `GlassSettings\Native\GlassSettings.Native.vcxproj`
- Remove `GlassProjectRoot` property computation (line 44)
- Remove `SolutionDir` fallback that uses `GlassProjectRoot` (line 45)
- Replace all `$(GlassProjectRoot)` occurrences in `OutDir`/`IntDir` with `$(MSBuildThisFileDirectory)..\..\`
- This matches the pattern already used by `GlassShellHost`, `GlassShellExt`, and other native projects, and is already used successfully in `AdditionalLibraryDirectories` of the same file.

### 2. `GlassSettings\GlassSettings.csproj`
- In `CopyNativeDllToOutput` target (line 133), replace `$(Platform)` with `$(GlassSettingsNativePlatform)` in `_NativeDllFrom` path so x86 builds look in `Build\Win32\Debug\` instead of `Build\x86\Debug\`.

## Validation
- Build `GlassSettings.Native` for Debug/Release × Win32/x64 → outputs land in `Build\Win32\Debug\`, `Build\Win32\Release\`, `Build\x64\Debug\`, `Build\x64\Release\`
- Build `GlassSettings` managed project for x86/x64 → `CopyNativeDllToOutput` finds and copies `GlassSettings.Native.dll` correctly
- Publish produces self-contained output with native DLL present
