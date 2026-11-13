"""Exercise the actual Native registration code with a portable fake registry.
Does not test Windows privilege enforcement, WRP, MSI, COM loading or reboot.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "GlassSettings/Native/SecureUxThemeNative.cpp").read_text(encoding="utf-8")
constants = source[source.index("    constexpr wchar_t kOurDllName"):source.index("    HMODULE g_loadedPatcher")]
tokens = source[source.index("    [[nodiscard]] bool TokenEquals"):source.index("    void ReadIfeo(")]
registration = source[source.index("    struct RegistrationValue"):source.index('\n}\n\nextern "C" HRESULT WINAPI GlassSettings_SecureUxThemeQuery')]
harness = Path(__file__).with_name("secure_ux_registration_mock.cpp").read_text(encoding="utf-8")
harness = harness.replace("// INSERT_NATIVE_REGISTRATION_HERE", constants + tokens + registration)
compiler = next((shutil.which(name) for name in ("cl", "clang++", "g++") if shutil.which(name)), None)
if not compiler:
    raise SystemExit("Use a VS Developer Command Prompt or install clang++/g++")
with tempfile.TemporaryDirectory(prefix="GlassSecureUxRegistrationTests-") as directory:
    path = Path(directory) / "registration.cpp"
    exe = Path(directory) / "registration_tests.exe"
    path.write_text(harness, encoding="utf-8")
    if Path(compiler).stem.lower() == "cl":
        command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/utf-8", str(path), "/Fe:" + str(exe)]
    else:
        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-pedantic", str(path), "-o", str(exe)]
    subprocess.run(command, cwd=directory, check=True)
    subprocess.run([str(exe)], cwd=directory, check=True)
