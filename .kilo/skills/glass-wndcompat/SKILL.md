---
name: glass-wndcompat
description: WndCompat — Host vs Subclass, Detach, GlassAttachGate, WM_GLASS_DETACH_*.
---

# WndCompat

- *Host.cpp vs *Subclass.cpp
- RemoveWindowSubclass только в потоке окна; иначе WM_GLASS_DETACH_* + SendMessageTimeoutW
- GlassAttachGate, ComboLBox exception

См. docs/modules/wndcompat.md
