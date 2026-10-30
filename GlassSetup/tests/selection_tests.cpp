#include "../src/GlassProducts.h"
#include <cassert>
#include <iostream>

int main() {
    using GlassProducts::Selection;
    for (int bits=0;bits<16;++bits) for (bool os64:{false,true}) {
        const Selection raw{bool(bits&1),bool(bits&2),bool(bits&4),bool(bits&8)};
        const auto installed=GlassProducts::NormalizeInstall(raw,os64);
        assert(os64 ? installed.platform64 : installed.platform32);
        if (!os64) assert(!installed.platform64 && !installed.openGlass);
        assert(installed.glassSettings==raw.glassSettings);
        if (os64) {
            assert(installed.platform32==raw.platform32);
            assert(installed.openGlass==raw.openGlass);
        }
        for (int present=0;present<16;++present) {
            const Selection exists{bool(present&1),bool(present&2),bool(present&4),bool(present&8)};
            const auto changed=GlassProducts::RemovalForInstall(raw,exists,os64);
            assert(!changed.platform64); // the x64 OS platform is mandatory
            assert(changed.platform32==(os64 && exists.platform32 && !raw.platform32));
            assert(changed.glassSettings==(exists.glassSettings && !raw.glassSettings));
            assert(changed.openGlass==(os64 && exists.openGlass && !raw.openGlass));
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
    std::cout << "PASS: desired install set removes unchecked products; native platform mandatory; uninstall dependency cascade\n";
}
