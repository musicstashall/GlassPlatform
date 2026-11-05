"""Compile/run real release JSON parser with a captured official API manifest.
No Windows/MSI operations or network access. Outputs are temporary.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    directory = Path(__file__).resolve().parent
    compiler = next((shutil.which(name) for name in ("cl", "clang++", "g++") if shutil.which(name)), None)
    if not compiler:
        raise SystemExit("Use a VS Developer Command Prompt or install clang++/g++")
    with tempfile.TemporaryDirectory(prefix="GlassSecureUxReleaseTests-") as temporary:
        executable = Path(temporary) / "secure_ux_release_tests.exe"
        source = directory / "secure_ux_release_tests.cpp"
        if Path(compiler).stem.lower() == "cl":
            command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/utf-8", str(source), "/Fe:" + str(executable)]
        else:
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(executable)]
        subprocess.run(command, cwd=temporary, check=True)
        subprocess.run([str(executable), str(directory / "fixtures/secure_ux_release_v4.json")], cwd=temporary, check=True)


if __name__ == "__main__":
    main()
