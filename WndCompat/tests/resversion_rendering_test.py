#!/usr/bin/env python3
"""Run the complete production TResVersionEditorDraw unit with Win32 doubles.

Release and Debug/probe configurations exercise what reaches the surface for
the EXE's ExtTextOutA and FillRect while it paints the control (background,
glyph string, position, clipping, cleared fills), the class/session/DC gates
and the lease of the two imports.
This is NOT Windows GDI/DWM rasterization, the shared text renderer, the real
PE import table, the VCL or a Windows/MSVC build acceptance.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ResVersionEditorRenderingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='resversion-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        stubs = tmp / 'include'
        for header in (
            'CorePch.h', 'windows.h', 'wil.h', 'DebugLog.h', 'GlassBackdropMark.h', 'WinApiUtils.h',
            'Subsystems/PaintDispatcher.h', 'Subsystems/RenderText.h', 'Subsystems/RootSwcaGradient.h',
            'Module/ModuleHandler.h', 'Core/Intercept/HookDispatcher.h',
        ):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        cls.binaries = {}
        for mode, flags in (
            ('Release', ['-O2']),
            ('Debug', ['-O0', '-g', '-D_DEBUG', '-DGLASS_DELPHI_DRAW_PROBE']),
        ):
            binary = tmp / f'resversion-{mode}'
            result = subprocess.run(compiler + [
                '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                '-Wno-unknown-pragmas', '-pthread', *flags,
                '-I', str(stubs), '-I', str(ROOT / 'GlassCore/Include'),
                str(ROOT / 'WndCompat/tests/resversion_rendering_tests.cpp'),
                '-o', str(binary),
            ], capture_output=True, text=True, timeout=180)
            if result.returncode:
                raise AssertionError(f'{mode} TResVersionEditor test build failed:\n{result.stdout}{result.stderr}')
            cls.binaries[mode] = binary

    def run_case(self, name):
        for mode, binary in self.binaries.items():
            with self.subTest(mode=mode):
                result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_cell_fill_is_cleared_and_text_is_drawn_at_the_callers_position(self):
        self.run_case('cells')

    def test_only_solid_default_window_and_button_face_fills_are_cleared(self):
        self.run_case('fills')

    def test_options_alignment_spacing_conversion_and_failures_are_one_native_call(self):
        self.run_case('text-gates')

    def test_ansi_bytes_are_decoded_with_the_code_page_of_the_fonts_charset(self):
        self.run_case('ansi')

    def test_exact_class_exe_caller_instance_mode_and_newest_session_only(self):
        self.run_case('scope')

    def test_other_dc_session_mapping_thread_and_reentry_are_native(self):
        self.run_case('isolation')

    def test_two_imports_are_leased_together_and_survive_shutdown_and_restart(self):
        self.run_case('leases')

    def test_metadata_only_rate_limited_reasons(self):
        self.run_case('diagnostics')


if __name__ == '__main__':
    unittest.main()
