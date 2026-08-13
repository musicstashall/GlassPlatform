#pragma once

#include "cpprt.h"
#include "framework.h"
#include "wil.h"

namespace GlassCore::Hooks
{
    //
    // ---------------------- ThreadSnapshotExcludeSelf ----------------------
    //

    class ThreadSnapshotExcludeSelf
    {
    public:
        ThreadSnapshotExcludeSelf();
        ~ThreadSnapshotExcludeSelf();

        void Suspend();
        void Resume();
        DWORD GetMainThreadId();

        void ForEach(const std::function<bool(const PSS_THREAD_ENTRY&)>& callback);

    private:
        HPSS m_snapshot{ nullptr };
    };

    //
    // --------------------------- Low-level helpers -------------------------
    //

    // Безопасная запись в защищённую память (меняет protection на время записи)
    void WriteMemory(void* memoryAddress, const std::function<void()>& callback) noexcept;

    // Впрыск callback в чужой поток (x64-only, возвращает адрес shellcode или nullptr)
    void* InjectCallbackToThread(DWORD threadId, const std::function<void()>& callback) noexcept;

    // Поиск модуля процесса по полному пути
    HMODULE GetProcessModule(HANDLE processHandle, std::wstring_view dllPath);

    //
    // --------------------------- IAT / Delay-IAT ---------------------------
    //

    // Обход обычного IAT
    void WalkIAT(
        void* baseAddress,
        std::string_view dllName,
        const std::function<bool(void** functionAddress, LPCSTR functionNameOrOrdinal, BOOL importedByName)>& callback
    ) noexcept;

    // Обход delay-IAT
    void WalkDelayloadIAT(
        void* baseAddress,
        std::string_view dllName,
        const std::function<bool(HMODULE* moduleHandle, void** functionAddress, LPCSTR functionNameOrOrdinal, BOOL importedByName)>& callback
    ) noexcept;

    // Получить адрес слота в IAT
    void** GetIAT(
        void* baseAddress,
        std::string_view dllName,
        LPCSTR targetFunctionNameOrOrdinal
    );

    // Получить пару (HMODULE*, void**) для delay-IAT
    std::pair<HMODULE*, void**> GetDelayloadIAT(
        void* baseAddress,
        std::string_view dllName,
        LPCSTR targetFunctionNameOrOrdinal,
        bool resolveAPI = false
    );

    // Записать detour в IAT, вернуть оригинальный адрес
    void* WriteIAT(
        void* baseAddress,
        std::string_view dllName,
        LPCSTR targetFunctionNameOrOrdinal,
        void* detourFunction
    );

    // Записать detour в delay-IAT, вернуть (оригинальный модуль, оригинальная функция)
    std::pair<HMODULE, void*> WriteDelayloadIAT(
        void* baseAddress,
        std::string_view dllName,
        LPCSTR targetFunctionNameOrOrdinal,
        void* detourFunction,
        std::optional<HMODULE> newModuleHandle = std::nullopt
    );

    // Разрулить delay-IAT, если там до сих пор trampoline/детур
    void ResolveDelayloadIAT(
        const std::pair<HMODULE*, void**>& info,
        void* baseAddress,
        std::string_view dllName,
        LPCSTR targetFunctionNameOrOrdinal
    );

    //
    // ---------------------- Code pattern / .text helpers -------------------
    //

    // Поиск сигнатуры в коде с "дырками" (asmSliceInfo: {len, gap})
    unsigned char* MatchAsmCode(
        unsigned char* startAddress,
        std::pair<unsigned char*, size_t> asmCodeInfo,
        std::vector<std::pair<size_t, size_t>> asmSliceInfo,
        size_t maxSearchBytes
    );

    // Получить адрес и размер .text секции
    std::pair<unsigned char*, size_t> GetTextSectionInfo(void* baseAddress);

    //
    // --------------------------- VTable helpers ----------------------------
    //

    // Вызов AV на фейковом объекте, чтобы вытащить индекс метода в vtable
    size_t InvokeAccessViolation(void(*vtable_function)(void*));

    template <typename T>
    __forceinline size_t GetVTableIndex(T member_function)
    {
        return InvokeAccessViolation(reinterpret_cast<void(*)(void*)>(member_function));
    }

    __forceinline void** GetObjectVTable(void* This)
    {
        return reinterpret_cast<void**>(*reinterpret_cast<void**>(This));
    }

    template <typename T>
    struct MemberFunction;

    template <typename ReturnValue, typename Interface, typename... Args>
    struct MemberFunction<ReturnValue(Interface::*)(Args...)>
    {
        using type = ReturnValue(Interface* This, Args... args);
    };

    //
    // --------------------------- ImageMapper / Offset ----------------------
    //

    class ImageMapper
    {
        wil::unique_hfile m_fileHandle{ INVALID_HANDLE_VALUE };
        wil::unique_handle m_fileMapping{ nullptr };
        wil::unique_mapview_ptr<void> m_baseAddress{ nullptr };

    public:
        explicit ImageMapper(std::wstring_view dllPath);
        ~ImageMapper() noexcept = default;

        ImageMapper(const ImageMapper&) = delete;
        ImageMapper& operator=(const ImageMapper&) = delete;

        template <typename T = HMODULE>
        __forceinline T GetBaseAddress() const
        {
            return reinterpret_cast<T>(m_baseAddress.get());
        }
    };

    struct OffsetStorage
    {
        long long value{ 0 };

        inline bool IsValid() const { return value != 0; }

        template <typename T = void*, typename T2 = void*>
        inline T To(T2 baseAddress) const
        {
            if (!baseAddress || !IsValid())
                return nullptr;

            auto base = reinterpret_cast<unsigned char*>(baseAddress);
            return reinterpret_cast<T>(base + value);
        }

        template <typename T = void*>
        static inline OffsetStorage From(T baseAddress, T targetAddress)
        {
            if (!baseAddress || !targetAddress)
                return OffsetStorage{ 0 };

            auto b = reinterpret_cast<unsigned char*>(baseAddress);
            auto t = reinterpret_cast<unsigned char*>(targetAddress);
            return OffsetStorage{ static_cast<long long>(t - b) };
        }
    };

    //
    // ------------------------------ Detours API ----------------------------
    //

    namespace Detours
    {
        HRESULT Begin();
        HRESULT End(bool commit = true);

        // Обёртка: Begin → callback → End
        HRESULT Write(const std::function<void()>& callback) noexcept;

        // Найти функцию по имени в DLL и повесить detour
        void Attach(std::string_view dllName, std::string_view funcName, void** realFuncAddr, void* hookFuncAddr);

        // Повесить detour на уже известный адрес
        void Attach(void** realFuncAddr, void* hookFuncAddr);

        // Снять detour
        void Detach(void** realFuncAddr, void* hookFuncAddr);
    }
}
