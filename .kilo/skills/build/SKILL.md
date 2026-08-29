---
name: build
description: MSBuild GlassProject.sln из корня решения. Не запускать и не останавливать платформу.
---

# Сборка GlassProject

Сборка только из корня `D:\VS_PROJECT\DWM\GlassProject\`.

## Команда сборки всех проектов

```bash
cd /d/VS_PROJECT/DWM/GlassProject && \
"C:/Program Files/Microsoft Visual Studio/2022/Professional/MSBuild/Current/Bin/MSBuild.exe" \
  GlassProject.sln //p:Configuration=Debug //p:Platform=x64
```
## Команда сборки отдельных проектов

```bash
& "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" "d:\VS_PROJECT\DWM\GlassProject\GlassProject.sln" /t:UICompat;GlassCompat;WndCompat /p:Configuration=Debug /p:Platform=x64
```

Артефакты: `Build\x64\Debug\`.

## Публикация GlassSettings

После сборки solution, публикация GlassSettings выполняется отдельно:

```bash
& "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" "d:\VS_PROJECT\DWM\GlassProject\GlassSettings\GlassSettings.csproj" /p:Configuration=Debug /p:Platform=x64 /t:Publish /p:PublishProfile=win-x64
```

Для x86: заменить `x64` на `x86` и `win-x64` на `win-x86`.

Артефакты публикации: `Build\x64\Debug\GlassSettings\` (single-file exe + WASDK native DLLs).

## Запрет для ассистента

- **Не** запускать/останавливать GlassPlatform (LogViewer, Rundll32 `/start`/`/stop`, `stop.vbs`).
- **Не** завершать процессы ради разблокировки DLL.
- Запуск и тесты — **только пользователь**.

## LNK1168

Сообщить, какой DLL занят. Задать вопрос пользователю. **Ждать ответа.** Повторять MSBuild без ответа — нельзя.

## Прочие ошибки компиляции

Исправлять в цикле до успешной сборки.

Подробнее: [docs/contributing/build.md](../../docs/contributing/build.md)
