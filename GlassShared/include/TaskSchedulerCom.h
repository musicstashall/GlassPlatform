#pragma once

#include <wil/com.h>

#include <Windows.h>

#include <string>

#include <taskschd.h>

namespace GlassShared
{
    /// Session COM for Schedule.Service (task planner COM API).
    class TaskSchedulerCom
    {
    public:
        TaskSchedulerCom() noexcept = default;
        ~TaskSchedulerCom() noexcept { Disconnect(); }

        TaskSchedulerCom(const TaskSchedulerCom&) = delete;
        TaskSchedulerCom& operator=(const TaskSchedulerCom&) = delete;
        TaskSchedulerCom(TaskSchedulerCom&&) = delete;
        TaskSchedulerCom& operator=(TaskSchedulerCom&&) = delete;

        [[nodiscard]] bool Connect() noexcept;

        void Disconnect() noexcept;

        [[nodiscard]] bool IsConnected() const noexcept { return m_service != nullptr; }

        [[nodiscard]] ITaskService* Service() const noexcept { return m_service.get(); }

        [[nodiscard]] bool GetRegisteredTask(
            const wchar_t* folderPath, const wchar_t* taskName,
            wil::com_ptr<IRegisteredTask>& outTask) noexcept;

        [[nodiscard]] bool TaskExists(const wchar_t* folderPath, const wchar_t* taskName) noexcept;

        [[nodiscard]] bool FindTaskByNameRecursive(
            const wchar_t* taskName,
            wil::com_ptr<IRegisteredTask>& outTask,
            std::wstring* outFullPath) noexcept;

        [[nodiscard]] static bool RunTask(IRegisteredTask* task) noexcept;

        [[nodiscard]] static bool StopTask(IRegisteredTask* task) noexcept;

        [[nodiscard]] static bool GetTaskSchedulerEnabled(IRegisteredTask* task, bool* enabledOut) noexcept;

        [[nodiscard]] bool DeleteTaskByFullPath(const wchar_t* fullTaskPath) noexcept;

        [[nodiscard]] bool DeleteTaskInRootFolder(const wchar_t* taskName) noexcept;

    private:
        wil::com_ptr<ITaskService> m_service{};

        [[nodiscard]] bool EnsureConnectedRootFolder(wil::com_ptr<ITaskFolder>& rootOut) noexcept;

        [[nodiscard]] static bool FindInFolder(
            ITaskFolder* folder,
            const wchar_t* targetName,
            wil::com_ptr<IRegisteredTask>& outTask,
            std::wstring* outFullPath) noexcept;
    };

    [[nodiscard]] bool RunSchtasksOnTask(const wchar_t* fullTaskPathOrName, const wchar_t* verbArgs) noexcept;

}
