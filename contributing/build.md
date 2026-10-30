# Сборка

**Скилл Cursor:** `.cursor/skills/build/SKILL.md`

## Сборка

**Система:** MSBuild, Visual Studio solution (.sln)

```bat
:: Debug
msbuild GlassProject.sln /p:Configuration=Debug /p:Platform=x64

:: Release
msbuild GlassProject.sln /p:Configuration=Release /p:Platform=x64

:: Один проект
msbuild GlassCore\GlassCore.vcxproj /p:Configuration=Debug /p:Platform=x64
```

## GlassSetup

Окно мастера — проект **`GlassSetup`** в solution (**только x86**, выход `Build\GlassSetup\Debug\` / `Release\`):

```bat
msbuild GlassProject.sln /t:GlassSetup /p:Configuration=Debug /p:Platform=x86
```

См. [GlassSetup/README.md](../GlassSetup/README.md) и [GlassSetup/docs/GlassSetup.md](../GlassSetup/docs/GlassSetup.md).

## Публикация GlassSettings

GlassSettings публикуется отдельно от solution после сборки platform+GlassSettings:

```bat
:: x64
msbuild GlassSettings\GlassSettings.csproj /p:Configuration=Debug /p:Platform=x64 /t:Publish /p:PublishProfile=win-x64

:: x86
msbuild GlassSettings\GlassSettings.csproj /p:Configuration=Debug /p:Platform=x86 /t:Publish /p:PublishProfile=win-x86
```

**Результат публикации:**
- `Build\x64\Debug\GlassSettings\` — single-file exe + WASDK native DLLs
- `Build\x86\Debug\GlassSettings\` — аналогично для x86

**Профили публикации:** `GlassSettings\Properties\PublishProfiles\win-x64.pubxml`, `win-x86.pubxml`.

## Не удалять WinUI assets из output GlassSettings

Папка `Microsoft.UI.Xaml\Assets\NoiseAsset_256x256_PNG.png` в `GlassSettingsUnpackaged` (и в publish) — шумовая текстура `AcrylicBrush`.
Без неё composition-кисть не строится и пропадают сразу все acrylic-поверхности: фон диалогов, штора `WindowBusyOverlay`,
тултипы, дропдауны комбобоксов (ключ `AcrylicInAppFillColorDefaultBrush`). Регрессия 2026-09-08: таргет `RemoveWinUIAssets` удалял
папку целиком — удалён, `MaterialTextureLoader` (шум из `Windows.UI.Xaml.Controls.dll`) питает только превью материалов
и кисти не чинит. Контроль после сборки: `Test-Path Build\x64\Debug\GlassSettingsUnpackaged\Microsoft.UI.Xaml\Assets\NoiseAsset_256x256_PNG.png`.

## Платформа и тесты — только пользователь

**Запуск, остановка и проверка GlassPlatform** (LogViewer START/STOP, `Rundll32 … Main /start` или `/stop`, `stop.vbs`, отладка в VS, перезапуск explorer и т.п.) выполняет **только пользователь**.

**Ассистенту запрещено:**

- запускать или останавливать платформу любым способом;
- вызывать `Rundll32`, `stop.vbs`, скрипты или API «от имени пользователя» для start/stop;
- завершать процессы (`taskkill`, «освободить DLL» и аналоги), чтобы разблокировать сборку.

Справка для пользователя (не для выполнения ассистентом): хост — `C:\Windows\System32\Rundll32.exe`, аргументы вида `"<путь>\Build\x64\Debug\GlassCompat.dll",Main /start` и `Main /stop`; остановка — также через LogViewer или `stop.vbs` в каталоге сборки.

## Ошибки MSBuild

**Обычные ошибки компиляции** (C2065, C2039, C1083 и т.д.) — исправлять в цикле до чистой сборки **без** вмешательства пользователя.

**LNK1168** (не удаётся записать DLL — файл занят/загружен):

1. **Не** останавливать платформу и **не** трогать процессы самостоятельно.
2. Сообщить пользователю, какой файл заблокирован и что сборка не прошла. MSBuild не повторять.
3. Показать вопрос-диалог строго с двумя вариантами (дословно):
   - `Модули освободил, можно повторить сборку` — после этого ответа один раз повторить ту же команду MSBuild;
   - `Сборку выполнил сам` — ничего не запускать; ждать, пользователь соберёт вручную и сообщит результат.
4. **Ждать ответа пользователя** и действовать только согласно выбранному варианту.

Повторять сборку в цикле при LNK1168 без ответа пользователя **нельзя**.
