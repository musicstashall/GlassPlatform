#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

// Selection only: a choice never authorizes MSI execution. Acquisition must lock,
// copy and verify the actual MSI/identity/hash before any product mutation.
namespace GlassPackageRules {
struct Version {
    std::array<std::uint32_t, 4> parts{};
};
inline int Compare(const Version& a, const Version& b) noexcept {
    return a.parts < b.parts ? -1 : a.parts == b.parts ? 0 : 1;
}
inline bool ParseVersion(const std::string& text, Version& out) noexcept {
    out = {};
    Version parsed;
    std::size_t begin = 0, count = 0;
    while (begin < text.size() && count < 4) {
        const auto end = text.find('.', begin);
        const auto length = (end == std::string::npos ? text.size() : end) - begin;
        if (!length || length > 10) return false;
        std::uint32_t number = 0;
        for (std::size_t i = begin; i < begin + length; ++i) {
            const char c = text[i];
            if (c < '0' || c > '9') return false;
            const auto digit = static_cast<std::uint32_t>(c - '0');
            if (number > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) return false;
            number = number * 10 + digit;
        }
        parsed.parts[count++] = number;
        if (end == std::string::npos) {
            if (count < 3) return false;
            out = parsed;
            return true;
        }
        begin = end + 1;
    }
    return false;
}
enum class Source { Current, LocalPackages, LocalAdjacent, Release };
struct Candidate {
    Source source;
    Version version;
    // Local/cache: inspected file facts; release: validated catalog metadata.
    // This does NOT assert that a future download's bytes have been verified.
    bool eligible = false;
};
struct Request {
    std::optional<Version> installed;
    // Explicit repair uses installed as target. Otherwise target may be inferred
    // ONLY from local/release candidates, never from an old Current package.
    std::optional<Version> target;
};
enum class Failure { None, NoSource, Downgrade };
struct Choice {
    std::optional<std::size_t> index;
    std::optional<Version> target;
    Failure failure = Failure::NoSource;
};
inline Choice Select(const Request& request, const std::vector<Candidate>& candidates) {
    auto target = request.target;
    if (target && request.installed && Compare(*target, *request.installed) < 0)
        return {{}, target, Failure::Downgrade};
    if (!target) {
        for (const auto& c : candidates) {
            if (!c.eligible || c.source == Source::Current) continue;
            if (!target || Compare(c.version, *target) > 0) target = c.version;
        }
    }
    if (!target) return {};
    if (request.installed && Compare(*target, *request.installed) < 0)
        return {{}, target, Failure::Downgrade};
    Choice result{{}, target, Failure::NoSource};
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        if (!c.eligible || Compare(c.version, *target) != 0) continue;
        if (c.source == Source::Current &&
            (!request.installed || Compare(*target, *request.installed) != 0)) continue;
        if (!result.index || static_cast<int>(c.source) < static_cast<int>(candidates[*result.index].source)) {
            result.index = i;
            result.failure = Failure::None;
        }
    }
    return result;
}
}
