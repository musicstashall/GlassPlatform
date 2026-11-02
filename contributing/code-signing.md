# Подпись сборок: ключ автора, смена ПК, восстановление

Контракт для пользователей (что подписано, как проверить) — корневой `README.md`, раздел «Подпись сборок».
Механика сборки — `Shared/GlassSign.targets` и `GlassSetup/README.md`. Здесь — действия владельца ключа.

## Что где лежит

| | |
|---|---|
| Сертификат | `CN=Stanislav K.`, SHA-1 `A801E5F5F430D6D19164C856F79B8EFD23F83EB8`, действует до 2031-11-01 |
| Закрытый ключ | только в хранилище Windows машины автора: `Cert:\CurrentUser\My\A801E5F5…3EB8` |
| Резервная копия ключа | `GlassAuthorCodeSigning.pfx` (зашифрован паролем) — вне репозитория, на внешнем носителе/облаке; пароль хранится отдельно |
| Публичная часть | `Shared/certs/GlassAuthorCodeSigning.cer` — в репозитории, секретов не содержит |
| Пин отпечатка | `Shared/GlassSign.targets` → `<GlassSignThumbprint>` и таблица в `README.md` |

Файлов `*.pfx` в рабочей копии быть не должно (`.gitignore` их отбрасывает, но и класть не нужно).

Этим же сертификатом подписан sparse MSIX контекстного меню (`GlassShellSparse\GlassShellSparse.msix`), и его Subject
**равен** `Identity@Publisher` обоих `GlassShellExt/Package/AppxManifest*.xml` (`CN=Stanislav K.`) — правило MSIX.
Отсюда семейство пакета `GlassProject.GlassShellMenu_83517z3ecdax2` (`GlassShellMenuConstants.h`); проверка —
`python GlassSetup/tests/sparse_identity_mock.py`. Отдельного «dev»-сертификата для MSIX нет; сборка без ключа автора
даёт неподписанный msix (на Win11 — классический fallback меню).

## Компактное меню Windows 11: сертификат автора на машине пользователя

Windows регистрирует sparse-пакет только от издателя, сертификат которого есть в «Доверенных лицах»
(`LocalMachine\TrustedPeople`). Установка сертификата — всегда осознанный выбор пользователя:

- **Установщик** (Win11 x64) показывает отдельную страницу «Сертификат автора» с объяснением, что это и зачем;
  её можно пропустить. При согласии helper (`GlassSettings_ShellSignerTrustSetupV1W`) добавляет сертификат
  из подписи `GlassShellSparse.msix` в `LocalMachine\TrustedPeople` — единственное место записи
  (`ShellSparsePackage::TrustSparseSigner`). Отказ — обычный сценарий: меню остаётся классическим.
- **GlassSettings** → Система: уведомление о непринятом сертификате со ссылкой «Установить сертификат автора
  и включить компактное меню» и ссылкой на папку с файлом сертификата.
- **EULA** (RU/EN, раздел «Сертификат автора») описывает сертификат, его назначение и право не устанавливать его.
- **GlassUnins сертификат никогда не удаляет** — это явное решение пользователя (остаётся в хранилище до ручного
  удаления через `certlm.msc` → «Доверенные лица»).
- Сборка ничего не создаёт и не экспортирует (`New-SelfSignedCertificate`, `Import-Certificate` и т. п. запрещены).

Ручной путь (резервный, для автора) — тот же файл:

```powershell
Import-Certificate -FilePath .\Shared\certs\GlassAuthorCodeSigning.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople
```

Инструкция для пользователей — `Shared/certs/GlassAuthorCodeSigning.txt` (ставится в корень платформы рядом с `.cer`).

## Смена ПК или переустановка ОС

Нужны: `GlassAuthorCodeSigning.pfx`, его пароль, клон репозитория. Больше ничего из старой системы не требуется.

1. Установить Visual Studio с рабочей нагрузкой C++ и Windows SDK (в SDK входит `signtool.exe`), WiX — как для обычной сборки.
2. Импортировать ключ в хранилище текущего пользователя (без прав администратора):

   ```powershell
   Import-PfxCertificate -FilePath <путь>\GlassAuthorCodeSigning.pfx `
     -CertStoreLocation Cert:\CurrentUser\My -Exportable -Password (Read-Host -AsSecureString 'PFX password')
   ```

   `-Exportable` оставляет возможность снова снять резервную копию с этой машины.
3. Проверить, что ключ на месте и это тот самый сертификат:

   ```powershell
   Get-Item Cert:\CurrentUser\My\A801E5F5F430D6D19164C856F79B8EFD23F83EB8 | Format-List Subject, HasPrivateKey, NotAfter
   ```

   Ожидается `HasPrivateKey : True`. Если `Get-Item` ничего не находит — импорт прошёл в другое хранилище (например, под другим пользователем или с правами администратора в `LocalMachine`).
4. **Rebuild** Release|x64, затем Release|x86. В Output должны быть строки `GlassSign: [Проект] signed (timestamp …)`.
5. Контроль комплекта:

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .\Shared\scripts\Test-GlassSignatures.ps1
   ```

   Итоговая строка должна быть `RESULT: all first-party artifacts are signed by the expected certificate.`

Скрипт `New-GlassSelfSignedCert.ps1` при переезде **не запускать**: он создаёт новый ключ с новым отпечатком.

## Старый ПК

Если старая система остаётся в работе, но подписывать с неё больше не планируется — удалить ключ вместе с закрытой частью:

```powershell
Remove-Item Cert:\CurrentUser\My\A801E5F5F430D6D19164C856F79B8EFD23F83EB8 -DeleteKey
```

Перед этим убедиться, что `.pfx` открывается паролем (например, `Get-PfxCertificate <путь>\GlassAuthorCodeSigning.pfx`).

## Ключ утерян (нет ни хранилища, ни `.pfx`)

Восстановить ключ невозможно; выпускается новый сертификат, и это видимое для пользователей событие.

1. `powershell -NoProfile -ExecutionPolicy Bypass -File .\Shared\scripts\New-GlassSelfSignedCert.ps1` (на машине без старого ключа `-Force` не нужен).
2. Сразу сделать резервную копию нового ключа (команда печатается скриптом).
3. Обновить пин: `Shared/GlassSign.targets` → `<GlassSignThumbprint>`; `README.md` → таблица сертификата. Старый отпечаток из README не удалять, а перенести в строку «предыдущие сертификаты» с датами — опубликованные ранее сборки подписаны именно им.
4. Закоммитить новый `Shared/certs/GlassAuthorCodeSigning.cer` (старый `.cer` можно оставить рядом с суффиксом даты) и обновить отпечаток/срок в `Shared/certs/GlassAuthorCodeSigning.txt`.
5. Если изменился Subject — обновить `Publisher` обоих `AppxManifest*.xml`, `kSparsePublisher` и `kSparsePackageFamilyName` в `GlassShellMenuConstants.h` (новое семейство печатает `sparse_identity_mock.py`); прежний sparse-пакет у пользователей не мигрируется.
6. Rebuild обеих фаз и `Test-GlassSignatures.ps1`.

## Истечение срока (2031-11-01)

Подписи с меткой времени остаются действительными после истечения срока сертификата — старые выпуски не «ломаются».
Для новых сборок за 1–2 месяца до даты выпустить новый сертификат по сценарию «Ключ утерян» (старый ключ при этом можно удалить после первого успешного выпуска).

## Если сборка ругается

| Сообщение | Причина | Действие |
|---|---|---|
| `warning GSIGN001` | ключ с пином не найден в `CurrentUser\My` / `LocalMachine\My` | шаги 2–3 выше; сборка при этом собирается без подписи |
| `error GSIGN107` | в хранилище есть ключ `CN=Stanislav K.`, но с другим отпечатком | импортирован не тот `.pfx`, либо случайно создан новый ключ — удалить лишний (`Remove-Item … -DeleteKey`) |
| `error GSIGN101` | не найден `signtool.exe` | доустановить Windows SDK или задать переменную окружения `GLASS_SIGNTOOL` с полным путём |
| `warning GSIGN002` | ни один сервер меток времени не ответил | нет сети; для осознанной офлайн-сборки `GlassSignNoTimestamp=true`, иначе повторить позже |
| `error GSIGN106` в GlassSetup | в payload попал неподписанный MSI/DLL | Rebuild фазы, которая его собирает (x64 для `*-x64.msi`, x86 для `*-x86.msi`) |

## Переход на SignPath (или любой внешний подписант)

Точки подписи и проверка не меняются: сборка CI идёт с `GlassSign=Skip`, подписант подписывает те же файлы в том же порядке
(PE и MSI, затем `GlassSetup.exe`), после чего `Test-GlassSignatures.ps1 -Thumbprint <новый>` проверяет комплект.
В репозитории меняются только пин в `GlassSign.targets`, таблица README, `.cer`/`.txt` и — при другом Subject — Publisher/семейство sparse-пакета (см. выше).
