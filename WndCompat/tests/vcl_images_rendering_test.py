#!/usr/bin/env python3
"""Run complete production Toolbar/panel/opaque-source code with Win32 doubles.

Both optimized and Debug/probe configurations exercise bytes, native parameters,
thread/session/DC gates and resource ownership. This is NOT Windows GDI/DWM
rasterization, a real PE/IAT test or a Windows/MSVC build acceptance.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class VclImagesRenderingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='vcl-images-')
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
            'Core/Intercept/HookHelper.h',
        ):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        cls.binaries = {}
        for mode, flags in (
            ('Release', ['-O2']),
            ('Debug', ['-O0', '-g', '-D_DEBUG', '-DGLASS_DELPHI_DRAW_PROBE']),
        ):
            binary = tmp / f'vcl-images-{mode}'
            result = subprocess.run(compiler + [
                '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                '-Wno-unknown-pragmas', '-pthread', *flags,
                '-I', str(stubs), '-I', str(ROOT / 'GlassCore/Include'),
                '-I', str(ROOT / 'GlassShared/include'),
                '-I', str(ROOT / 'GlassShared/include/Helpers'),
                str(ROOT / 'WndCompat/tests/vcl_images_rendering_tests.cpp'),
                '-o', str(binary),
            ], capture_output=True, text=True, timeout=120)
            if result.returncode:
                raise AssertionError(f'{mode} VCL image test build failed:\n{result.stdout}{result.stderr}')
            cls.binaries[mode] = binary

    def run_case(self, name):
        for mode, binary in self.binaries.items():
            with self.subTest(mode=mode):
                result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_production_copy_normalizes_8_16_24_32_bpp_dib_and_ddb_without_color_key(self):
        self.run_case('utility-types')

    def test_selected_reads_masks_oversized_metadata_and_gdi_failures_are_safe(self):
        self.run_case('utility-failures')

    def test_ttoolbar_exe_and_comctl_begin_routes_lease_both_known_comctl_modules(self):
        self.run_case('toolbar-routes')

    def test_masks_offsets_rop_and_black_pixels_remain_native_modern_toolbar_unchanged(self):
        self.run_case('toolbar-native-semantics')

    def test_toolbar_foreign_thread_dc_module_mode_and_session_are_native(self):
        self.run_case('toolbar-isolation')

    def test_toolbar_nested_false_enable_missing_slot_registration_failure_and_snapshot_release(self):
        self.run_case('toolbar-leases')

    def test_toolbar_unready_vcl_begin_is_a_native_barrier_over_outer_iat_refs(self):
        self.run_case('toolbar-barriers')

    def test_toolbar_native_false_last_error_cleanup_and_reentry(self):
        self.run_case('toolbar-failures')

    def test_panel_image_uses_actual_exe_stretch_with_scaled_and_mirrored_extents(self):
        self.run_case('panel-images')

    def test_panel_image_route_is_exact_nested_tpanel_paint_only(self):
        self.run_case('panel-scope')

    def test_panel_dc_isolation_and_partial_full_client_buffer_passthrough(self):
        self.run_case('panel-isolation')

    def test_panel_refs_missing_import_registration_failure_native_barrier_and_shutdown(self):
        self.run_case('panel-leases')

    def test_panel_native_failure_error_cleanup_and_reentry(self):
        self.run_case('panel-failures')

    def test_zero_extents_self_copies_and_mapped_or_mask_source_stay_native(self):
        self.run_case('native-extents')


if __name__ == '__main__':
    unittest.main()
