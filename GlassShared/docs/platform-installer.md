# Установщик платформы Glass (концепт)

**Связанный код:** [`GlassSettings/Core/Platform/GlassModulePath.cs`](../../GlassSettings/Core/Platform/GlassModulePath.cs), [`GlassSettings/Core/Platform/PlatformInstallRegistry.cs`](../../GlassSettings/Core/Platform/PlatformInstallRegistry.cs), [`GlassShared/src/GlassModulePath.cpp`](../../GlassShared/src/GlassModulePath.cpp)

Документ закрепляет целевую схему установки. Сами MSI в репозитории пока не собираются — это спецификация для будущего установщика и для текущего поиска модулей в GlassSettings.

## Раскладка на диске (OS x64)

Два независимых каталога, плоский корень (все exe/DLL своей битности рядом, **без** подкаталогов `Sys32`/`Sys64`):

| Пакет | Каталог по умолчанию | Содержимое |
|-------|----------------------|------------|
| x64 MSI | `%ProgramFiles%\GlassPlatform` | `GlassCompat.dll` и peer x64, `GlassShellExt.dll`; UI в подкаталоге `GlassSettings\` |
| x86 MSI | `%ProgramFiles(x86)%\GlassPlatform` | то же для x86 |

Пользователь может выбрать другой путь при установке. Фактический путь пишется в `InstallPath`.

```
Program Files\GlassPlatform          ← x64
Program Files (x86)\GlassPlatform    ← x86
```

На OS x86 существует только x86-пакет (`%ProgramFiles%\GlassPlatform`).

## Реестр: `InstallPath` и Wow6432Node

Ключ: `Software\GlassPlatform`, значение `InstallPath` (REG_SZ). Ветка — **HKCU**. Это не ключ настроек `Software\StanislavKa\GlassPlatform`. Разделение x64/x86 — через **registry view** (Wow6432Node), не через подключи `x64`/`x86`.

### OS x64

| Кто | Что пишет MSI | Физическое расположение | Кто читает |
|-----|---------------|-------------------------|------------|
| **x64 MSI** | `HKCU\Software\GlassPlatform\InstallPath` из 64-bit компонента | 64-bit view (без Wow6432Node) | GS x64 — native-модули из корня GS; этот ключ — путь установки x64 |
| **x86 MSI** | `HKCU\Software\GlassPlatform\InstallPath` из **32-bit** компонента | Система перенаправляет в `HKCU\Software\Wow6432Node\GlassPlatform\InstallPath` | GS x86 — автоматически (WOW64); **GS x64 для модулей x86 — явно `Software\Wow6432Node\GlassPlatform`** |

Два независимых `InstallPath` на одной машине: x64 в native HKCU, x86 в Wow6432Node. Они не перезаписывают друг друга.

### OS x86

Wow6432Node нет. Единственный ключ: `HKCU\Software\GlassPlatform\InstallPath`.

### Правила чтения в коде (`PlatformInstallRegistry`)

- **GS x86** (WOW64 / OS x86): `HKCU\Software\GlassPlatform` — ОС сама отдаёт 32-bit view на OS x64. **Не** открывать `Wow6432Node` из 32-bit процесса (двойное перенаправление).
- **GS x64, native x64:** `HKCU\Software\GlassPlatform`.
- **GS x64, cross-arch x86:** `HKCU\Software\Wow6432Node\GlassPlatform` — обязательно явный путь. 64-bit процесс без Wow6432Node прочитает x64-установку.
- Признак «платформа arch установлена»: непустой `InstallPath` и файл `<path>\GlassCompat.dll`.
- Trailing slash при чтении нормализуется.

Поиск модулей Debug и Release одинаковый:

| Процесс GS | Целевая arch | Install root |
|------------|--------------|--------------|
| GS x86 | x86 | корень GS (`AppContext.BaseDirectory`) |
| GS x64 | x64 | корень GS |
| GS x64 | x86 | `InstallPath` из Wow6432Node; нет ключа или нет `GlassCompat.dll` → модулей x86 нет |

GlassCompat / LogViewer / инжект ищут peer-DLL **рядом со своим модулем** (свой install root), без реестра.

## MSI: запись реестра

Два пакета (x86 и x64). Каждый кладёт файлы только своей битности и пишет только свой view.

- **x64-пакет:** компонент Registry в 64-bit view (`Win64="yes"`). Ключ в таблице: `Software\GlassPlatform`, значение `InstallPath` = `[INSTALLDIR]`.
- **x86-пакет на OS x64:** 32-bit MSI, ключ в таблице тоже `Software\GlassPlatform` (без Wow6432Node в компоненте). Система перенаправляет запись в Wow6432Node. **Не** дублировать `Wow6432Node` руками в таблице Registry.
- **Uninstall:** каждый MSI удаляет только свой ключ/view и свои файлы. Снятие x64 не трогает x86 и наоборот.
- После установки проверить в `regedit`: native `HKCU\Software\GlassPlatform\InstallPath` и `HKCU\Software\Wow6432Node\GlassPlatform\InstallPath` — два разных пути.

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

`GlassUnins` (`/MT`, Common Controls v6): копия в `%TEMP%`, `/fromtemp "<installDir>"`, стоп хоста + `/watchGlassModules`, затем `MsiConfigureProductExW` ABSENT для platform, затем для `SETTINGS_PRODUCT_CODE`.

Сборка WiX: `Build\GlassSetup\msi\$(Configuration)\$(ProjectName)\`, промежуточное — `Cache\$(ProjectName)\`.

## GlassSettings

- x64 и x86 — отдельные ярлыки. На OS x64 основной UI — GS x64.
- GS x64 на странице «Система»: если x86 **не** установлена — спойлер «Платформа Glass x86» заблокирован, тумблер OFF. Если установлена — GS x64 владеет модулями x86 и может создать задачу.
- Dev без установщика: запускать **GS своей разрядности** из `Build\x64\{Config}\` / `Build\Win32\{Config}\`. Cross-arch из GS x64 в dev не поддерживается (нет Wow6432Node).

## GlassShellExt

Регистрируется с x64-пакетом. Управляет **только задачами** (start/stop через Task Scheduler). Пути `GlassCompat.dll` не резолвит. `GlassSettings.exe` ищет рядом с собой (пункт «Настройки»).
