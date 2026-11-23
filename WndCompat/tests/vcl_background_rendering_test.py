#!/usr/bin/env python3
"""Run the complete production TBackgroundHooks unit with Win32 doubles.

Release and Debug/probe configurations exercise what the unit does with the
EXE's FillRect around BeginPaint of a VCL list: an erase that arrives inside
BeginPaint, an erase Windows had sent before it (window shown, restored or
resized), the class/mode/DC gates and the import reference count.
This is NOT Windows GDI/DWM, the update region, the VCL, the real PE import
table or a Windows/MSVC build acceptance.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class VclBackgroundRenderingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='vcl-background-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        stubs = tmp / 'include'
        for header in (
            'CorePch.h', 'windows.h', 'DebugLog.h', 'GlassControlKind.h', 'GlassBackdropMark.h',
            'GlassDelphiInputBackground.h', 'Hooks/delphi/TListBoxDraw.h',
            'Core/Intercept/HookDispatcher.h', 'Module/ModuleHandler.h',
            'Subsystems/PaintDispatcher.h', 'Subsystems/RootSwcaGradient.h',
        ):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        cls.binaries = {}
        for mode, flags in (
            ('Release', ['-O2']),
            ('Debug', ['-O0', '-g', '-D_DEBUG', '-DGLASS_DELPHI_DRAW_PROBE']),
        ):
            binary = tmp / f'vcl-background-{mode}'
            result = subprocess.run(compiler + [
                '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                '-Wno-unknown-pragmas', '-pthread', *flags,
                '-I', str(stubs), '-I', str(ROOT / 'GlassCore/Include'),
                str(ROOT / 'WndCompat/tests/vcl_background_rendering_tests.cpp'),
                '-o', str(binary),
            ], capture_output=True, text=True, timeout=180)
            if result.returncode:
                raise AssertionError(f'{mode} VCL background test build failed:\n{result.stdout}{result.stderr}')
            cls.binaries[mode] = binary

    def run_case(self, name):
        for mode, binary in self.binaries.items():
            with self.subTest(mode=mode):
                result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_erase_inside_beginpaint_is_observed_and_cleared_after_it(self):
        self.run_case('erase-inside')

    def test_erase_sent_before_beginpaint_is_asked_again_on_the_paint_dc(self):
        self.run_case('erase-before')

    def test_custom_pattern_double_buffered_and_absent_erase_stay_the_windows_own(self):
        self.run_case('own-background')

    def test_only_exact_vcl_list_classes_in_full_mode_are_asked(self):
        self.run_case('classes')

    def test_only_the_default_full_client_fill_on_the_returned_dc_is_replaced(self):
        self.run_case('gates')

    def test_fills_after_beginpaint_and_getdc_observation_are_unchanged(self):
        self.run_case('existing')

    def test_paints_from_inside_the_erase_handler_keep_sessions_apart(self):
        self.run_case('reentry')

    def test_import_references_are_balanced_and_shutdown_releases_them(self):
        self.run_case('lifecycle')

    def test_debug_line_reports_each_repeated_erase_within_a_budget(self):
        self.run_case('diagnostics')


if __name__ == '__main__':
    unittest.main()
