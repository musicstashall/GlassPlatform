#pragma once
#include "GlassPackageRules.h"
#include "../../third_party/nlohmann/json.hpp"
#include <algorithm>
#include <stdexcept>

namespace GlassPackageCatalog {
using GlassPackageRules::Version;
enum class Product : std::size_t { Platform64, Platform32, Settings64, Settings32, OpenGlass, Count };
constexpr std::size_t kCount = static_cast<std::size_t>(Product::Count);
inline constexpr const char* kIds[] = {"platform64", "platform32", "settings64", "settings32", "openglass"};
inline constexpr const char* kFiles[] = {"Glass-x64.msi", "Glass-x86.msi", "GlassSettings-x64.msi", "GlassSettings-x86.msi", "OpenGlass-x64.msi"};
struct Asset {
    Version version;
    std::string file, url, sha256;
    std::uint64_t size = 0;
    bool downloadable = false;
};
struct Catalog {
    std::optional<Version> setupVersion;
    std::optional<Asset> setup;
    std::array<std::optional<Asset>, kCount> products{};
};
inline bool Digest(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
inline bool ReleaseUrl(const std::string& url, const std::string& file) {
    const std::string prefix = "https://github.com/musicstashall/GlassPlatform/releases/download/";
    if (url.compare(0, prefix.size(), prefix) != 0) return false;
    const auto slash = url.find('/', prefix.size());
    if (slash == std::string::npos || url.substr(slash + 1) != file) return false;
    const auto tag = url.substr(prefix.size(), slash - prefix.size());
    return !tag.empty() && tag.size() <= 128 && tag.find("..") == std::string::npos &&
        std::all_of(tag.begin(), tag.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        });
}
inline Version ReadVersion(const nlohmann::json& value) {
    Version version;
    if (!GlassPackageRules::ParseVersion(value.get<std::string>(), version))
        throw std::runtime_error("Invalid numeric version");
    return version;
}
inline Asset ReadAsset(const nlohmann::json& value, const char* expectedFile) {
    Asset asset;
    asset.version = ReadVersion(value.at("version"));
    asset.file = value.at("file").get<std::string>();
    if (asset.file != expectedFile) throw std::runtime_error("Unexpected asset filename");
    // Missing digest/size makes this release source unavailable, not an excuse
    // to trust unverified downloads. Local independent packages remain usable.
    if (!value.contains("sha256") || !value.contains("size")) return asset;
    asset.sha256 = value.at("sha256").get<std::string>();
    if (!value.at("size").is_number_unsigned()) throw std::runtime_error("Invalid asset size");
    asset.size = value.at("size").get<std::uint64_t>();
    asset.url = value.at("url").get<std::string>();
    if (!Digest(asset.sha256) || asset.size < 512 || asset.size > 1024ull * 1024 * 1024 ||
        !ReleaseUrl(asset.url, asset.file)) throw std::runtime_error("Invalid asset digest, size or URL");
    asset.downloadable = true;
    return asset;
}
inline bool Parse(const std::string& text, Catalog& out, std::string& error) {
    out = {};
    try {
        if (text.size() > 1024 * 1024) throw std::runtime_error("Catalog exceeds limit");
        // Reject duplicate JSON keys instead of accepting last-key-wins data.
        std::vector<std::vector<std::string>> keys;
        const auto json = nlohmann::json::parse(text, [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
            if (depth > 32) throw std::runtime_error("Catalog nesting exceeds limit");
            using Event = nlohmann::json::parse_event_t;
            if (event == Event::object_start) keys.emplace_back();
            else if (event == Event::object_end) keys.pop_back();
            else if (event == Event::key) {
                auto key = parsed.get<std::string>();
                auto& current = keys.back();
                if (current.size() >= 64) throw std::runtime_error("Too many object keys");
                if (std::find(current.begin(), current.end(), key) != current.end())
                    throw std::runtime_error("Duplicate JSON key");
                current.push_back(std::move(key));
            }
            return true;
        });
        if (!json.is_object()) throw std::runtime_error("Expected catalog object");
        Catalog result;
        if (json.contains("setup")) result.setupVersion = ReadVersion(json.at("setup"));
        else if (json.contains("glass")) result.setupVersion = ReadVersion(json.at("glass"));
        if (json.contains("setupPackage")) {
            auto asset = ReadAsset(json.at("setupPackage"), "GlassSetup.exe");
            if (result.setupVersion && GlassPackageRules::Compare(*result.setupVersion, asset.version) != 0)
                throw std::runtime_error("Setup version mismatch");
            result.setupVersion = asset.version;
            result.setup = std::move(asset);
        }
        if (json.contains("products")) {
            if (!json.at("products").is_array() || json.at("products").size() > 64)
                throw std::runtime_error("Invalid products list");
            for (const auto& entry : json.at("products")) {
                const auto id = entry.at("id").get<std::string>();
                for (std::size_t i = 0; i < kCount; ++i) {
                    if (id != kIds[i]) continue; // Future IDs are not executable candidates.
                    if (result.products[i]) throw std::runtime_error("Duplicate product ID");
                    result.products[i] = ReadAsset(entry, kFiles[i]);
                }
            }
        }
        if (!result.setupVersion && !json.contains("products")) throw std::runtime_error("Empty catalog");
        out = std::move(result);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        out = {};
        error = e.what();
        return false;
    }
}
}
