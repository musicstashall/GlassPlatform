#pragma once

#include <guiddef.h>

EXTERN_C extern const CLSID CLSID_GlassDesktopShellMenu;
EXTERN_C extern const CLSID CLSID_GlassDesktopToggleExplorerCommand;
EXTERN_C extern const CLSID CLSID_GlassDesktopOpenConfigExplorerCommand;

/// Canonical name для IExplorerCommand::GetCanonicalName (не совпадает с CLSID класса).
EXTERN_C extern const GUID GUID_GlassShellToggleCommandCanonical;
EXTERN_C extern const GUID GUID_GlassShellToggleX86CommandCanonical;
EXTERN_C extern const GUID GUID_GlassShellToggleX64CommandCanonical;
EXTERN_C extern const GUID GUID_GlassShellOpenGlassToggleCommandCanonical;
EXTERN_C extern const GUID GUID_GlassShellOpenConfigCommandCanonical;
