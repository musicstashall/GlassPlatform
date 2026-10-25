# GlassSetup — Win32 / C++17

Окно мастера установки GlassPlatform: **x86 (Win32)**, MSVC, WebView2, сборка через **`GlassProject.sln`**.

Документация: [docs/guides/GlassSetup.md](../docs/guides/GlassSetup.md).  
WiX в Visual Studio (HeatWave, отдельная сборка MSI и exe): [docs/guides/GlassSetup-WiX-VisualStudio.md](../docs/guides/GlassSetup-WiX-VisualStudio.md).

## Сборка

Visual Studio 2022, платформа **Win32** (в solution — **x86**), Debug или Release:

```bat
msbuild GlassProject.sln /t:GlassSetup /p:Configuration=Debug /p:Platform=x86
```

При сборке всего solution с **x64** проект GlassSetup всё равно собирается как **Win32** (см. маппинг в `.sln`).

**Выход:** `Build\GlassSetup\Debug\GlassSetup.exe` или `Build\GlassSetup\Release\GlassSetup.exe`.

WebView2 Loader (x86) статически в `third_party/`; для запуска нужен **WebView2 Evergreen Runtime** той же битности.
