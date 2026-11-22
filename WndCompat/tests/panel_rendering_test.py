#!/usr/bin/env python3
"""Execute production VCL panel hooks/message bridge with Win32/IAT/Detours doubles.

The root themed-text gate and IAT handler are also compiled directly from
production source excerpts. The complete TPanelDraw unit is compiled in three
configurations, including its TPanel user32 export detour, exact TPage EXE-IAT
route and shared ANSI glyph decoding. Checks acquisition/ref/trampoline lifetime,
first-occupant and shared ANSI context through either entry, cached class admission,
DIB bytes, options and scoped state, not Windows NLS tables, pixels, executable Detours patches,
real IAT chains, uxtheme alpha, or MSVC build compatibility.
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
            'CorePch.h', 'WndPch.h', 'windows.h', 'uxtheme.h', 'wil.h', 'DebugLog.h',
            'GlassBackdropMark.h', 'InjectionMode.h', 'WndCompat.h',
            'Tracker/WinEventHandler.h', 'Core/Intercept/HookDispatcher.h',
            'Core/Intercept/HookHelper.h',
            'Module/ModuleHandler.h', 'Subsystems/PaintDispatcher.h',
            'Subsystems/RootSwcaGradient.h', 'Subsystems/RenderText.h',
            'Subsystems/OpaqueBitmapSource.h',
        ):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        # Compile the real root gate and handler without dragging the unrelated
        # MMC/Explorer hooks into the Win32 double. Do not duplicate their logic.
        root = (ROOT / 'GlassCore/src/Hooks/RootWindowHooks.cpp').read_text(encoding='utf-8')
        gate = root.split('// VCL paints windowless labels and group captions', 1)[1]
        gate = gate[gate.index('            if ('):].split('// Custom-painted VCL groups:', 1)[0]
        gdi = (ROOT / 'GlassCore/src/Hooks/RootGdiHooks.cpp').read_text(encoding='utf-8')
        handler = '        HRESULT WINAPI Root_MyDelphiDrawThemeTextEx(\n'
        handler += gdi.split(handler, 1)[1].split('        // Bome memo EXE thunks:', 1)[0]
        (stubs / 'root_panel_theme_test.inc').write_text(
            'namespace GlassCore::Root {\n'
            'inline RootHookCache PanelTextSpecsForTest(HMODULE instMod, LPCWSTR cls) {\n'
            'RootHookCache cache;\n' + gate + '\nreturn cache;\n}\n' + handler + '\n}\n',
            encoding='utf-8')
        cls.binaries = {}
        cls.build_args = compiler + [
            '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
            '-Wno-unknown-pragmas', '-pthread', '-I', str(stubs),
            '-I', str(ROOT / 'GlassCore/Include'), '-I', str(ROOT / 'WndCompat'),
            '-I', str(ROOT / 'GlassShared/include'),
        ]
        for mode, defines in (
            ('Release', ['-O2']), ('Debug', ['-O0', '-g', '-D_DEBUG']),
            ('DebugProbe', ['-O0', '-g', '-D_DEBUG', '-DGLASS_DELPHI_DRAW_PROBE']),
        ):
            binary = tmp / ('panel-tests-' + mode)
            result = subprocess.run(cls.build_args + defines + [
                str(ROOT / 'WndCompat/tests/panel_rendering_tests.cpp'), '-o', str(binary),
            ], capture_output=True, text=True, timeout=90)
            if result.returncode:
                raise AssertionError(f'Panel {mode} C++ test build failed:\n{result.stdout}{result.stderr}')
            cls.binaries[mode] = binary

    def run_case(self, name):
        for mode, binary in self.binaries.items():
            with self.subTest(mode=mode):
                result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_exact_classes_and_native_erase_reach_color_brush(self):
        self.run_case('classes-and-erase')

    def test_custom_alpha_100_system_black_and_nonaccumulating_repaints(self):
        self.run_case('alpha-colors')

    def test_tpanel_category_panels_and_frames_blacken_both_current_system_defaults(self):
        self.run_case('panel-default-scope')

    def test_native_brush_apis_and_preserved_chrome(self):
        self.run_case('apis-and-chrome')

    def test_dc_ownership_and_native_printclient(self):
        self.run_case('dc-and-print')

    def test_mode_backdrop_missing_bridge_and_balanced_sessions(self):
        self.run_case('gates-and-sessions')

    def test_gdi_failure_cleanup_and_reentry_passthrough(self):
        self.run_case('failures-and-reentry')

    def test_actual_root_class_gate_and_handler_keep_the_exe_original(self):
        self.run_case('root-themed-route')

    def test_themed_probe_captions_use_transparent_compositor_and_keep_dttopts(self):
        self.run_case('probe-themed-captions')

    def test_themed_dc_isolation_print_and_existing_vcl_containers(self):
        self.run_case('themed-dc-and-legacy')

    def test_themed_measurement_callback_failure_and_reentry_are_native(self):
        self.run_case('themed-native-semantics')

    def test_card_and_radio_group_use_probe_options_without_panel_background_changes(self):
        self.run_case('card-and-radio-captions')

    def test_ansi_export_route_needs_neither_exe_import_nor_iat_acquisition(self):
        self.run_case('ansi-route')

    def test_ansi_detour_is_exact_tpanel_paint_print_only(self):
        self.run_case('ansi-classes')

    def test_ansi_bounded_embedded_nul_dbcs_and_utf16_lengths(self):
        self.run_case('ansi-lengths')

    def test_ansi_codepage_tracks_selected_font_charset_not_global_mode(self):
        self.run_case('ansi-codepages')

    def test_ansi_window_tracked_memory_and_explicit_print_dc_ownership(self):
        self.run_case('ansi-dcs')

    def test_ansi_full_backdrop_window_shutdown_and_non_ansi_inner_context(self):
        self.run_case('ansi-gates')

    def test_ansi_native_measure_modify_invalid_geometry_and_mapping_semantics(self):
        self.run_case('ansi-native-args')

    def test_ansi_caption_preserves_tint_clipping_rect_and_entire_saved_dc(self):
        self.run_case('ansi-state')

    def test_ansi_translation_allocation_and_both_conversion_pass_failures(self):
        self.run_case('ansi-conversion-failures')

    def test_ansi_save_mode_renderer_exception_failure_and_zero_result(self):
        self.run_case('ansi-render-failures')

    def test_ansi_native_once_uses_real_detour_trampoline_and_detached_export(self):
        self.run_case('ansi-original')

    def test_ansi_lookup_logging_cleanup_and_native_last_error(self):
        self.run_case('ansi-last-error')

    def test_ansi_nested_ref_delta_and_ineligible_inner_session_do_not_leak(self):
        self.run_case('ansi-nested')

    def test_ansi_export_lease_leaves_unrelated_iat_first_occupant_untouched(self):
        self.run_case('ansi-imports')

    def test_ansi_detour_begin_attach_and_commit_failures_do_not_acquire_phantom_refs(self):
        self.run_case('ansi-detour-failures')

    def test_ansi_missing_user32_export_and_repeated_startup_do_not_attach_or_destroy_trampoline(self):
        self.run_case('ansi-detour-resolution')

    def test_ansi_detach_failure_retains_native_trampoline_and_shutdown_retries_once(self):
        self.run_case('ansi-detour-lifetime')

    def test_ansi_renderer_reentry_and_log_budget_do_not_suppress_calls(self):
        self.run_case('ansi-reentry-budget')

    def test_ansi_other_thread_cannot_use_the_owning_thread_panel_context(self):
        self.run_case('ansi-tls')


    def test_page_route_uses_only_exe_iat_for_both_native_paragraph_counts(self):
        self.run_case('page-route')

    def test_page_print_bridge_and_full_backdrop_shutdown_export_gates(self):
        self.run_case('page-print-gates')

    def test_page_class_and_message_gates_do_not_expand_background_or_themed_policy(self):
        self.run_case('page-classes')

    def test_page_missing_import_or_registration_failure_keeps_native_message_route(self):
        self.run_case('page-import-failures')

    def test_page_iat_does_not_need_panel_export_resolution_or_detours(self):
        self.run_case('page-export-independent')

    def test_page_text_requires_own_window_tracked_memory_or_explicit_print_dc(self):
        self.run_case('page-dcs')

    def test_page_ansi_bounds_font_codepages_dbcs_and_real_utf16_counts(self):
        self.run_case('page-ansi')

    def test_page_measurement_mutation_invalid_mapping_and_geometry_stay_native_once(self):
        self.run_case('page-native-args')

    def test_page_transparent_glyphs_restore_dc_clip_rect_and_entry_exit_error(self):
        self.run_case('page-state-error')

    def test_page_conversion_and_renderer_failures_restore_state_before_native_once(self):
        self.run_case('page-fallbacks')

    def test_page_nested_false_enable_refs_and_foreign_context_are_balanced(self):
        self.run_case('page-nested')

    def test_page_inner_session_cannot_use_outer_panel_background_or_themed_hooks(self):
        self.run_case('page-no-background')

    def test_page_iat_draws_nested_tpanel_without_forwarding_to_another_text_hook(self):
        self.run_case('page-nested-panel')

    def test_page_native_fallback_and_reentry_cross_panel_export_only_once(self):
        self.run_case('page-hybrid-fallback')

    def test_page_iat_respects_first_occupant_and_independent_peer_lifetime(self):
        self.run_case('page-occupants')

    def test_page_native_fallback_uses_registry_original_and_stale_handler_is_safe(self):
        self.run_case('page-original')

    def test_shared_ansi_native_fallback_blocks_reentry_from_iat_and_export_originals(self):
        self.run_case('page-original-reentry')

    def test_page_shutdown_releases_only_its_iat_refs_and_can_restart(self):
        self.run_case('page-shutdown')

    def test_page_reentry_and_metadata_log_budget_do_not_change_execution(self):
        self.run_case('page-reentry')

    def test_page_iat_cannot_use_a_different_threads_session(self):
        self.run_case('page-tls')

    def test_shared_ansi_context_does_not_require_a_lease_from_the_same_hook(self):
        self.run_case('ansi-shared-context')

    def test_shared_ansi_fallback_is_once_for_both_entries_without_rechecking_class(self):
        self.run_case('ansi-shared-fallback')

    def test_shared_ansi_missing_original_and_stale_iat_stay_native_under_export(self):
        self.run_case('ansi-shared-missing-original')


if __name__ == '__main__':
    unittest.main()
