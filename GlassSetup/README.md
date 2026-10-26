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

## Версия комплекта (Release)

Полный релизный комплект получает один номер вида `2.0.0.0042` (версия комплекта / ProductVersion модулей и GlassSetup). Счётчик модулей по-прежнему задаёт **FileVersion** каждой DLL.

1. **Release | x64** — собрать **весь** `GlassProject.sln` (создаётся сессия, выделяется номер комплекта, MSI x64).
2. **Release | x86** — снова собрать **весь** solution (MSI x86 + GlassSetup с тем же номером; сессия финализируется).

Локальный счётчик: `Build\GlassBundleBuildNumber.txt`. Активная сессия: `Build\GlassBundleSession.json` / `.props`. Первый номер после клона — **0042** (`GlassBundleBuildNumberSeed` в `Directory.Build.props`). CI может передать `/p:GlassBundleBuildNumber=N`.

Отмена незавершённой сессии (номер не возвращается в счётчик):

```bat
msbuild GlassProject.sln /t:GlassBundleCancelSession /p:Configuration=Release /p:Platform=x64
```

Сборка одного проекта или Debug **не** создаёт сессию комплекта.

WebView2 Loader (x86) статически в `third_party/`; для запуска нужен **WebView2 Evergreen Runtime** той же битности.
