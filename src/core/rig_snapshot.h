#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

namespace rdrvr::rig_snapshot {

// One rig build only. Region checks are reused while copying that build's
// metadata; never retain this reader across frames. Every copy still has an
// SEH backstop in case the game releases or protects memory after the query.
class Reader {
    struct Region { uintptr_t begin, end; bool readable; };
    std::vector<Region> regions_;
    size_t queries_ = 0;

    static bool guarded_copy(void* out, uintptr_t address, size_t bytes) {
        __try {
            std::memcpy(out, reinterpret_cast<const void*>(address), bytes);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool region(uintptr_t address, Region& out) {
        for (const auto& r : regions_)
            if (address >= r.begin && address < r.end) { out = r; return r.readable; }
        MEMORY_BASIC_INFORMATION mi{};
        ++queries_;
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &mi, sizeof(mi))) return false;
        const auto begin = reinterpret_cast<uintptr_t>(mi.BaseAddress);
        if (mi.RegionSize > std::numeric_limits<uintptr_t>::max() - begin) return false;
        out = {begin, begin + mi.RegionSize,
            mi.State == MEM_COMMIT && !(mi.Protect & PAGE_GUARD) &&
            (mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0};
        if (address < out.begin || address >= out.end) return false;
        regions_.push_back(out);
        return out.readable;
    }

public:
    size_t queries() const { return queries_; }

    bool copy(uintptr_t address, void* out, size_t bytes) {
        if (!address || !out || !bytes || bytes > std::numeric_limits<uintptr_t>::max() - address) return false;
        auto* dst = static_cast<unsigned char*>(out);
        while (bytes) {
            Region r{};
            if (!region(address, r)) return false;
            const size_t n = std::min(bytes, static_cast<size_t>(r.end - address));
            if (!guarded_copy(dst, address, n)) return false;
            address += n;
            dst += n;
            bytes -= n;
        }
        return true;
    }

    // Stop at the terminator without requiring the following page to exist.
    // Unreadable/unterminated names are omitted, never partially matched.
    bool name(uintptr_t address, std::array<char, 40>& out) {
        out.fill(0);
        if (!address) return false;
        size_t used = 0;
        while (used < out.size()) {
            Region r{};
            if (!region(address, r)) break;
            const size_t n = std::min(out.size() - used, static_cast<size_t>(r.end - address));
            if (!guarded_copy(out.data() + used, address, n)) break;
            if (std::memchr(out.data() + used, 0, n)) return true;
            used += n;
            if (n > std::numeric_limits<uintptr_t>::max() - address) break;
            address += n;
        }
        out.fill(0);
        return false;
    }
};

constexpr size_t kStride = 0x110;
constexpr int kMaxBones = 512;

struct Header {
    uintptr_t bones = 0, locals = 0;
    uint16_t count = 0;
    bool operator==(const Header&) const = default;
};

template <class T>
T field(const unsigned char* data, size_t offset) {
    T value{};
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

inline bool header(Reader& reader, uintptr_t data, Header& out) {
    std::array<unsigned char, 0x32> bytes{};
    if (!reader.copy(data, bytes.data(), bytes.size())) return false;
    out = {field<uintptr_t>(bytes.data(), 0), field<uintptr_t>(bytes.data(), 0x20), field<uint16_t>(bytes.data(), 0x30)};
    return true;
}

struct Snapshot {
    Header source;
    std::vector<std::array<char, 40>> names;
    std::vector<int> parents;
    std::vector<std::array<float, 16>> locals;
    bool has_locals = false;

    // Raw pointers become checked indices. Reject cross-array pointers,
    // misalignment and cycles before any group can index a result vector.
    // A self-parent root is equivalent to a null parent in the old bounded
    // subtree walks; normalize it rather than rejecting that root convention.
    bool load(Reader& reader, uintptr_t data) {
        *this = {};
        if (!header(reader, data, source) || !source.bones || source.count == 0 || source.count > kMaxBones) return false;
        const size_t count = source.count;
        if (count * kStride > std::numeric_limits<uintptr_t>::max() - source.bones) return false;
        std::vector<unsigned char> records(count * kStride);
        if (!reader.copy(source.bones, records.data(), records.size())) return false;
        names.resize(count);
        parents.assign(count, -1);
        for (size_t i = 0; i < count; ++i) {
            const auto* rec = records.data() + i * kStride;
            const auto parent = field<uintptr_t>(rec, 0x20);
            if (parent) {
                if (parent < source.bones) return false;
                const auto delta = parent - source.bones;
                if (delta % kStride || delta / kStride >= count) return false;
                if (delta / kStride != i) parents[i] = static_cast<int>(delta / kStride);
            }
            reader.name(field<uintptr_t>(rec, 0), names[i]);
        }
        for (size_t i = 0; i < count; ++i) {
            int p = static_cast<int>(i);
            size_t depth = 0;
            while (p >= 0 && depth < count) { p = parents[p]; ++depth; }
            if (p >= 0) return false;
        }
        if (source.locals) {
            locals.resize(count);
            has_locals = reader.copy(source.locals, locals.data(), count * sizeof(locals[0]));
            if (!has_locals) locals.clear();  // retain the existing missing-bind-data fallback
        }
        Header after;
        return header(reader, data, after) && after == source;
    }

    int index(std::string_view name) const {
        if (name.empty()) return -1;
        for (size_t i = 0; i < names.size(); ++i)
            if (name == names[i].data()) return static_cast<int>(i);
        return -1;
    }

    std::vector<int> subtree(int root) const {
        if (root < 0 || static_cast<size_t>(root) >= parents.size()) return {};
        std::vector<int> out{root};
        for (size_t i = 0; i < parents.size(); ++i) {
            if (static_cast<int>(i) == root) continue;
            int p = parents[i];
            // Preserve the original traversal limit and root-first/index order.
            for (int depth = 0; p >= 0 && depth < 64; ++depth) {
                if (p == root) { out.push_back(static_cast<int>(i)); break; }
                p = parents[p];
            }
        }
        return out;
    }
};

}  // namespace rdrvr::rig_snapshot
