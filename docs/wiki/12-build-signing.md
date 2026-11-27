# 12 · Сборка, 5 MSI-пакетов установщика и подпись

`GlassSetup` упаковывает систему в модульный набор из 5 инсталляторов MSI под общим
Bootstrapper, проверяет сертификат кода (`CN=Stanislav K.`) и предотвращает конфликты
при обновлении x86/x64 бинарников в памяти.

| Компонент установки | Содержимое | Условия развёртывания |
|---|---|---|
| `01 · GlassCore.msi` | `GlassCompat`, `GlassCore`, `WndCompat`, Win11 IXDE DLL (x64 + x86) | обязателен; перезагружает активные инжекции через `GlassStop` перед заменой |
| `02 · GlassHost64.msi` | 64-битный хост на `Rundll32` + задача автозапуска | основной канал перехватов 64-разрядных процессов |
| `03 · GlassHost32.msi` | 32-битный хост для WOW64-приложений | поддержка 32-битных Win32-программ |
| `04 · GlassSettings.msi` | .NET 8 / WinUI 3 панель + `LogViewer` | ярлык меню Пуск, схема `.glassbackup` |
| `05 · GlassShellExt.msi` | COM-расширение проводника + `GlassShellSparse` | опционально; MSIX Sparse Identity для Win11-меню |

## Подпись

Все бинарные файлы подписываются сертификатом Code Signing `CN=Stanislav K.`
(сейчас self-signed, переход на SignPath в CI запланирован).

| | |
|---|---|
| Subject | `CN=Stanislav K.` |
| SHA-1 thumbprint | `A801E5F5F430D6D19164C856F79B8EFD23F83EB8` |
| SHA-256 fingerprint | `1295F4BD2E1E7D710B380ECE573E8C3C021AE1E21C0E77174E5307E68A7DC93D` |
| Публичный сертификат | `Shared/certs/DeveloperCodeSigning.cer` |
| Действителен | 2026-11-01 .. 2031-11-01 |

SmartScreen и UAC для self-signed показывают «Неизвестный издатель» — подпись
подтверждает авторство и целостность файла, а не репутацию издателя.

## Где смотреть подробности

| Документ | Где | Ссылка |
|---|---|---|
| Полный состав подписи, таблица отпечатков, точки подписи в сборке | **только GitHub** (на сайте отдельной страницы нет) | [`docs/signing.md`](https://github.com/musicstashall/GlassPlatform/blob/master/docs/signing.md) |
| Сборка из корня solution (x64 Debug → `Build\x64\Debug\`) | GitHub | [`contributing/build.md`](https://github.com/musicstashall/GlassPlatform/blob/master/contributing/build.md) |
| Публикация комплекта и отдельных MSI | GitHub | [`GlassSetup/docs/release-publish.md`](https://github.com/musicstashall/GlassPlatform/blob/master/GlassSetup/docs/release-publish.md) |

Источник: [docs.html · раздел 12](https://glass-platform.ru/docs.html) · ← [11](11-tools) · → [13](13-openglass-textures)
