#include "../src/GlassProducts.h"
#include <cassert>
#include <iostream>

int main() {
    using GlassProducts::Selection;
    using GlassProducts::InstallAction;
    static_assert(GlassProducts::ActionForInstall(false, false) == InstallAction::None);
    static_assert(GlassProducts::ActionForInstall(true, false) == InstallAction::Install);
    static_assert(GlassProducts::ActionForInstall(true, true) == InstallAction::Update);
    static_assert(GlassProducts::ActionForInstall(false, true) == InstallAction::Keep); // Setup never removes
    using GlassProducts::OpenGlassStartupState;
    static_assert(!GlassProducts::ShouldStartOpenGlass(OpenGlassStartupState{false,false,true}));
    static_assert(GlassProducts::ShouldStartOpenGlass(OpenGlassStartupState{true,false,false})); // first install
    static_assert(GlassProducts::ShouldStartOpenGlass(OpenGlassStartupState{true,false,true}));
    static_assert(!GlassProducts::ShouldStartOpenGlass(OpenGlassStartupState{true,true,false})); // upgrade, stopped at wizard start
    static_assert(GlassProducts::ShouldStartOpenGlass(OpenGlassStartupState{true,true,true}));  // upgrade, running at wizard start
    for (int bits=0;bits<16;++bits) for (bool os64:{false,true}) {
        const Selection raw{bool(bits&1),bool(bits&2),bool(bits&4),bool(bits&8)};
        const auto installed=GlassProducts::NormalizeInstall(raw,os64);
        // ОС-нативная платформа не принудительно отмечена: исправный продукт
        // остаётся снятым, обязательность держит замок карточки при обработке.
        assert(installed.platform64==(os64 && raw.platform64));
        assert(installed.platform32==raw.platform32);
        assert(installed.openGlass==(os64 && raw.openGlass));
        assert(installed.glassSettings==raw.glassSettings);
        for (int present=0;present<16;++present) {
            const Selection exists{bool(present&1),bool(present&2),bool(present&4),bool(present&8)};
            // The wizard never shows a removal: an installed product that is
            // unchecked after applying the OS-mandatory selection is kept.
            const auto keptOrAbsent=[](bool selected, bool installed) {
                const auto action=GlassProducts::ActionForInstall(selected,installed);
                return selected || action==(installed ? InstallAction::Keep : InstallAction::None);
            };
            assert(keptOrAbsent(installed.platform32,exists.platform32));
            assert(keptOrAbsent(installed.glassSettings,exists.glassSettings));
            assert(keptOrAbsent(installed.platform64,exists.platform64));
            assert(keptOrAbsent(installed.openGlass,exists.openGlass));
            const auto removed=GlassProducts::NormalizeRemoval(raw,exists,os64);
            assert(!removed.platform64 || (os64 && exists.platform64));
            assert(!removed.platform32 || exists.platform32);
            assert(!removed.glassSettings || exists.glassSettings);
            assert(!removed.openGlass || (os64 && exists.openGlass));
            const bool core=os64?removed.platform64:removed.platform32;
            if (core) {
                assert(removed.platform32==exists.platform32);
                assert(removed.glassSettings==exists.glassSettings);
                assert(removed.openGlass==(os64 && exists.openGlass));
            }
        }
    }
    const Selection all{true,true,true,true};
    const auto onlySettings=GlassProducts::NormalizeRemoval({false,false,true,false},all,true);
    assert(!onlySettings.platform64 && !onlySettings.platform32 && onlySettings.glassSettings && !onlySettings.openGlass);
    std::cout << "PASS: install keeps unchecked products; native platform kept and never removed; uninstall dependency cascade\n";
}
