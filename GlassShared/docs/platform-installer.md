# Установщик платформы Glass: раскладка и реестр

**Связанный код:** [`GlassSettings/Core/Platform/GlassModulePath.cs`](../../GlassSettings/Core/Platform/GlassModulePath.cs), [`GlassSettings/Core/Platform/PlatformInstallRegistry.cs`](../../GlassSettings/Core/Platform/PlatformInstallRegistry.cs), [`GlassShared/src/GlassModulePath.cpp`](../../GlassShared/src/GlassModulePath.cpp)

Документ описывает раскладку и реестровые ключи, которые используют собранные MSI (`Glass-x64`,
`Glass-x86`, `GlassSettings-*`, `OpenGlass-x64`) и текущий поиск модулей в GlassSettings.

## Раскладка на диске (OS x64)

Два независимых каталога, плоский корень (все exe/DLL своей битности рядом, **без** подкаталогов `Sys32`/`Sys64`):

| Пакет | Каталог по умолчанию | Содержимое |
|-------|----------------------|------------|
| x64 MSI | `%ProgramFiles%\GlassPlatform` | `GlassCompat.dll` и peer x64, `DeveloperCodeSigning.cer`/`.txt`; UI в подкаталоге `GlassSettings\`; меню рабочего стола в подкаталоге `GlassShellSparse\` (`GlassShellExt.dll`, `GlassShellHost.exe`, `GlassShellSparse.msix`, манифесты) |
| x86 MSI | `%ProgramFiles(x86)%\GlassPlatform` | то же для x86 |

Пользователь может выбрать другой путь при установке. Фактический путь пишется в `InstallPath`.

```
Program Files\GlassPlatform          ← x64
Program Files (x86)\GlassPlatform    ← x86
```

На OS x86 существует только x86-пакет (`%ProgramFiles%\GlassPlatform`).

## Реестр: `InstallPath` и Wow6432Node

Ключ: `Software\GlassPlatform`. Актуальные значения — `InstallPathX64` и `InstallPathX86`
(REG_SZ), основная ветка — **HKLM** в представлении своей разрядности
(`KEY_WOW64_64KEY` для x64, `KEY_WOW64_32KEY` для x86). Это не ключ настроек
`Software\StanislavKa\GlassPlatform`.

### Порядок чтения (`ReadPlatformInstallPath`, `PlatformInstallRegistry`)

| Приоритет | Ветка и значение | Метка в журнале |
|---|---|---|
| 1 | `HKLM\Software\GlassPlatform\InstallPathX64` / `…\InstallPathX86` (view своей разрядности) | `HKLM(64) InstallPathX64` / `HKLM(32) InstallPathX86` |
| 2 | `HKLM\Software\GlassPlatform\InstallPath` — прежняя схема | `HKLM(64) InstallPath` / `HKLM(32) InstallPath` |
| 3 | `HKCU\Software\GlassPlatform\InstallPathX64` / `…\InstallPathX86` | `HKCU InstallPathX64` / `HKCU InstallPathX86` |
| 4 | только x86 на OS x64: `HKCU\Software\Wow6432Node\GlassPlatform\InstallPath` | `HKCU Wow6432Node InstallPath` |
| 5 | `HKCU\Software\GlassPlatform\InstallPath` | `HKCU InstallPath` |

Правила:

- Машинная ветка всегда проверяется **до** пользовательской; фактические ветка и значение
  пишутся в журнал (`Startup installation roots: …`), ничего не «исправляется» молча.
- Признак «платформа arch установлена»: непустой путь и файл `<path>\GlassCompat.dll`.
- Trailing slash нормализуется.
- x86-процесс на OS x64 читает 32-битное представление (`KEY_WOW64_32KEY`), без ручного
  `Wow6432Node` в пути.

### OpenGlass

`HKLM\Software\StanislavKa\GlassPlatform\OpenGlassInstallPath`, затем тот же путь в HKCU,
затем `HKCU\Software\StanislavKa\OpenGlass\InstallPath` (метка источника — в журнале).

Поиск модулей Debug и Release одинаковый: GlassCompat / LogViewer / инжект ищут peer-DLL
**рядом со своим модулем** (свой install root), без реестра.

## MSI: запись реестра

Два пакета платформы (x86 и x64). Каждый кладёт файлы только своей битности и пишет только
свой view.

- **x64-пакет:** компонент Registry в 64-bit view (`Win64="yes"`), значения `InstallPathX64`
  и (для совместимости) прежний `InstallPath`.
- **x86-пакет на OS x64:** 32-bit MSI, тот же ключ в таблице, значения `InstallPathX86` /
  `InstallPath`; система сама перенаправляет запись в 32-битное представление. **Не**
  дублировать `Wow6432Node` руками в таблице Registry.
- **Uninstall:** каждый MSI удаляет только свои значения/view и свои файлы.
- Проверка после установки: `regedit` → `HKLM\Software\GlassPlatform` (значения
  `InstallPathX64`/`InstallPathX86` в представлении своей разрядности).

## Задачи планировщика

GS создаёт задачу, когда пользователь включает тумблер на странице «Система». `/TR`:

- x64: `"%SystemRoot%\System32\rundll32.exe" "<x64 InstallPath>\GlassCompat.dll",Main /start`
- x86: `"%SystemRoot%\SysWOW64\rundll32.exe" "<x86 InstallPath>\GlassCompat.dll",Main /start`

Имена: `GlassPlatform.Win32 Autorun Task (x64)` / `(x86)`.

## MSI: platform и GlassSettings

В одном каталоге `GlassPlatform` живут **два** скрытых продукта Windows Installer:

| MSI | ProductCode | UpgradeCode | ARP |
|-----|-------------|-------------|-----|
| GlassSettings-x64 | `…5074` | `…5040` | `ARPSYSTEMCOMPONENT=1` (скрыт) |
| GlassSettings-x86 | `…5075` | `…5050` | скрыт |
| Glass-x64 / x86 | `…5071` / `…5072` | как в wixproj | виден в «Установка и удаление» |
| OpenGlass-x64 | `…5073` | — | виден |

GlassSettings MSI ставит только дерево `GlassSettings\` (включая `GlassSettings.Native.dll`). Platform MSI — peer DLL, `unins000.exe` / `unins000.dat`, задачи.

`unins000.dat` (platform x64/x86):

```ini
PRODUCT_CODE={…5071}
SETTINGS_PRODUCT_CODE={…5074}
```

(x86: `5072` / `5075`; OpenGlass — только `PRODUCT_CODE`).

`GlassUnins` (`/MT`, Common Controls v6): копия в `%WINDIR%\Temp\GlassUnins-<guid>`, запуск
`/fromtemp "<installDir>"`, остановка платформы через `Shared::GlassStop`
(`Reason::Uninstaller`), затем `/watchGlassModules` и `MsiConfigureProductExW` ABSENT для
platform, затем для `SETTINGS_PRODUCT_CODE`. После завершения работы копия и её каталог
удаляются; хвосты прошлых запусков убираются при следующем старте (`Stale private copies`).

Сборка WiX: `Build\GlassSetup\msi\$(Configuration)\$(ProjectName)\`, промежуточное — `Cache\$(ProjectName)\`.

## GlassSettings

- x64 и x86 — отдельные ярлыки. На OS x64 основной UI — GS x64.
- GS x64 на странице «Система»: если x86 **не** установлена — спойлер «Платформа Glass x86» заблокирован, тумблер OFF. Если установлена — GS x64 владеет модулями x86 и может создать задачу.
- Dev без установщика: запускать **GS своей разрядности** из `Build\x64\{Config}\` / `Build\Win32\{Config}\`. Cross-arch из GS x64 в dev не поддерживается (нет Wow6432Node).

## GlassShellExt

Регистрируется с x64-пакетом. Управляет **только задачами** (start/stop через Task Scheduler). Пути `GlassCompat.dll` не резолвит. `GlassSettings.exe` ищет от своего каталога `<root>\GlassShellSparse\` на уровень выше — `<root>\GlassSettings\` (пункт «Настройки»). На Windows 11 без доверия сертификату разработчика регистрируется классический пункт (после «Показать дополнительные параметры»); подробности — `GlassShellExt/README.md`.
