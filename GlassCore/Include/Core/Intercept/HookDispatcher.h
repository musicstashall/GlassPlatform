// GlassCore/Include/Core/Intercept/HookDispatcher.h
#pragma once

// OffsetStorage определён в HookHelper.h — не дублировать здесь.
#include <Core/Intercept/HookHelper.h>
#include <Core/Intercept/IATRegistry.h>
#include <wil.h>

namespace GlassCore::Hooks
{
    //
    // ----------------------- HookDispatcherDependency ----------------------
    //

    template <size_t hookCount, size_t possibleImportCount>
    using HookDispatcherDependency = std::array<
        std::tuple<
        std::array<std::string_view, possibleImportCount>,
        LPCSTR,
        void*
        >,
        hookCount
    >;

    //
    // --------------------------- HookDispatcher ---------------------------
    //

    template <size_t hookCount, size_t possibleImportCount>
    struct HookDispatcher
    {
        const HookDispatcherDependency<hookCount, possibleImportCount>& hookDependency;

        std::array<std::pair<OffsetStorage, std::optional<OffsetStorage>>, hookCount> hookInfoCache{};
        std::array<std::pair<void*, std::optional<HMODULE>>, hookCount> hookTable{};
        std::array<size_t, hookCount> hookRef{};
        mutable wil::srwlock hookLock{};
        void* moduleAddress{ nullptr };

        //
        // ------------------------ CacheHookData ----------------------------
        //

        void CacheHookData(size_t index = 0, bool single = false)
        {
            if (!moduleAddress)
                return;

            void* liveBase = moduleAddress;

            auto initOffset = [&](size_t i, void** funcSlot, HMODULE* modSlot)
                {
                    hookInfoCache[i] = std::make_pair(
                        OffsetStorage::From<void*>(liveBase, static_cast<void*>(funcSlot)),
                        modSlot
                        ? std::optional<OffsetStorage>(OffsetStorage::From<void*>(liveBase, static_cast<void*>(modSlot)))
                        : std::nullopt
                    );
                };

            for (size_t i = (single ? index : 0); i < hookDependency.size(); ++i)
            {
                if (hookInfoCache[i].first.IsValid())
                {
                    if (single) break;
                    continue;
                }

                auto& dep = hookDependency[i];
                auto& possibleImports = std::get<0>(dep);
                auto  funcName = std::get<1>(dep);

                for (auto dllName : possibleImports)
                {
                    if (dllName.empty())
                        continue;

                    void** funcSlot = Hooks::GetIAT(liveBase, dllName, funcName);
                    if (funcSlot)
                    {
                        initOffset(i, funcSlot, nullptr);
                        break;
                    }

                    auto [modSlot, funcSlotDelay] = Hooks::GetDelayloadIAT(liveBase, dllName, funcName);
                    if (funcSlotDelay)
                    {
                        initOffset(i, funcSlotDelay, modSlot);
                        break;
                    }
                }

                if (single) break;
            }
        }

        //
        // --------------------------- State helpers -------------------------
        //

        bool IsHookEnabled(size_t index) const
        {
            auto lock = hookLock.lock_shared();
            return hookRef[index] > 0;
        }

        int GetRef(size_t index) const
        {
            auto lock = hookLock.lock_shared();
            return static_cast<int>(hookRef[index]);
        }

        //
        // ---------------------------- EnableHook ---------------------------
        //

        bool EnableHook(size_t index, bool enable)
        {
            if (!moduleAddress)
                return false;

            auto lock = hookLock.lock_exclusive();

            if (!hookInfoCache[index].first.IsValid())
                CacheHookData(index, true);

            if (!hookInfoCache[index].first.IsValid())
                return false;

            bool hookChanged = false;
            if (enable && hookRef[index] == 0) hookChanged = true;
            if (!enable && hookRef[index] == 1) hookChanged = true;

            hookRef[index] += enable ? 1 : static_cast<size_t>(-1);

            if (!hookChanged)
                return false;

            auto& [funcOffset, moduleOffset] = hookInfoCache[index];
            auto* iatFunc = reinterpret_cast<void**>(funcOffset.To(moduleAddress));

            std::optional<HMODULE*> iatModule;
            if (moduleOffset.has_value() && moduleOffset->IsValid())
                iatModule = reinterpret_cast<HMODULE*>(moduleOffset->To(moduleAddress));

            if (enable)
            {
                auto& dep = hookDependency[index];
                auto& possibleImports = std::get<0>(dep);
                auto  funcName = std::get<1>(dep);
                auto  detour = std::get<2>(dep);

                if (iatModule.has_value())
                {
                    Hooks::ResolveDelayloadIAT(
                        std::make_pair(iatModule.value(), iatFunc),
                        moduleAddress,
                        possibleImports[0],
                        funcName
                    );
                }

                if (iatModule.has_value())
                    hookTable[index].second = *iatModule.value();

                void* effectiveOriginal = nullptr;
                if (!IATRegistry::Instance().Register(iatFunc, detour, &effectiveOriginal))
                {
                    hookRef[index]--;
                    return false;
                }
                hookTable[index].first = effectiveOriginal;
            }
            else
            {
                auto detour = std::get<2>(hookDependency[index]);

                IATRegistry::Instance().Unregister(iatFunc, detour);
                hookTable[index].first = nullptr;

                if (iatModule.has_value() && hookTable[index].second.has_value())
                {
                    Hooks::WriteMemory(iatModule.value(), [&]
                        {
                            *iatModule.value() = hookTable[index].second.value();
                            hookTable[index].second.reset();
                        });
                }
            }

            return true;
        }

        //
        // ------------------------- EnableHookNoRef -------------------------
        //

        bool EnableHookNoRef(size_t index, bool enable)
        {
            if ((IsHookEnabled(index) && enable) || (!IsHookEnabled(index) && !enable))
                return false;

            return EnableHook(index, enable);
        }

        //
        // -------------------------- DisableAllHooks ------------------------
        //

        void DisableAllHooks()
        {
            if (!moduleAddress)
                return;

            auto lock = hookLock.lock_exclusive();

            for (size_t i = 0; i < hookInfoCache.size(); ++i)
            {
                if (hookRef[i] == 0)
                    continue;

                auto& [funcOffset, moduleOffset] = hookInfoCache[i];

                if (funcOffset.IsValid())
                {
                    auto* iatFunc = reinterpret_cast<void**>(funcOffset.To(moduleAddress));
                    auto  detour = std::get<2>(hookDependency[i]);
                    IATRegistry::Instance().Unregister(iatFunc, detour);
                }

                if (moduleOffset.has_value() && moduleOffset->IsValid()
                    && hookTable[i].second.has_value())
                {
                    auto* maddr = reinterpret_cast<HMODULE*>(moduleOffset->To(moduleAddress));
                    Hooks::WriteMemory(maddr, [&]
                        {
                            *maddr = hookTable[i].second.value();
                        });
                }

                hookRef[i] = 0;
                hookTable[i].first = nullptr;
                hookTable[i].second = std::nullopt;
            }
        }

        //
        // ----------------------------- GetOrg ------------------------------
        //

        template <size_t index, typename T = void*>
        [[nodiscard]] __forceinline T GetOrg() const
        {
            auto lock = hookLock.lock_shared();
            return reinterpret_cast<T>(hookTable[index].first);
        }

        //
        // --------------------------- Constructors --------------------------
        //

        HookDispatcher(
            const HookDispatcherDependency<hookCount, possibleImportCount>& dependency,
            void* targetModule
        )
            : hookDependency(dependency)
            , hookInfoCache{}
            , hookTable{}
            , hookRef{}
            , hookLock{}
            , moduleAddress(targetModule)
        {
        }

        HookDispatcher() = delete;
    };

    //
    // --------------------------- CTAD helper ------------------------------
    //

    template <size_t hookCount, size_t possibleImportCount>
    HookDispatcher(
        const HookDispatcherDependency<hookCount, possibleImportCount>&,
        void*
    ) -> HookDispatcher<hookCount, possibleImportCount>;

    //
    // --------------------------- THookDispatcher --------------------------
    //

    template <typename THookDispatcherDependency>
    struct THookDispatcher;

    template <size_t hookCount, size_t possibleImportCount>
    struct THookDispatcher<HookDispatcherDependency<hookCount, possibleImportCount>>
    {
        using type = HookDispatcher<hookCount, possibleImportCount>;
    };

} // namespace GlassCore::Hooks