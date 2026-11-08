#pragma once
#include "GlassProducts.h"

// Mutation rows are a union, never a dependency-expanding selection. Settings
// needs the same-architecture host stopped because it supplies its Native helper.
namespace GlassProcedurePlan {
constexpr unsigned MutationMask(const GlassProducts::Selection& s,bool os64) noexcept {
    return s.removeMask | (s.platform64?1u:0u) | (s.platform32?2u:0u) |
        (s.Settings64(os64)?4u:0u) | (s.openGlass?8u:0u) | (s.Settings32(os64)?32u:0u);
}
constexpr bool Platform64(unsigned mask) noexcept {return (mask&5u)!=0;}
constexpr bool Platform32(unsigned mask) noexcept {return (mask&34u)!=0;}
constexpr bool OpenGlass(unsigned mask) noexcept {return (mask&8u)!=0;}
constexpr bool Settings(unsigned mask) noexcept {return (mask&36u)!=0;}
constexpr bool ModuleScan(unsigned mask) noexcept {return (mask&39u)!=0;}
}
