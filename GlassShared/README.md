# GlassShared

**Связанный код:** `GlassShared/` — static library, общий код для host и peer DLL (GlassModulePath, RegHelper, GlassPlatformTask, сообщения `WM_GLASS_*`, политики inject и т.д.).

Единственная допустимая **статическая** линковка между проектами solution для peer/host модулей. Подробнее: [ARCHITECTURE.md](../ARCHITECTURE.md).

## Документация

| Задача | Документ |
|--------|----------|
| GDI и альфа | [docs/gdi-alpha.md](docs/gdi-alpha.md) |
| InstallPath x86/x64, Wow6432Node | [docs/platform-installer.md](docs/platform-installer.md) |
