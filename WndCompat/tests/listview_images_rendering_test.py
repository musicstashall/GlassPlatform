#!/usr/bin/env python3
"""Compile full production ListView/bitmap sources against environment doubles.

Check VCL native MaskBlt preparation, params, refs and isolation in Release and
Debug/probe. This is not a Windows GDI/DWM/PE-IAT/MSVC acceptance test.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ListViewImagesRenderingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='listview-images-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        stubs = tmp / 'include'
        for header in (
            'CorePch.h', 'SharedPch.h', 'windows.h', 'wil.h', 'wil/resource.h',
            'CommCtrl.h', 'uxtheme.h', 'WinApiUtils.h', 'DebugLog.h',
            'GlassControlKind.h', 'GlassBackdropMark.h',
            'Subsystems/RenderText.h', 'Subsystems/Settings.h',
            'Subsystems/GdiHookUtils.h', 'Subsystems/RootSwcaGradient.h',
            'Subsystems/TextGlowClientRegistry.h', 'Subsystems/PaintDispatcher.h',
            'Module/ModuleHandler.h', 'Core/Intercept/HookDispatcher.h',
            'Helpers/ThemeHelper.h', 'Hooks/ScrollBarTheme.h', 'Hooks/ScrollBarDrawTheme.h',
        ):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        cls.binaries = {}
        for mode, flags in (
            ('Release', ['-O2']),
            ('Debug', ['-O0', '-g', '-D_DEBUG', '-DGLASS_DELPHI_DRAW_PROBE']),
        ):
            binary = tmp / f'listview-images-{mode}'
            result = subprocess.run(compiler + [
                '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                '-Wno-unknown-pragmas', '-pthread', *flags,
                '-I', str(stubs), '-I', str(ROOT / 'GlassCore/Include'),
                '-I', str(ROOT / 'GlassShared/include'),
                '-I', str(ROOT / 'GlassShared/include/Helpers'),
                str(ROOT / 'WndCompat/tests/listview_images_rendering_tests.cpp'),
                '-o', str(binary),
            ], capture_output=True, text=True, timeout=120)
            if result.returncode:
                raise AssertionError(f'{mode} ListView image test build failed:\n{result.stdout}{result.stderr}')
            cls.binaries[mode] = binary

    def run_case(self, name):
        for mode, binary in self.binaries.items():
            with self.subTest(mode=mode):
                result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_exact_vcl_list_classes_lease_maskblt_on_actual_new_or_legacy_caller(self):
        self.run_case('routes')

    def test_no_exe_cross_comctl_substring_or_winforms_extension(self):
        self.run_case('scope')

    def test_mask_offsets_rop_rgb_and_native_return_are_preserved(self):
        self.run_case('native-semantics')

    def test_image_dib_ddb_and_garbage_alpha_are_opaque_without_chroma(self):
        self.run_case('source-types')

    def test_paint_owner_dc_mode_backdrop_class_and_scrollbar_isolation(self):
        self.run_case('dc-isolation')

    def test_only_registered_owned_memory_dc_and_current_thread_are_authorized(self):
        self.run_case('memory-and-thread')

    def test_nested_ref_delta_snapshot_release_and_mismatched_end(self):
        self.run_case('leases')

    def test_missing_import_failed_registration_and_unready_inner_session_stay_native(self):
        self.run_case('barriers')

    def test_zero_negative_alias_monochrome_and_client_sized_sources_are_native(self):
        self.run_case('native-fallbacks')

    def test_preparation_failures_and_native_false_draw_once_and_restore_error(self):
        self.run_case('failures')

    def test_reentry_and_session_stack_reallocation_preserve_source_and_guard(self):
        self.run_case('reentry')

    def test_unload_shutdown_and_failed_begin_compensation_release_captured_ref(self):
        self.run_case('lifecycle')

    def test_existing_syslist_winforms_text_background_and_alpha_blits_are_unchanged(self):
        self.run_case('existing-paths')


if __name__ == '__main__':
    unittest.main()
