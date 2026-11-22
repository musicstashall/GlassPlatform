#!/usr/bin/env python3
"""Run the complete production RestoratorImages unit with Win32 doubles.

Optimized and Debug/probe configurations exercise the pixel policy byte by
byte, the class/session/caller/DC gates, the two-function Detours lease and
resource ownership. This is NOT Windows GDI/DWM rasterization, real export
patching, the Detours ABI or a Windows/MSVC build acceptance.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RestoratorImagesRenderingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='restorator-images-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        stubs = tmp / 'include'
        for header in (
            'CorePch.h', 'SharedPch.h', 'windows.h', 'wil.h', 'wil/resource.h',
            'CommCtrl.h', 'DebugLog.h', 'GlassBackdropMark.h',
            'Subsystems/PaintDispatcher.h', 'Subsystems/RootSwcaGradient.h',
            'Module/ModuleHandler.h', 'Core/Intercept/HookHelper.h',
        ):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        cls.binaries = {}
        for mode, flags in (
            ('Release', ['-O2']),
            ('Debug', ['-O0', '-g', '-D_DEBUG', '-DGLASS_DELPHI_DRAW_PROBE']),
        ):
            binary = tmp / f'restorator-images-{mode}'
            result = subprocess.run(compiler + [
                '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                '-Wno-unknown-pragmas', '-pthread', *flags,
                '-I', str(stubs), '-I', str(ROOT / 'GlassCore/Include'),
                '-I', str(ROOT / 'GlassShared/include'),
                '-I', str(ROOT / 'GlassShared/include/Helpers'),
                str(ROOT / 'WndCompat/tests/restorator_images_rendering_tests.cpp'),
                '-o', str(binary),
            ], capture_output=True, text=True, timeout=180)
            if result.returncode:
                raise AssertionError(f'{mode} Restorator image test build failed:\n{result.stdout}{result.stderr}')
            cls.binaries[mode] = binary

    def run_case(self, name):
        for mode, binary in self.binaries.items():
            with self.subTest(mode=mode):
                result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_placeholder_alpha_is_the_brightness_of_every_pixel_and_nothing_dark_stays_opaque(self):
        self.run_case('policy-placeholder')

    def test_every_level_and_channel_maps_to_its_own_alpha_whatever_the_neighbours(self):
        self.run_case('policy-levels')

    def test_thick_dark_blocks_and_enclosed_black_are_masked_like_any_other_pixel(self):
        self.run_case('policy-mask')

    def test_pictures_without_a_three_quarter_black_outline_are_not_touched(self):
        self.run_case('policy-outline')

    def test_tree_and_viewer_copy_and_stretch_keep_the_callers_geometry(self):
        self.run_case('routes')

    def test_exact_class_exe_caller_instance_mode_and_empty_tree_only(self):
        self.run_case('scope')

    def test_other_dc_rop_caller_thread_session_and_reentry_are_native(self):
        self.run_case('isolation')

    def test_depths_existing_alpha_size_and_gdi_failures(self):
        self.run_case('sources')

    def test_two_detours_are_leased_in_one_transaction_and_survive_failures(self):
        self.run_case('leases')

    def test_native_false_and_last_error_are_returned_once(self):
        self.run_case('failures')

    def test_metadata_only_rate_limited_reasons(self):
        self.run_case('diagnostics')

    def test_client_buffer_and_already_prepared_alpha(self):
        self.run_case('buffered')


if __name__ == '__main__':
    unittest.main()
