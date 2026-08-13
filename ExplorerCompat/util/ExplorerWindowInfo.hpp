#pragma once
#include <windows.h>
#include <string>

struct ExplorerWindowInfo
{
    HWND Hwnd = nullptr;
    HWND XamlIsland = nullptr;
    RECT ClientRect = { 0 };
    bool IsWin11 = false;
    bool HasTabs = false;

    ExplorerWindowInfo() = default;
    explicit ExplorerWindowInfo(HWND hwnd);

    // Проверка, является ли окно Explorer
    bool IsExplorerWindow() const;

    // Обновление состояния
    void Refresh();

    // Состояния окна
    bool IsActive() const;
    bool IsMaximized() const;

private:
    // Внутренние методы
    void DetectXamlIsland();
    void DetectWin11Features();
};
