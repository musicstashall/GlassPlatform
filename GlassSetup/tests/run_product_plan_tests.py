"""Compile/run the portable policy tests without starting any Glass processes.

Run from the solution root:
    python GlassSetup/tests/run_product_plan_tests.py
Uses cl (Developer Command Prompt), clang++ or g++; all outputs stay in a
TemporaryDirectory. This is not a Windows MSI/registry integration test.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    source = Path(__file__).resolve().with_name("product_plan_tests.cpp")
    compiler = next((shutil.which(name) for name in ("cl", "clang++", "g++")
                     if shutil.which(name)), None)
    if compiler is None:
        raise SystemExit("No C++ compiler found; use a VS Developer Command Prompt or install clang++/g++.")
    with tempfile.TemporaryDirectory(prefix="GlassProductPlanTests-") as directory:
        executable = Path(directory) / "product_plan_tests.exe"
        if Path(compiler).stem.lower() == "cl":
            command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", "/utf-8",
                       str(source), "/Fe:" + str(executable)]
        else:
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       str(source), "-o", str(executable)]
        for test in ("product_plan_tests.cpp","maintenance_executor_tests.cpp"):
            current=source.with_name(test)
            subprocess.run([str(current) if arg==str(source) else arg for arg in command],cwd=directory,check=True)
            subprocess.run([str(executable)],cwd=directory,check=True)


if __name__ == "__main__":
    main()
