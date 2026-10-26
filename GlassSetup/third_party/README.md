# SecureUxTheme (Glass payload)

Бинарники из официального MSI [namazso/SecureUxTheme](https://github.com/namazso/SecureUxTheme) (x86/x64), для каталога `SecureUxTheme\` в MSI платформы. Обновление — распаковка `/a` соответствующего MSI в `SecureUxTheme\x86\` / `x64\`.

# WebView2 SDK

Vendored from the official NuGet package:

- Package: Microsoft.Web.WebView2
- Version: 1.0.3537.50
- URL: https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.3537.50
- `WebView2.h`: `build/native/include/WebView2.h`
- `WebView2LoaderStatic.lib`: `build/native/x86/WebView2LoaderStatic.lib`
- `LICENSE.txt`: the package license, unchanged.

GlassSetup — **только x86 (Win32)**. Loader статически линкуется; `WebView2Loader.dll` рядом с exe не нужна.

The Microsoft Edge WebView2 **Runtime** is not vendored here yet. For development, install Evergreen Runtime on the dev machine. The shipping installer must bundle and install Runtime offline — see [docs/guides/GlassSetup.md](../../docs/guides/GlassSetup.md).
