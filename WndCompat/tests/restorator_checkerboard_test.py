#!/usr/bin/env python3
"""Run the production GlassCore RestoratorCheckerboard unit with Win32 doubles.

Both configurations exercise what Glass does where the EXE composes a picture:
the solid fill of the window's colour over the whole of the last DIB section
it asked for, in a memory DC, becomes a checkerboard made by the same call
with a pattern brush - byte by byte - while every other fill and every DIB
section stay the EXE's own; the gates, the two imports taken together by the
first viewer that paints on glass and held until Shutdown, the window that
answers for the glass, every failure and the Debug lines.
This is NOT Windows: no GDI (the fill is a model of a solid and of a DIB
pattern brush), no VCL, no PNG decoder, no real import table or IAT registry,
no threads, and it is not an MSVC build acceptance.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RestoratorCheckerboardTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='restorator-checkerboard-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        stubs = tmp / 'include'
        for header in (
            'CorePch.h', 'windows.h', 'DebugLog.h', 'wil.h', 'GlassBackdropMark.h',
            'Core/Intercept/HookDispatcher.h', 'Module/ModuleHandler.h',
            'Subsystems/RootSwcaGradient.h',
        ):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        cls.binaries = {}
        for mode, flags in (
            ('Release', ['-O2']),
            ('Debug', ['-O0', '-g', '-D_DEBUG']),
        ):
            binary = tmp / f'restorator-checkerboard-{mode}'
            result = subprocess.run(compiler + [
                '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                '-Wno-unknown-pragmas', *flags,
                '-I', str(stubs), '-I', str(ROOT / 'GlassCore/Include'),
                str(ROOT / 'WndCompat/tests/restorator_checkerboard_tests.cpp'),
                '-o', str(binary),
            ], capture_output=True, text=True, timeout=180)
            if result.returncode:
                raise AssertionError(
                    f'{mode} checkerboard test build failed:\n{result.stdout}{result.stderr}')
            cls.binaries[mode] = binary

    def run_case(self, name):
        for mode, binary in self.binaries.items():
            with self.subTest(mode=mode):
                result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_window_colour_over_the_whole_last_section_becomes_the_checkerboard(self):
        self.run_case('board')

    def test_every_other_fill_of_the_exe_is_its_own_single_call(self):
        self.run_case('gates')

    def test_every_dib_section_is_passed_on_and_only_the_last_size_is_kept(self):
        self.run_case('sizes')

    def test_a_failed_brush_or_fill_leaves_the_exes_own_fill_once(self):
        self.run_case('failures')

    def test_both_imports_are_taken_together_once_and_held_until_shutdown(self):
        self.run_case('arming')

    def test_the_window_of_the_last_viewer_answers_for_the_glass(self):
        self.run_case('glass')

    def test_nested_calls_a_closed_gate_and_a_stale_detour_stay_native(self):
        self.run_case('isolation')

    def test_debug_lines_say_what_was_replaced_and_why_not_within_a_budget(self):
        self.run_case('diagnostics')


if __name__ == '__main__':
    unittest.main()
