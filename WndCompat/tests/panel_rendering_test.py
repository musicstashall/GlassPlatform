#!/usr/bin/env python3
"""Execute production VCL panel hooks/message bridge with Win32/IAT doubles.

Checks native message routing, true-alpha DIB bytes and scoped hook state, not
Windows pixel output, actual IAT chains, or MSVC build compatibility.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PanelRenderingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='panel-rendering-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        stubs = tmp / 'include'
        for header in (
            'CorePch.h', 'WndPch.h', 'windows.h', 'wil.h', 'DebugLog.h',
            'GlassBackdropMark.h', 'InjectionMode.h', 'WndCompat.h',
            'Tracker/WinEventHandler.h', 'Core/Intercept/HookDispatcher.h',
            'Module/ModuleHandler.h', 'Subsystems/PaintDispatcher.h',
            'Subsystems/RootSwcaGradient.h',
        ):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        cls.binary = tmp / 'panel-tests'
        result = subprocess.run(compiler + [
            '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
            '-Wno-unknown-pragmas', '-I', str(stubs),
            '-I', str(ROOT / 'GlassCore/Include'), '-I', str(ROOT / 'WndCompat'),
            '-I', str(ROOT / 'GlassShared/include'),
            str(ROOT / 'WndCompat/tests/panel_rendering_tests.cpp'), '-o', str(cls.binary),
        ], capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise AssertionError(f'Panel C++ test build failed:\n{result.stdout}{result.stderr}')

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_exact_classes_and_native_erase_reach_color_brush(self):
        self.run_case('classes-and-erase')

    def test_custom_alpha_100_system_black_and_nonaccumulating_repaints(self):
        self.run_case('alpha-colors')

    def test_native_brush_apis_and_preserved_chrome(self):
        self.run_case('apis-and-chrome')

    def test_dc_ownership_and_native_printclient(self):
        self.run_case('dc-and-print')

    def test_mode_backdrop_missing_bridge_and_balanced_sessions(self):
        self.run_case('gates-and-sessions')

    def test_gdi_failure_cleanup_and_reentry_passthrough(self):
        self.run_case('failures-and-reentry')


if __name__ == '__main__':
    unittest.main()
