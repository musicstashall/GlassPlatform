# Извлечение продуктов из OpenGlassSetup.Signed.exe

Статус: portable parser и защищённый staging provider реализованы в
`GlassInnoExtract.h` / `GlassOpenGlassPackage.cpp`. Next/engine/inventory ещё
не переведены с MSI на file-based servicing: целиком схема пока не работает.
Parser проверен synthetic fixtures, реальный EXE/Windows Authenticode здесь
не проверены. Этот контракт остаётся источником формата, EXE заново не открывался.
Облачная модель не открывает `OpenGlassSetup.Signed.exe` заново: формат ниже уже снят
с релиза и является единственным источником.

Вместо своего `OpenGlass-x64.msi` Setup скачивает `OpenGlassSetup.Signed.exe` из релизов
ALTaleX531/OpenGlass и извлекает продукты своим разборщиком Inno Setup 6.7.
Установщик Glass ещё в разработке. Перехода с `OpenGlass-x64.msi` нет: семейство MSI
OpenGlass, его ProductCode и строка каталога удаляются, а не мигрируются.

Эталон, на котором процедура уже проверена: релиз `v3.0.2.3749`, asset
`OpenGlassSetup.Signed.exe` (4 049 232 байта, опубликован 2026-10-05). Подпись самого
setup — `Valid`, субъект `CN=Open Source Developer, Zhe Yang, O=Open Source Developer, L=Guangzhou, S=Guangdong, C=CN`.
Внутри Inno Setup 6.7.0, строка заголовка `Inno Setup Setup Data (6.7.0)`.

## Что лежит в установщике

Один номер версии на весь комплект. `Common/Version.props` задаёт `OpenGlassProductVersion`;
`Scripts/OpenGlass.Packaging.proj` передаёт его в ISCC как `/DMyAppVersion`. Тот же номер
становится `FILEVERSION`/`PRODUCTVERSION` обеих DLL, `OpenGlassHost.exe`, `OpenGlassGUI.exe`
и version resource самого setup. Архитектура DWM в номер не входит. Она только в
`FileDescription` DLL: `Aero Glass Extension for Desktop Window Manager (Legacy)` или
`(MILComp)`. Проекты — `OpenGlass.Legacy.vcxproj` и `OpenGlass.MILComp.vcxproj`,
свойство `OpenGlassDwmArchitecture`.

`Scripts/OpenGlass.iss` кладёт оба DLL в одно имя `{app}\OpenGlass.dll` и выбирает один:
сборка Windows `>= 28000` — MILComp, иначе Legacy. Host и GUI ставятся всегда.
Запускать setup нельзя: он создаёт запись удаления
`{D3D1BC7D-5E24-4B33-9383-7934271A3B05}_is1`, свою службу и ACL.

Срезы одного solid-потока в эталоне v3.0.2.3749, после LZMA и фильтра. Это пример,
не вечные константы размера. Узнавать файлы нужно по `OriginalFilename` и
`FileDescription`, а не по индексу.

- срез 0, смещение 0, 781 504 байта: `OpenGlass.dll`, описание заканчивается на `(Legacy)`, x64
- срез 1, смещение 781 504, 584 392 байта: `OpenGlass.dll`, описание заканчивается на `(MILComp)`, x64
- срез 2, смещение 1 365 896, 19 648 байт: `OpenGlassHost.exe`
- срез 3, смещение 1 385 544, 5 619 392 байта: `OpenGlassGUI.exe`

Все четыре — PE32+ (`Machine` `0x8664`, magic `0x20B`). Загрузчик setup при этом PE32 x86.
После правильного фильтра подпись каждого файла — `Valid`, тот же субъект Zhe Yang.
Неверный фильтр оставляет MZ и version resource, но даёт `HashMismatch`.

На диск Glass кладёт `OpenGlassHost.exe`, `OpenGlassGUI.exe` и ровно один DLL.
Лишний DLL не копировать.

## Разбор, по шагам

Не вызывать 7-Zip и не запускать exe. 7-Zip показывает только сырой хвост PE и секции загрузчика.

1. Проверить Authenticode setup до чтения payload. Статус не `Valid` или другой субъект — отказ.
   `SameMsiPublisher` не использовать: там сертификат Glass.
2. Найти таблицу смещений. Это ресурс `RT_RCDATA` с именем-числом `11111`, ровно 64 байта.
   Сигнатура в начале, 12 байт: ASCII `rDlPtS` и затем `CD E6 D7 7B 0B 2A`. В 6.7 таблица
   не XOR-кодируется. Поля little-endian:

- 0, 12 байт: сигнатура
- 12, `u32`: версия таблицы, ожидается `2`
- 16, `u64`: минимальный размер setup.exe
- 24, `u64`: `OffsetEXE`, сжатый мастер Inno (`setup.e32`). Это не продукты, не распаковывать его как DLL
- 32, `u32`: несжатый размер `setup.e32`
- 36, `i32`: CRC `setup.e32`
- 40, `u64`: `Offset0`, заголовок `setup-0.bin`
- 48, `u64`: `Offset1`, данные файлов `setup-1.bin`
- 56, `u32`: padding
- 60, `i32`: CRC предыдущих полей

На эталоне `Offset0 = 2703742` (`0x29417E`), `Offset1 = 893952` (`0xDA400`).
Если по `Offset0` нет ASCII `Inno Setup`, поменять `Offset0` и `Offset1` местами.

3. Заголовок по `Offset0` начинается с 64 байт ASCII `Inno Setup Setup Data (6.7.0)` и нулей.
   Другая версия строки или шифрование — отказ, формат не угадывать. Сразу после этих 64 байт
   идёт блок шифрования 53 байта. На эталоне пароля нет, распаковка без ключа проходит.
   Затем два сжатых блока заголовка.

Блок заголовка, не путать с `zlb` файлов:

- `u32` CRC
- `u64` stored size, считает и CRC-префиксы, и байты
- `u8` compressed (`1` = LZMA)
- дальше куски по 4096: каждые 4 байта CRC пропустить, затем взять `min(4096, остаток stored size)`. В stored size входят и эти 4 байта CRC

Если compressed = 0, склеенные байты уже готовы. Если 1: первые 5 байт склейки — свойства LZMA1,
дальше поток. Распаковать как LZMA alone: 5 байт свойств, затем 8 байт `0xFF` (размер неизвестен),
затем поток. Декодер останавливается по EOS. На эталоне первый блок stored = 153014 и даёт
899996 байт строк; второй stored = 223 и даёт 356 байт.

4. Записи данных брать из второго блока, не из эвристики строк `{app}` в первом.
   Размер записи 6.7.0 — 89 байт, число записей = длина / 89, остаток не ноль — отказ. Поля:

- 0, `u32` first slice
- 4, `u32` last slice
- 8, `u64` chunk offset
- 16, `u64` file offset внутри распакованного chunk
- 24, `u64` file size
- 32, `u64` chunk size
- 40, 32 байта SHA-256 уже восстановленного файла
- 72, `u64` timestamp
- 80, `u32` version MS
- 84, `u32` version LS
- 88, `u8` flags: бит `0x10` — chunk сжат, бит `0x04` — включён фильтр CALL/JMP

На эталоне 4 записи, у всех `chunk offset = 0`, `chunk size = 1809786`, оба флага установлены.
Смещения и размеры — список в разделе выше. Будущий релиз может добавить файлы; лишние записи
допустимы, отсутствие любого из четырёх продуктов — отказ.

5. Тело файлов лежит по `Offset1 + chunk offset` и начинается с `zlb\x1A` (`7A 6C 62 1A`).
   Это другая рамка, без CRC по 4096. Если бит сжатия снят, байты продукта идут сразу после
   4 байт сигнатуры. Если установлен: сразу после сигнатуры 5 байт свойств LZMA1
   (на эталоне `5D 00 00 80 00`, словарь 8 МБ), затем поток до EOS. Заголовок декодера снова
   `props + 8 байт 0xFF + поток`. Один распакованный chunk на эталоне — 7 004 936 байт;
   файлы — срезы `[file offset, file offset + file size)`.

6. Если флаг `0x04` установлен, снять фильтр Inno с уже распакованных байтов файла.
   Буфер строго 65536 байт, последний кусок короче. `AddrOffset` начинается с 0 и после куска
   увеличивается на его длину (`uint32`, переполнение допустимо). Другой размер буфера или
   фильтр на весь файл сразу ломает подпись: инструкции на стыке кусков не склеиваются,
   opcode в последних 4 байтах куска не обрабатывается. Это `TransformCallInstructions` из
   `Compression.Base.pas`, режим decode (`Encode = false`):

```pascal
if Size < 5 then Exit;
Dec(Size, 4);
I := 0;
while I < Size do begin
  if (P[I] = $E8) or (P[I] = $E9) then begin
    Inc(I);
    if (P[I+3] = $00) or (P[I+3] = $FF) then begin
      Addr := (AddrOffset + I + 4) and $FFFFFF;
      Rel := P[I] or (P[I+1] shl 8) or (P[I+2] shl 16);
      Dec(Rel, Addr);
      if Rel and $800000 <> 0 then
        P[I+3] := not P[I+3];
      P[I] := Byte(Rel);
      P[I+1] := Byte(Rel shr 8);
      P[I+2] := Byte(Rel shr 16);
    end;
    Inc(I, 4);
  end else
    Inc(I);
end;
```

`not` байта — инверсия младших 8 бит (`00` и `FF` меняются местами). `Dec` — вычитание
`uint32` с переносом.

7. Приёмка каждого нужного файла, все пункты сразу:

- PE32+, `Machine` `0x8664`
- Authenticode `Valid`, тот же субъект, что у setup
- `FileVersion` равен тегу релиза (`v` в начале тега отбрасывается)
- `OriginalFilename` равен `OpenGlass.dll`, `OpenGlassHost.exe` или `OpenGlassGUI.exe`
- у DLL `FileDescription` заканчивается на `(Legacy)` или `(MILComp)`; обеих меток по одному файлу
- SHA-256 восстановленных байтов совпадает с 32 байтами по смещению 40 записи. Inno считает этот хеш уже после фильтра

Пока все пункты не сошлись, файлы в каталог Glass не писать.

## Куда встраивать

Скачивать asset `OpenGlassSetup.Signed.exe` релиза
`api.github.com/repos/ALTaleX531/OpenGlass/releases/latest`, тег которого принимает
`GlassOpenGlassRelease::Parse`. Сравнение обновления остаётся по `FileVersion` DLL.
Ремонт дополнительно сравнивает архитектуру: переход сборки Windows через 28000
при том же номере заменяет DLL.

Точки текущей MSI-схемы, которые реализация заменяет целиком, без ветки совместимости:

- [GlassPackageCatalog.h](../src/InstallSupport/GlassPackageCatalog.h) — убрать `OpenGlass-x64.msi` из каталога Glass
- [GlassPackageAcquisition.cpp](../src/InstallSupport/GlassPackageAcquisition.cpp) — не требовать ProductCode/UpgradeCode MSI для OpenGlass
- [GlassOpenGlassRelease.h](../src/InstallSupport/GlassOpenGlassRelease.h) — тег разрешает скачать именно этот asset после проверки подписи
- шаг `OpenGlass-x64.msi` в [InstallEngine.cpp](../src/InstallEngine.cpp)
- строка OpenGlass в [procedure-map.md](procedure-map.md): снимок файлов и подписи вместо снимка MSI
- строка `GlassOpenGlassMsiVersion` в [msi-product-versions.md](msi-product-versions.md) и сборка `OpenGlass-x64.msi`

Файлы кладутся в каталог, которым уже владеет Glass. Запись удаления Inno не создаётся.
Службу поднимает существующий код Glass; правило автозапуска `!filesPresent || wasRunning`
не меняется. Уже установленный независимый Inno OpenGlass не перезаписывается.
