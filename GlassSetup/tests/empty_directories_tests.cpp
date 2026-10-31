#include "../src/InstallSupport/GlassEmptyDirectories.h"
#include <cassert>
#include <map>
#include <set>
#include <string>
#include <vector>

using GlassEmptyDirectories::Entry;

// A filesystem model: removal succeeds only for an ordinary, empty directory.
struct FakeFileSystem {
    struct Node { bool directory = true, reparse = false, readable = true; };
    struct Handle { std::wstring path; bool valid = false; explicit operator bool() const { return valid; } };
    std::map<std::wstring, Node> nodes;
    std::set<std::wstring> opened, removed, protectedDirectories;
    std::wstring changedToLink;

    Handle OpenOrdinaryDirectory(const std::wstring& path) {
        opened.insert(path);
        auto it = nodes.find(path);
        if (path == changedToLink && it != nodes.end()) it->second.reparse = true;
        return {path, it != nodes.end() && it->second.directory && !it->second.reparse &&
            it->second.readable && !protectedDirectories.count(path)};
    }
    bool ReadEntries(const Handle& h, const std::wstring&, std::vector<Entry>& entries) {
        if (!nodes.at(h.path).readable) return false;
        const std::wstring prefix = h.path + L"\\";
        for (const auto& [path, node] : nodes) {
            if (path.rfind(prefix, 0) != 0) continue;
            const auto remainder = path.substr(prefix.size());
            if (remainder.find(L'\\') == std::wstring::npos)
                entries.push_back({remainder, node.directory, node.reparse});
        }
        return true;
    }
    void RemoveIfEmpty(const Handle& h, const std::wstring&) {
        const std::wstring prefix = h.path + L"\\";
        for (const auto& [path, node] : nodes)
            if (path.rfind(prefix, 0) == 0) return;
        removed.insert(h.path);
        nodes.erase(h.path);
    }
};

int main() {
    FakeFileSystem fs;
    fs.nodes = {
        {L"root", {}}, {L"root\\empty", {}}, {L"root\\empty\\nested", {}},
        {L"root\\data", {}}, {L"root\\data\\user.txt", {false}},
        {L"root\\link", {true, true}}, {L"root\\link\\outside", {}},
        {L"root\\race", {}}, {L"root\\race\\outside", {}},
        {L"root\\unreadable", {true, false, false}},
    };
    // Simulate a directory replaced by a link after the parent was enumerated.
    fs.changedToLink = L"root\\race";
    GlassEmptyDirectories::Prune(fs, L"root");
    assert(fs.removed.count(L"root\\empty\\nested"));
    assert(fs.removed.count(L"root\\empty"));
    for (const auto* kept : {L"root", L"root\\data", L"root\\data\\user.txt",
             L"root\\link", L"root\\link\\outside", L"root\\race", L"root\\race\\outside", L"root\\unreadable"})
        assert(fs.nodes.count(kept));
    assert(!fs.opened.count(L"root\\link"));
    assert(!fs.opened.count(L"root\\link\\outside"));
    assert(!fs.opened.count(L"root\\race\\outside"));

    FakeFileSystem empty;
    empty.nodes = {{L"root", {}}, {L"root\\one", {}}, {L"root\\one\\two", {}}};
    GlassEmptyDirectories::Prune(empty, L"root");
    assert(empty.nodes.empty());

    // A kept product nested within a removed product root must remain intact,
    // while empty siblings belonging to the removed product can still go.
    FakeFileSystem overlap;
    overlap.nodes = {{L"root", {}}, {L"root\\kept", {}}, {L"root\\kept\\empty", {}},
                     {L"root\\removed", {}}, {L"root\\removed\\empty", {}}};
    overlap.protectedDirectories.insert(L"root\\kept");
    GlassEmptyDirectories::Prune(overlap, L"root");
    assert(overlap.nodes.count(L"root\\kept\\empty"));
    assert(overlap.nodes.count(L"root\\kept"));
    assert(overlap.nodes.count(L"root"));
    assert(!overlap.nodes.count(L"root\\removed"));

    assert(GlassEmptyDirectories::SafeName(L"normal"));
    for (const auto* name : {L"", L".", L"..", L"a\\b", L"a/b"})
        assert(!GlassEmptyDirectories::SafeName(name));
}
