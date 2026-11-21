#!/usr/bin/env python3
"""Compile the production memo drawing, atomic IAT acquisition and root providers.

Win32/IAT/codepage doubles check routing, arguments, state and ref balancing.
They do not model Windows rasterization, StretchBlt alpha or DWM acceptance.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def block(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 0
    for end in range(opening, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if depth == 0:
            return source[start:end + 1]
    raise AssertionError('Unclosed production block: ' + marker)


class RestoratorMemoRenderingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='restorator-memo-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        stubs = tmp / 'include'
        for header in ('CorePch.h', 'windows.h', 'wil.h', 'DebugLog.h', 'Module/ModuleHandler.h',
                       'Subsystems/PaintDispatcher.h', 'Subsystems/RenderText.h',
                       'Subsystems/RootSwcaGradient.h', 'GlassBackdropMark.h'):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        root = (ROOT / 'GlassCore/src/Hooks/RootWindowHooks.cpp').read_text(encoding='utf-8')
        gdi = (ROOT / 'GlassCore/src/Hooks/RootGdiHooks.cpp').read_text(encoding='utf-8')
        rows = gdi.split('// Restorator memo route:', 1)[1]
        rows = rows[rows.index('            std::tuple{'):].split('#if defined(_DEBUG)', 1)[0]
        thunks = gdi.split('// Restorator memo EXE thunks:', 1)[1]
        thunks = thunks[thunks.index('        int WINAPI Root_MyRestoratorMemoText('):].split('// Dedicated EXE slots:', 1)[0]
        declarations = '\n'.join(line for line in gdi.splitlines()
                                 if ' WINAPI Root_MyRestoratorMemo' in line and line.endswith(';'))
        data = root[root.index('        struct RootHookCache'):root.index('        // ── Утилиты')]
        memo_classification = root.split('            cache.restoratorMemo =', 1)[1].split(';', 1)[0]
        generic = block(root.split('// Точка входа процесса —', 1)[1],
                        '            if (instMod == GlassCore::ModuleHandler::g_entryPointModule)')
        root_stubs = '''
        inline bool IsRarextWindowModule(HWND) { return false; }
        inline void EnsureRarextHookCache(HWND) {}
        inline bool IsMmcChartSessionClass(LPCWSTR) { return false; }
        inline void RequestRootAttachForHwnd(HWND) {}
        struct RootCacheLookupResult { RootHookCache* cache{}; bool inheritedFromChartGraph{}; };
        inline RootCacheLookupResult LookupRootHookCacheLocked(HWND hwnd) {
            const auto it = s_rootCache.find(hwnd);
            return {it == s_rootCache.end() ? nullptr : &it->second, false};
        }
        '''
        text = ('namespace GlassCore::Root {\nusing namespace std::literals;\n'
                'inline std::atomic<bool> g_shutdown{false};\n' + declarations + '\n'
                'inline const GlassCore::Hooks::HookDispatcherDependency<4,3> MemoDependencies{{\n' + rows + '\n}};\n'
                'inline const GlassCore::Hooks::HookDispatcherDependency<OHK_COUNT,3> g_rootHookDep = [] {\n'
                'GlassCore::Hooks::HookDispatcherDependency<OHK_COUNT,3> result{};\n'
                'for (size_t i=0;i<4;++i) result[OHK_RESTORATOR_MEMO_TEXT+i]=MemoDependencies[i];\n'
                'return result; }();\n'
                'using RootDispatcher = MemoTest::Dispatcher<OHK_COUNT,3>;\n'
                'inline wil::srwlock g_rootLock;\n'
                'inline std::unordered_map<void*,RootDispatcher> g_rootMap;\n'
                + gdi[gdi.index('        template <size_t idx, typename T>'):].split('#if defined(_DEBUG)', 1)[0]
                + block(gdi, '        bool EnableRootHook(') + '\n'
                + block(gdi, '        void PrewarmRootHook(') + '\n'
                + gdi[gdi.index('        static constexpr size_t kRestoratorMemoSlots[]'):gdi.index('        void DisableAllRootHooks(')]
                + block(gdi, '        void DisableAllRootHooks(') + '\n' + thunks + '\n' + data + '\n'
                + block(root, '        static bool StrEqI(') + '\n'
                + block(root, '        [[nodiscard]] static std::wstring BasenamePtr(') + '\n'
                + block(root, '        static void PushPaint(') + '\n'
                + block(root, '        static void PushDc(') + '\n' + root_stubs
                + 'inline RootHookCache MemoSpecsForTest(HWND hWnd, HMODULE instMod) {\n'
                'RootHookCache cache; wchar_t cls[256]{}; GetClassNameW(hWnd,cls,_countof(cls));\n'
                'cache.restoratorMemo =' + memo_classification + ';\n' + generic + '\nreturn cache; }\n'
                + block(root, '        static bool Root_OnRestoratorMemoAfterBeginPaint(') + '\n'
                + block(root, '        static void EnableSpecs(') + '\n'
                + '\n'.join(block(root, '        static void ' + fn + '(') for fn in (
                    'Root_OnBeginPaint', 'Root_OnEndPaint', 'Root_OnGetDC', 'Root_OnReleaseDC'))
                + '\n}\n')
        (stubs / 'root_restorator_memo_test.inc').write_text(text, encoding='utf-8')
        cls.binaries = []
        for config, flags in (('release', ['-O2']), ('debug', ['-O0', '-g', '-D_DEBUG', '-DGLASS_DELPHI_DRAW_PROBE'])):
            binary = tmp / ('memo-tests-' + config)
            result = subprocess.run(compiler + [
                '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                '-Wno-unknown-pragmas', '-pthread', *flags, '-I', str(stubs),
                '-I', str(ROOT / 'GlassCore/Include'), '-I', str(ROOT / 'GlassShared/include'),
                str(ROOT / 'WndCompat/tests/restorator_memo_rendering_tests.cpp'), '-o', str(binary),
            ], capture_output=True, text=True, timeout=90)
            if result.returncode:
                raise AssertionError(f'{config} memo C++ test build failed:\n{result.stdout}{result.stderr}')
            cls.binaries.append((config, binary))

    def run_case(self, name):
        for config, binary in self.binaries:
            with self.subTest(config=config):
                result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_scope_exit_double_rejects_reference_and_non_void_callbacks(self):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        source = Path(self.tmp.name) / 'invalid-scope-exit.cpp'
        cases = (
            ('const auto restoreNow = [] {}; auto guard = wil::scope_exit(restoreNow);',
             'scope_exit should only be directly used with a lambda'),
            ('auto guard = wil::scope_exit([] { return 1; });',
             'scope_exit lambdas must not have a return value'),
        )
        for body, diagnostic in cases:
            with self.subTest(diagnostic=diagnostic):
                source.write_text('#include "coolbar_test_shim.h"\nvoid Invalid() { ' + body + ' }\n', encoding='utf-8')
                result = subprocess.run(compiler + [
                    '-std=c++20', '-fsyntax-only', '-I', str(ROOT / 'WndCompat/tests'), str(source),
                ], capture_output=True, text=True, timeout=30)
                self.assertNotEqual(result.returncode, 0, 'The WIL double accepted an invalid callback')
                self.assertIn(diagnostic, result.stderr)

    def test_exact_classes_process_and_no_generic_half_route(self): self.run_case('classes')
    def test_each_missing_or_conflicting_import_rolls_back_only_acquired_refs(self): self.run_case('transaction')
    def test_real_paint_provider_post_erase_then_ansi_memory_dc_path(self): self.run_case('paint')
    def test_real_getdc_provider_and_non_exe_caller_native_barrier(self): self.run_case('getdc')
    def test_observed_twenty_ansi_lines_flags_font_rect_and_no_bpp_gate(self): self.run_case('text')
    def test_dbcs_cyrillic_explicit_bytes_and_minus_one_use_unicode_count(self): self.run_case('conversion')
    def test_measurement_mutation_invalid_and_unknown_conversion_remain_native(self): self.run_case('native')
    def test_system_colors_brush_ownership_cached_fills_custom_black_patterns(self): self.run_case('backgrounds')
    def test_opaque_custom_a255_and_transparent_text_background(self): self.run_case('opaque')
    def test_dc_ownership_top_session_full_and_backdrop_isolation(self): self.run_case('isolation')
    def test_failures_restore_original_dc_before_native_and_reentry_is_native(self): self.run_case('failures')
    def test_nested_out_of_order_end_and_failed_beginpaint_compensation(self): self.run_case('nesting')
    def test_cross_thread_native_shutdown_and_exact_original_thunks(self): self.run_case('lifecycle')


if __name__ == '__main__':
    unittest.main()
