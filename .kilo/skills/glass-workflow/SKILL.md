---
name: glass-workflow
description: Workflow GlassProject — план, backup, подтверждение. Платформу и тесты запускает только пользователь.
---

# Glass workflow

1. План изменений -> подтверждение (ок/выполняй). Мелочи без плана по CLAUDE.
2. Backup в `.claude/backups/<дата>/` перед правками.
3. После **создания** файла — проверить UTF-8 (не UTF-16); чинить сразу, пока файл пустой/короткий.
3a. Разовые `.py`/`.ps1` — только `.claude/tmp/`; после использования удалить (не корень, не `tools/` без запроса).
4. **Не** запускать/останавливать GlassPlatform, **не** вызывать stop.vbs/Rundll32, **не** убивать процессы.
5. Запуск и тесты — только пользователь (LogViewer, VS, stop.vbs).
6. Русский язык ответов и комментариев.

См. [workflow.md](../../docs/contributing/workflow.md), [build.md](../../docs/contributing/build.md), [file-encoding.md](../../docs/contributing/file-encoding.md), [repo-hygiene.md](../../docs/contributing/repo-hygiene.md).
