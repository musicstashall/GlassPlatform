#include "SharedPch.h"

#include <framework.h>
#include <TaskSchedulerCom.h>

#include <oleauto.h>

#include <atomic>
#include <vector>

#pragma comment(lib, "oleaut32.lib")

namespace GlassShared
{
    namespace
    {
        std::atomic<DWORD> g_schedulerUiThread{ 0 };
    }

    void NoteSchedulerUiThread() noexcept
    {
        g_schedulerUiThread.store(GetCurrentThreadId(), std::memory_order_release);
    }

    bool IsSchedulerUiThread() noexcept
    {
        const DWORD id = g_schedulerUiThread.load(std::memory_order_acquire);
        return id != 0 && id == GetCurrentThreadId();
    }

    namespace
    {
        struct BstrWrap
        {
            BSTR b = nullptr;
            explicit BstrWrap(const wchar_t* s) noexcept : b(::SysAllocString(s)) {}
            ~BstrWrap() noexcept
            {
                if (b)
                    ::SysFreeString(b);
            }
            explicit operator bool() const noexcept { return b != nullptr; }
        };
    }

    bool TaskSchedulerCom::FindInFolder(
        ITaskFolder* folder,
        const wchar_t* targetName,
        wil::com_ptr<IRegisteredTask>& outTask,
        std::wstring* outFullPath) noexcept
    {
        if (!folder || !targetName)
            return false;
        outTask.reset();

        IRegisteredTaskCollection* pTasks = nullptr;
        if (FAILED(folder->GetTasks(TASK_ENUM_HIDDEN, &pTasks)) || !pTasks)
            return false;
        wil::com_ptr<IRegisteredTaskCollection> tasks;
        tasks.attach(pTasks);

        LONG count = 0;
        (void)tasks->get_Count(&count);
        for (LONG i = 1; i <= count; ++i)
        {
            VARIANT vi{};
            vi.vt = VT_I4;
            vi.lVal = i;
            IRegisteredTask* pt = nullptr;
            if (FAILED(tasks->get_Item(vi, &pt)) || !pt)
                continue;
            wil::com_ptr<IRegisteredTask> t;
            t.attach(pt);

            BSTR name = nullptr;
            bool match = false;
            if (SUCCEEDED(t->get_Name(&name)) && name)
            {
                match = (_wcsicmp(name, targetName) == 0);
                ::SysFreeString(name);
            }
            if (match)
            {
                if (outFullPath)
                {
                    outFullPath->clear();
                    BSTR p = nullptr;
                    if (SUCCEEDED(t->get_Path(&p)) && p)
                    {
                        *outFullPath = p;
                        ::SysFreeString(p);
                    }
                }
                outTask = std::move(t);
                return true;
            }
        }

        ITaskFolderCollection* pSubs = nullptr;
        if (FAILED(folder->GetFolders(0, &pSubs)) || !pSubs)
            return false;
        wil::com_ptr<ITaskFolderCollection> subfolders;
        subfolders.attach(pSubs);

        LONG fc = 0;
        (void)subfolders->get_Count(&fc);
        for (LONG i = 1; i <= fc; ++i)
        {
            VARIANT vi{};
            vi.vt = VT_I4;
            vi.lVal = i;
            ITaskFolder* ps = nullptr;
            if (FAILED(subfolders->get_Item(vi, &ps)) || !ps)
                continue;
            wil::com_ptr<ITaskFolder> sub;
            sub.attach(ps);
            if (FindInFolder(sub.get(), targetName, outTask, outFullPath))
                return true;
        }
        return false;
    }

    bool TaskSchedulerCom::Connect() noexcept
    {
        Disconnect();

        ITaskService* pSvc = nullptr;
        HRESULT hr = ::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITaskService, reinterpret_cast<void**>(&pSvc));
        if (FAILED(hr) || !pSvc)
        {
            Disconnect();
            return false;
        }
        m_service.attach(pSvc);

        VARIANT empty{};
        VariantInit(&empty);
        hr = m_service->Connect(empty, empty, empty, empty);
        if (FAILED(hr))
        {
            Disconnect();
            return false;
        }

        return true;
    }

    void TaskSchedulerCom::Disconnect() noexcept
    {
        m_service.reset();
    }

    bool TaskSchedulerCom::EnsureConnectedRootFolder(wil::com_ptr<ITaskFolder>& rootOut) noexcept
    {
        rootOut.reset();
        if (!m_service)
            return false;
        BstrWrap rootPath(L"\\");
        if (!rootPath)
            return false;
        ITaskFolder* raw = nullptr;
        HRESULT hr = m_service->GetFolder(rootPath.b, &raw);
        if (FAILED(hr) || !raw)
            return false;
        rootOut.attach(raw);
        return true;
    }

    bool TaskSchedulerCom::GetRegisteredTask(
        const wchar_t* folderPath, const wchar_t* taskName, wil::com_ptr<IRegisteredTask>& outTask) noexcept
    {
        outTask.reset();
        if (!folderPath || !taskName || !m_service)
            return false;

        BstrWrap fp(folderPath);
        BstrWrap tn(taskName);
        if (!fp || !tn)
            return false;

        ITaskFolder* pFolder = nullptr;
        HRESULT hr = m_service->GetFolder(fp.b, &pFolder);
        if (FAILED(hr) || !pFolder)
            return false;
        wil::com_ptr<ITaskFolder> folder;
        folder.attach(pFolder);

        IRegisteredTask* pTask = nullptr;
        hr = folder->GetTask(tn.b, &pTask);
        if (FAILED(hr) || !pTask)
            return false;
        outTask.attach(pTask);
        return true;
    }

    bool TaskSchedulerCom::TaskExists(const wchar_t* folderPath, const wchar_t* taskName) noexcept
    {
        wil::com_ptr<IRegisteredTask> t;
        return GetRegisteredTask(folderPath, taskName, t);
    }

    bool TaskSchedulerCom::FindTaskByNameRecursive(
        const wchar_t* taskName,
        wil::com_ptr<IRegisteredTask>& outTask,
        std::wstring* outFullPath) noexcept
    {
        outTask.reset();
        // Обход всех папок планировщика с UI не отдаёт сообщения и держит прокрутку.
        if (IsSchedulerUiThread())
            return false;
        if (!taskName || !m_service)
            return false;
        if (outFullPath)
            outFullPath->clear();

        wil::com_ptr<ITaskFolder> root;
        if (!EnsureConnectedRootFolder(root))
            return false;

        return FindInFolder(root.get(), taskName, outTask, outFullPath) && outTask != nullptr;
    }

    bool TaskSchedulerCom::RunTask(IRegisteredTask* task) noexcept
    {
        if (!task)
            return false;
        VARIANT empty{};
        VariantInit(&empty);
        IRunningTask* rt = nullptr;
        const HRESULT hr = task->Run(empty, &rt);
        if (rt)
            rt->Release();
        return SUCCEEDED(hr);
    }

    bool TaskSchedulerCom::StopTask(IRegisteredTask* task) noexcept
    {
        if (!task)
            return false;
        return SUCCEEDED(task->Stop(0));
    }

    bool TaskSchedulerCom::GetTaskSchedulerEnabled(IRegisteredTask* task, bool* enabledOut) noexcept
    {
        if (!task || !enabledOut)
            return false;
        *enabledOut = false;
        VARIANT_BOOL vb = VARIANT_FALSE;
        ITaskDefinition* def = nullptr;
        if (FAILED(task->get_Definition(&def)) || !def)
            return false;
        wil::com_ptr<ITaskDefinition> defPtr;
        defPtr.attach(def);
        ITaskSettings* settings = nullptr;
        if (SUCCEEDED(defPtr->get_Settings(&settings)) && settings)
        {
            wil::com_ptr<ITaskSettings> st;
            st.attach(settings);
            (void)st->get_Enabled(&vb);
        }
        *enabledOut = (vb == VARIANT_TRUE);
        return true;
    }

    bool TaskSchedulerCom::DeleteTaskByFullPath(const wchar_t* fullTaskPath) noexcept
    {
        if (!fullTaskPath || !*fullTaskPath || !m_service)
            return false;

        wil::com_ptr<ITaskFolder> root;
        if (!EnsureConnectedRootFolder(root))
            return false;

        BstrWrap delName(fullTaskPath);
        if (!delName)
            return false;

        return SUCCEEDED(root->DeleteTask(delName.b, 0));
    }

    bool TaskSchedulerCom::DeleteTaskInRootFolder(const wchar_t* taskName) noexcept
    {
        if (!taskName || !m_service)
            return false;

        wil::com_ptr<ITaskFolder> root;
        if (!EnsureConnectedRootFolder(root))
            return false;

        BstrWrap tn(taskName);
        if (!tn)
            return false;

        return SUCCEEDED(root->DeleteTask(tn.b, 0));
    }

    bool TaskSchedulerCom::DeleteRootSubfolder(const wchar_t* folderName) noexcept
    {
        if (!folderName || !*folderName || !m_service)
            return false;

        wil::com_ptr<ITaskFolder> root;
        if (!EnsureConnectedRootFolder(root))
            return false;

        BstrWrap name(folderName);
        if (!name)
            return false;

        return SUCCEEDED(root->DeleteFolder(name.b, 0));
    }

    bool RunSchtasksOnTask(const wchar_t* fullTaskPathOrName, const wchar_t* verbArgs) noexcept
    {
        if (!fullTaskPathOrName || !verbArgs)
            return false;

        wchar_t sysdir[MAX_PATH]{};
        if (GetSystemDirectoryW(sysdir, MAX_PATH) == 0)
            return false;

        std::wstring cmd = L"\"";
        cmd += sysdir;
        cmd += L"\\schtasks.exe\" ";
        cmd += verbArgs;
        cmd += L" /TN \"";
        cmd += fullTaskPathOrName;
        cmd += L"\"";

        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            return false;

        CloseHandle(pi.hThread);
        WaitForSingleObject(pi.hProcess, 20000);
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        return exitCode == 0;
    }

}
