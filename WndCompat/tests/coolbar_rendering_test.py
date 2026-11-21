#!/usr/bin/env python3
"""Execute the real CoolBar C++ sources with Win32/IAT/renderer test doubles.

Checks routing, DC isolation, native passthrough, and balanced hook lifetimes.
It deliberately does not claim to verify Windows pixels or alpha blending.
All generated include shims and binaries live in a temporary directory.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / 'WndCompat/tests'


class CoolBarRenderingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='coolbar-rendering-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        stubs = tmp / 'include'
        for header in (
            'CorePch.h', 'WndPch.h', 'windows.h', 'uxtheme.h', 'vssym32.h',
            'wil.h', 'DebugLog.h', 'GlassControlKind.h', 'GlassBackdropMark.h',
            'GlassMessages.h', 'InjectionMode.h', 'WndCompat.h',
            'Subsystems/RenderText.h', 'Subsystems/PaintDispatcher.h',
            'Subsystems/RootSwcaGradient.h', 'Core/Intercept/HookDispatcher.h',
            'Module/ModuleHandler.h', 'Tracker/WinEventHandler.h',
        ):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        cls.binary = tmp / 'coolbar-tests'
        command = compiler + [
            '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
            '-I', str(stubs), '-I', str(ROOT / 'GlassCore/Include'),
            '-I', str(ROOT / 'WndCompat'),
            '-I', str(ROOT / 'GlassShared/include/Providers'),
            str(TESTS / 'coolbar_rendering_tests.cpp'), '-o', str(cls.binary),
        ]
        result = subprocess.run(command, capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise AssertionError(f'CoolBar C++ test build failed:\n{result.stdout}{result.stderr}')

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_raw_tcoolbar_class_attach_and_recursive_catchup(self):
        self.run_case('exact-class-attach')

    def test_probe_text_routes_start_at_real_subclass_not_premarked_session(self):
        self.run_case('probe-through-subclass')

    def test_themed_classic_and_glyph_text_routes(self):
        self.run_case('text-routes')

    def test_only_owned_or_explicit_print_dcs_are_modified(self):
        self.run_case('dc-ownership')

    def test_text_background_and_saved_dc_state(self):
        self.run_case('transparent-text-state')

    def test_measurement_params_reentry_and_per_module_originals(self):
        self.run_case('native-semantics')

    def test_nested_sessions_aliases_late_modules_and_shutdown(self):
        self.run_case('sessions-and-modules')

    def test_background_is_cleared_without_wiping_chrome(self):
        self.run_case('background-and-chrome')

    def test_printclient_forwards_native_paint_and_pairs_callbacks(self):
        self.run_case('printclient-bridge')


if __name__ == '__main__':
    unittest.main()
