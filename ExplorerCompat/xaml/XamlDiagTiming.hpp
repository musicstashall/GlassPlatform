#pragma once

#include <Windows.h>

// Диагностика плана XAML: интервалы SetSite → AdviseVisualTreeChange → первый OnVisualTreeChange.
void GlassExplorerXamlDiag_BeginAdviseSequence(DWORD setSiteOrCallerTid) noexcept;
void GlassExplorerXamlDiag_OnAdviseCompleted(DWORD threadThatCalledAdvise) noexcept;
void GlassExplorerXamlDiag_OnVisualTreeCallback(DWORD vtcTid) noexcept;
