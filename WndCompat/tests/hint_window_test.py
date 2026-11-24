#!/usr/bin/env python3
"""Run the production GlassCore THintWindowDraw unit and the WndCompat bridge
that calls it (Popup/Tooltip/DelphiHintPaint.h) with Win32 doubles.

Both configurations exercise what Glass does with a VCL hint window (exact
THintWindow): the erase and the frame answered with the transparent base, the
caption VCL draws through the EXE's DrawText composed with alpha over a client
cleared once per paint (wide and ANSI), the answer that tooltips are not
animated, the gates, the imports held from the first hint window until
Shutdown, every refusal, the bridge and the Debug lines.
This is NOT Windows: no GDI, no DWM, no VCL, no AnimateWindow, no real import
table or subclass chain, and it is not an MSVC build acceptance.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class HintWindowTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='hint-window-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        stubs = tmp / 'include'
        for header in (
            'CorePch.h', 'windows.h', 'DebugLog.h', 'wil.h', 'WndCompat.h',
            'Core/Intercept/HookDispatcher.h', 'Module/ModuleHandler.h',
            'Subsystems/RenderText.h', 'Subsystems/Settings.h', 'Subsystems/TooltipContext.h',
        ):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        cls.binaries = {}
        for mode, flags in (
            ('Release', ['-O2']),
            ('Debug', ['-O0', '-g', '-D_DEBUG']),
        ):
            binary = tmp / f'hint-window-{mode}'
            result = subprocess.run(compiler + [
                '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                '-Wno-unknown-pragmas', *flags,
                '-I', str(stubs), '-I', str(ROOT / 'GlassCore/Include'),
                str(ROOT / 'WndCompat/tests/hint_window_tests.cpp'),
                '-o', str(binary),
            ], capture_output=True, text=True, timeout=180)
            if result.returncode:
                raise AssertionError(f'{mode} hint window test build failed:\n{result.stdout}{result.stderr}')
            cls.binaries[mode] = binary

    def run_case(self, name):
        for mode, binary in self.binaries.items():
            with self.subTest(mode=mode):
                result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_shown_hint_is_erased_framed_and_captioned_on_the_transparent_base(self):
        self.run_case('shown')

    def test_ansi_caption_is_converted_with_the_code_page_of_the_fonts_charset(self):
        self.run_case('ansi')

    def test_only_an_exe_hint_window_its_own_dc_and_its_caption_are_taken(self):
        self.run_case('gates')

    def test_frame_is_cleared_around_the_client_or_left_to_vcl(self):
        self.run_case('frame')

    def test_exe_is_told_that_tooltips_are_not_animated_while_it_has_such_a_hint(self):
        self.run_case('animation')

    def test_imports_are_taken_once_and_held_until_shutdown(self):
        self.run_case('lifecycle')

    def test_whatever_refuses_the_caption_and_the_erase_are_vcls_own(self):
        self.run_case('failures')

    def test_bridge_is_optional_paired_and_answers_the_erase_and_the_frame(self):
        self.run_case('bridge')

    def test_debug_build_writes_two_kinds_of_lines_and_a_release_build_none(self):
        self.run_case('diagnostics')


if __name__ == '__main__':
    unittest.main()
