#include "core/rig_snapshot.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <random>
#include <string>

namespace rs = rdrvr::rig_snapshot;
size_t checks = 0;
void check(bool ok, const char* why) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << why << '\n'; std::exit(1); }
}

template <class T>
void put(uintptr_t base, size_t offset, T value) {
    std::memcpy(reinterpret_cast<void*>(base + offset), &value, sizeof(value));
}

// Synthetic game-layout records in memory owned only by this test process.
struct Fixture {
    unsigned char* block = static_cast<unsigned char*>(VirtualAlloc(nullptr, 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    uintptr_t data = reinterpret_cast<uintptr_t>(block), bones = data + 0x100, names = data + 0x30000, locals = data + 0x40000;
    explicit Fixture(const std::vector<int>& parents) {
        check(block != nullptr, "fixture allocation");
        put(data, 0, bones);
        put(data, 0x20, locals);
        put(data, 0x30, static_cast<uint16_t>(parents.size()));
        for (size_t i = 0; i < parents.size(); ++i) {
            put(bones + i * rs::kStride, 0, names + i * 64);
            put(bones + i * rs::kStride, 0x20, parents[i] < 0 ? uintptr_t{0} : bones + static_cast<size_t>(parents[i]) * rs::kStride);
            std::snprintf(reinterpret_cast<char*>(names + i * 64), 64, "bone_%zu", i);
            std::array<float, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, static_cast<float>(i), 1.0f, 2.0f, 1.0f};
            std::memcpy(reinterpret_cast<void*>(locals + i * sizeof(m)), m.data(), sizeof(m));
        }
    }
    ~Fixture() { if (block) VirtualFree(block, 0, MEM_RELEASE); }
    void name(size_t i, const char* name) { std::snprintf(reinterpret_cast<char*>(names + i * 64), 64, "%s", name); }
};

// Independent ancestry oracle: distance to root in the full validated forest,
// then apply the original 64-parent-edge limit and root-first/index ordering.
std::vector<int> expected(const std::vector<int>& parent, int root) {
    std::vector<int> result{root};
    for (size_t i = 0; i < parent.size(); ++i) {
        if (static_cast<int>(i) == root) continue;
        int at = static_cast<int>(i), distance = 0;
        while (at >= 0 && at != root) { at = parent[at]; ++distance; }
        if (at == root && distance <= 64) result.push_back(static_cast<int>(i));
    }
    return result;
}

void compare(const std::vector<int>& parents) {
    Fixture f(parents);
    rs::Reader reader;
    rs::Snapshot snapshot;
    check(snapshot.load(reader, f.data), "valid snapshot loads");
    check(snapshot.parents == parents, "parent indices preserved including non-topological order");
    check(snapshot.has_locals, "default matrices available");
    for (size_t i = 0; i < parents.size(); ++i) {
        check(snapshot.index("bone_" + std::to_string(i)) == static_cast<int>(i), "name lookup");
        check(snapshot.subtree(static_cast<int>(i)) == expected(parents, static_cast<int>(i)), "subtree membership/depth/order unchanged");
        check(snapshot.locals[i][12] == static_cast<float>(i), "bind matrix copied exactly");
    }
    const auto queries = reader.queries();
    check(queries == 1, "one region query for a rig in one committed region");
    check(snapshot.subtree(-1).empty() && snapshot.subtree(static_cast<int>(parents.size())).empty(), "invalid group root rejected");
    check(snapshot.index("absent") == -1 && snapshot.index("") == -1, "missing names stay absent");
    // All subsequent group/name operations use copied metadata, even after
    // source memory changes. This snapshot is discarded at the end of the build.
    std::memset(f.block, 0, 1024 * 1024);
    check(snapshot.subtree(0) == expected(parents, 0), "groups use the local snapshot");
    check(reader.queries() == queries, "group traversal makes no memory queries");
}

void invalid_metadata() {
    for (uint16_t count : {uint16_t{0}, uint16_t{513}}) {
        Fixture f({-1, 0}); put(f.data, 0x30, count);
        rs::Reader r; rs::Snapshot s;
        check(!s.load(r, f.data), "invalid bone count rejected");
    }
    for (int kind = 0; kind < 4; ++kind) {
        Fixture f({-1, 0, 1});
        uintptr_t bad = kind == 0 ? f.bones - rs::kStride : kind == 1 ? f.bones + 3 * rs::kStride :
                        kind == 2 ? f.bones + 1 : f.bones + rs::kStride;
        put(f.bones + 2 * rs::kStride, 0x20, bad);
        if (kind == 3) put(f.bones + rs::kStride, 0x20, f.bones + 2 * rs::kStride);
        rs::Reader r; rs::Snapshot s;
        check(!s.load(r, f.data), "outside/misaligned/cyclic parent rejected");
    }
    {
        Fixture f({0, 0, 2, 2});
        rs::Reader r; rs::Snapshot s;
        check(s.load(r, f.data), "self-parent roots accepted without a cyclic walk");
        check(s.parents == std::vector<int>({-1, 0, -1, 2}), "self-parent roots normalize to null roots");
        check(s.subtree(0) == std::vector<int>({0, 1}) && s.subtree(2) == std::vector<int>({2, 3}), "self-parent root groups preserved");
    }
    {
        Fixture f({-1}); put(f.data, 0, std::numeric_limits<uintptr_t>::max() - 16);
        rs::Reader r; rs::Snapshot s;
        check(!s.load(r, f.data), "bone range overflow rejected");
    }
    {
        Fixture f({-1, 0}); f.name(0, "head"); f.name(1, "wrist_r_Attachment");
        rs::Reader r; rs::Snapshot s;
        check(s.load(r, f.data) && s.index("head") == 0 && s.index("wrist_r_Attachment") == 1, "real bone name forms preserved");
        put(f.bones, 0, uintptr_t{1});
        rs::Reader r2; rs::Snapshot s2;
        check(s2.load(r2, f.data) && s2.index("head") == -1, "unreadable name cannot match head");
    }
    {
        Fixture f({-1}); f.name(0, "head"); put(f.data, 0x20, uintptr_t{1});
        rs::Reader r; rs::Snapshot s;
        check(s.load(r, f.data) && !s.has_locals && s.locals.empty(), "missing bind matrices retain existing fallback");
    }
    {
        Fixture f({-1}); std::memset(reinterpret_cast<void*>(f.names), 'x', 64);
        rs::Reader r; rs::Snapshot s;
        check(s.load(r, f.data) && s.names[0][0] == 0, "unterminated name omitted");
    }
}

void page_boundaries() {
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    const size_t page = info.dwPageSize;
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, page * 3, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(memory != nullptr, "page fixture allocation");
    const auto base = reinterpret_cast<uintptr_t>(memory);
    std::memcpy(memory + page - 4, "head", 5);  // terminator on the next readable region
    std::memcpy(memory + page * 2 - 5, "head", 5);  // terminator before the inaccessible region
    DWORD old = 0;
    check(VirtualProtect(memory + page, page, PAGE_READONLY, &old) != 0, "test-owned middle page read-only");
    check(VirtualProtect(memory + page * 2, page, PAGE_NOACCESS, &old) != 0, "test-owned last page inaccessible");
    rs::Reader r;
    std::array<char, 40> name;
    check(r.name(base + page - 4, name) && std::strcmp(name.data(), "head") == 0, "name across readable regions");
    check(r.queries() == 2, "each readable region queried once");
    check(r.name(base + page * 2 - 5, name) && std::strcmp(name.data(), "head") == 0, "terminator at inaccessible boundary");
    std::array<unsigned char, 16> bytes{};
    check(!r.copy(base + page * 2 - 8, bytes.data(), bytes.size()), "copy spanning inaccessible region rejected");
    check(!r.copy(0, bytes.data(), 1), "null rejected");
    check(!r.copy(std::numeric_limits<uintptr_t>::max() - 3, bytes.data(), 8), "overflow rejected");
    check(VirtualProtect(memory, page, PAGE_NOACCESS, &old) != 0, "change only test-owned cached page protection");
    check(!r.copy(base, bytes.data(), bytes.size()), "SEH rejects mapping changed after query");
    check(VirtualProtect(memory + page * 2, page, PAGE_READWRITE | PAGE_GUARD, &old) != 0, "guard test-owned page");
    rs::Reader fresh;
    check(!fresh.copy(base + page * 2, bytes.data(), bytes.size()), "guard page rejected before copy");
    MEMORY_BASIC_INFORMATION mi{};
    VirtualQuery(memory + page * 2, &mi, sizeof(mi));
    check((mi.Protect & PAGE_GUARD) != 0, "guard was not consumed");
    VirtualFree(memory, 0, MEM_RELEASE);
}

int main() {
    compare({-1});
    compare({-1, 0, 0, 1, 1, 2, 2, -1, 7});
    compare({2, 2, -1, 0, 1});
    std::vector<int> chain(512);
    for (int i = 0; i < 512; ++i) chain[i] = i - 1;
    compare(chain);
    std::mt19937 rng(0x524452);
    for (int run = 0; run < 40; ++run) {
        const int n = run % 2 ? 119 : 512;
        std::vector<int> parents(n, -1);
        for (int i = 1; i < n; ++i) parents[i] = static_cast<int>(rng() % (i + 1)) - 1;
        compare(parents);
    }
    invalid_metadata();
    page_boundaries();
    std::cout << "PASS: " << checks << " rig snapshot checks; valid groups preserved, malformed data bounded, one query per region, no queries during group traversal.\n";
}
