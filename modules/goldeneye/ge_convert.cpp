// Native port of the GoldenEye 007 PC port's first-run sidecar converters (tools_pc/d43_emit.py, d69_emit.py,
// d88_emit.py + d88_propdefs.py at v0.6.0). The port reads model geometry, stage bg/stan data and stage setups from
// PC-layout "sidecars" converted from the player's ROM; release builds spawn a frozen Python converter, which Android
// can't run, so the module converts in-process before the game starts (plm_shim.cpp).
//
// Inputs: the ROM, plus ge007-convert-meta.txt (tools/gen_convert_meta.py: the decomp's file list, file table order
// and model header counts -- committed decomp sources, no ROM data). Outputs, as the Python writes them:
//   <data>/pcmodels-<region>/{pcmodels.bin,manifest.csv}   (d43)
//   <data>/pccg-<region>/{pccg.bin,manifest.csv}           (d69, then d88 appended)
//
// The conversion logic follows the Python statement by statement (same walk orders, same region maps, same
// relocation rules), including its quirks, so the decompressed sidecars are byte-identical; tools/check_convert.sh
// verifies that against the Python. The Python's validation and buffer-size cross-checks are not repeated here:
// they check the converter, not the ROM, and pass for the supported ROM. Any read outside a file, or any condition
// the Python reports as an error, fails the whole conversion and writes nothing.
//
// Build with -DGE_CONVERT_MAIN for a desktop command-line tool (used by the check script).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <zlib.h>

namespace {

using Bytes = std::vector<uint8_t>;
using i64 = int64_t;

struct ConvertError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const std::string& msg) { throw ConvertError(msg); }

std::string hex(i64 v) {
    char b[32];
    std::snprintf(b, sizeof b, "%#llx", static_cast<unsigned long long>(v));
    return b;
}

// ---------------------------------------------------------------- byte access (bounds-checked like Python) ---

struct Src {
    const uint8_t* p;
    i64 n;
    uint8_t at(i64 o) const {
        if (o < 0 || o >= n) fail("read outside file at " + hex(o));
        return p[o];
    }
    void need(i64 o, i64 len) const {
        if (o < 0 || o + len > n) fail("read outside file at " + hex(o));
    }
    uint32_t be32r(i64 o) const {
        need(o, 4);
        return (uint32_t(p[o]) << 24) | (uint32_t(p[o + 1]) << 16) | (uint32_t(p[o + 2]) << 8) | p[o + 3];
    }
    uint32_t be32o(i64 o) const { return be32r(o) & 0xFFFFFF; }
    int32_t bs32(i64 o) const { return static_cast<int32_t>(be32r(o)); }
    uint16_t bu16(i64 o) const {
        need(o, 2);
        return static_cast<uint16_t>((p[o] << 8) | p[o + 1]);
    }
    int16_t be16(i64 o) const { return static_cast<int16_t>(bu16(o)); }
};

// A growable output buffer: writes past the end extend it (Python bytearray slice assignment).
struct Out {
    Bytes b;
    explicit Out(i64 n) : b(static_cast<size_t>(n), 0) {}
    void ensure(i64 end) {
        if (end > static_cast<i64>(b.size())) b.resize(static_cast<size_t>(end), 0);
    }
    void u8(i64 o, uint8_t v) {
        ensure(o + 1);
        b[o] = v;
    }
    void le16(i64 o, uint32_t v) {
        ensure(o + 2);
        b[o] = v & 0xFF;
        b[o + 1] = (v >> 8) & 0xFF;
    }
    void le32(i64 o, uint32_t v) {
        ensure(o + 4);
        for (int i = 0; i < 4; i++) b[o + i] = (v >> (8 * i)) & 0xFF;
    }
    void zero(i64 o, i64 len) {
        ensure(o + len);
        std::fill(b.begin() + o, b.begin() + o + len, 0);
    }
    void copy(i64 o, const Src& s, i64 so, i64 len) {
        if (len <= 0) return;
        s.need(so, len);
        ensure(o + len);
        std::copy(s.p + so, s.p + so + len, b.begin() + o);
    }
    void bytes4(i64 o, uint8_t a, uint8_t c, uint8_t d, uint8_t e) {
        ensure(o + 4);
        b[o] = a;
        b[o + 1] = c;
        b[o + 2] = d;
        b[o + 3] = e;
    }
};

// ---------------------------------------------------------------- zlib (RZ: 0x11 0x72 + raw deflate) ---

Bytes inflate_raw(const uint8_t* p, size_t n) {
    z_stream z{};
    if (inflateInit2(&z, -15) != Z_OK) fail("inflateInit2 failed");
    Bytes out;
    uint8_t chunk[65536];
    z.next_in = const_cast<uint8_t*>(p);
    z.avail_in = static_cast<uInt>(n);
    int r;
    do {
        z.next_out = chunk;
        z.avail_out = sizeof chunk;
        r = inflate(&z, Z_NO_FLUSH);
        if (r != Z_OK && r != Z_STREAM_END) {
            inflateEnd(&z);
            fail("decompress failed");
        }
        out.insert(out.end(), chunk, chunk + (sizeof chunk - z.avail_out));
    } while (r != Z_STREAM_END && (z.avail_in > 0 || z.avail_out == 0));
    inflateEnd(&z);
    if (r != Z_STREAM_END) fail("decompress failed: truncated stream");
    return out;
}

Bytes rz_compress(const Bytes& in) {
    z_stream z{};
    if (deflateInit2(&z, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) fail("deflateInit2 failed");
    Bytes out{0x11, 0x72};
    out.resize(2 + deflateBound(&z, in.size()));
    z.next_in = const_cast<uint8_t*>(in.data());
    z.avail_in = static_cast<uInt>(in.size());
    z.next_out = out.data() + 2;
    z.avail_out = static_cast<uInt>(out.size() - 2);
    if (deflate(&z, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&z);
        fail("compress failed");
    }
    out.resize(2 + z.total_out);
    deflateEnd(&z);
    return out;
}

// ---------------------------------------------------------------- inputs ---

struct Meta {
    std::unordered_map<std::string, std::pair<i64, i64>> files;  // basename -> (rom offset, size)
    std::vector<std::string> table;                               // file_resource_table order
    std::unordered_map<std::string, std::pair<int, int>> nsnt;   // header key -> (NS, NT)
};

Meta read_meta(const std::string& path) {
    std::ifstream in(path);
    if (!in) fail("can't read " + path);
    Meta m;
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string tok;
        while (std::getline(ss, tok, '\t')) f.push_back(tok);
        if (f.empty()) continue;
        if (f[0] == "F" && f.size() == 4) m.files[f[1]] = {std::stoll(f[2]), std::stoll(f[3])};
        else if (f[0] == "T" && f.size() == 2) m.table.push_back(f[1]);
        else if (f[0] == "H" && f.size() == 4) m.nsnt[f[1]] = {std::stoi(f[2]), std::stoi(f[3])};
    }
    if (m.files.empty() || m.table.empty() || m.nsnt.empty()) fail("incomplete converter metadata in " + path);
    return m;
}

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
bool ends_with(const std::string& s, const char* p) {
    const size_t n = std::char_traits<char>::length(p);
    return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}

// A concatenated sidecar image: 16-aligned entries + manifest rows (name, offset, size).
struct Image {
    Bytes data;
    std::vector<std::tuple<std::string, i64, i64>> manifest;
    void emit(const std::string& name, const Bytes& d) {
        data.resize((data.size() + 15) & ~size_t(15), 0);
        manifest.emplace_back(name, static_cast<i64>(data.size()), static_cast<i64>(d.size()));
        data.insert(data.end(), d.begin(), d.end());
    }
};

// =====================================================================================================================
// d43_emit.py: model files -> pcmodels
// =====================================================================================================================

constexpr uint32_t MASK = 0xFFFFFF;

const std::map<int, int> N64_REC = {{1, 0x1C}, {2, 0x1C}, {3, 0x1C},  {4, 0x14},  {8, 0x10},  {9, 0x24},
                                    {10, 0x1C}, {12, 0x28}, {13, 0x20}, {15, 0x1C}, {17, 0x20}, {18, 0x08},
                                    {21, 0x14}, {22, 0x10}, {23, 0x02}, {24, 0x20}};
const std::map<int, int> PC_REC = {{1, 24},  {2, 40},  {3, 40},  {4, 40},  {8, 24}, {9, 48},  {10, 28}, {12, 48},
                                   {13, 48}, {15, 28}, {18, 16}, {21, 20}, {22, 32}, {23, 2}, {24, 64}};

struct Node {
    uint32_t op, data, parent, next, prev, child;
};

class ModelConverter {
public:
    ModelConverter(const Src& s, int ns, int nt) : src(s), D(s.n), NS(ns), NT(nt) {}

    Bytes run(const std::string& name) {
        name_ = name;
        build_nodes();
        if (nodes.empty()) fail(name + ": empty tree");
        visit_seq();          // GDL packing order (mutates the node map like the game's walk)
        placement_order();    // object placement order (reads the mutated map, as the Python does)
        layout();
        emit();
        return std::move(buf.b);
    }

private:
    const Src& src;
    const i64 D;
    const int NS, NT;
    std::string name_;
    std::unordered_map<i64, Node> nodes;
    i64 R0 = 0;
    std::vector<i64> seq;
    std::vector<std::pair<i64, int>> placed;

    struct Region {
        i64 old, osz, nw;
    };
    std::vector<Region> regions;
    std::vector<Region> blob_regions;
    std::unordered_map<i64, i64> node_newoff, rec_newoff, gdl_newoff;
    std::vector<std::pair<i64, i64>> vtx_regions;
    std::vector<i64> zero_vtx;
    std::unordered_set<i64> op24_is_collision;
    std::vector<std::pair<i64, i64>> op24_pointusage;  // insertion-ordered dict
    std::vector<i64> unvisited, all_gdls;
    i64 dstpos = 0;
    std::vector<i64> rold;
    Out buf{0};

    Node& node_at(i64 o) {
        auto it = nodes.find(o);
        if (it == nodes.end()) fail(name_ + ": node " + hex(o) + " not in the node map");
        return it->second;
    }

    void build_nodes() {
        R0 = 4 * NS + 12 * NT;
        std::vector<i64> stack{R0};
        while (!stack.empty()) {
            const i64 o = stack.back();
            stack.pop_back();
            if (nodes.count(o) || o >= D) continue;
            Node n{};
            n.op = src.bu16(o) & 0xff;
            n.data = src.be32o(o + 4);
            n.parent = src.be32o(o + 8);
            n.next = src.be32o(o + 0xC);
            n.prev = src.be32o(o + 0x10);
            n.child = src.be32o(o + 0x14);
            nodes[o] = n;
            if (n.op == 8) {
                const uint32_t q = src.be32o(n.data + 8);
                if (q) stack.push_back(q);
            } else if (n.op == 18) {
                const uint32_t q = src.be32o(n.data + 0);
                if (q) stack.push_back(q);
            } else if (n.child) {
                stack.push_back(n.child);
            }
            if (n.next) stack.push_back(n.next);
        }
    }

    std::pair<uint32_t, uint32_t> gdl_of(const Node& n) const {
        if (n.op == 4 || n.op == 24) return {src.be32o(n.data + 0), src.be32o(n.data + 4)};
        if (n.op == 22) return {src.be32o(n.data + 8), 0};
        return {0, 0};
    }

    void visit_seq() {
        i64 node = R0;
        i64 prev_node = -1;  // None
        i64 prev_gdl = 0;
        int guard = 0;
        while (node && guard < 200000) {
            guard++;
            Node& n = node_at(node);
            i64 gdl = 0;
            if (n.op == 4 || n.op == 22 || n.op == 24) {
                auto [p, s] = gdl_of(n);
                if (node != prev_node) gdl = p;
                else if (s && s != prev_gdl) gdl = s;
            } else if (n.op == 8) {
                n.child = src.be32o(n.data + 8);
            } else if (n.op == 18) {
                n.child = src.be32o(n.data + 0);
            } else if (n.op == 9) {
                const uint32_t node1 = src.be32o(n.data + 0x18), node2 = src.be32o(n.data + 0x1C);
                if (node1) {
                    n.child = node1;
                    node_at(node1).prev = 0;
                    i64 loop = node1;
                    while (node_at(loop).next && node_at(loop).next != node2) loop = node_at(loop).next;
                    node_at(loop).next = node2;
                    if (node2) {
                        node_at(node2).prev = static_cast<uint32_t>(loop);
                        loop = node2;
                        while (node_at(loop).next && node_at(loop).next != node1) loop = node_at(loop).next;
                        node_at(loop).next = 0;
                    }
                } else {
                    n.child = node2;
                    if (node2) node_at(node2).prev = 0;
                }
            }
            if (gdl) {
                seq.push_back(gdl);
                prev_node = node;
                prev_gdl = gdl;
                continue;
            }
            prev_node = -1;
            if (n.child) {
                node = n.child;
            } else {
                while (node) {
                    const uint32_t nn = node_at(node).next;
                    if (nn) {
                        node = nn;
                        break;
                    }
                    node = node_at(node).parent;
                }
            }
        }
    }

    void placement_order() {
        std::unordered_set<i64> seen;
        std::vector<i64> stack{R0};
        while (!stack.empty()) {
            const i64 o = stack.back();
            stack.pop_back();
            if (seen.count(o) || o >= src.n) continue;
            seen.insert(o);
            const Node& n = node_at(o);
            placed.emplace_back(o, static_cast<int>(n.op));
            if (n.op == 8) {
                const uint32_t a = src.be32o(n.data + 8);
                if (a) stack.push_back(a);
            } else if (n.op == 18) {
                const uint32_t c = src.be32o(n.data + 0);
                if (c) stack.push_back(c);
            } else if (n.child) {
                stack.push_back(n.child);
            }
            if (n.next) stack.push_back(n.next);
        }
    }

    i64 add_region(i64 old, i64 osz, i64 cur) {
        regions.push_back({old, osz, cur});
        return cur + osz;
    }

    i64 gdl_end(i64 g) const {
        i64 o = g;
        while (o + 8 <= D) {
            if ((src.be32r(o) >> 24) == 0xB8) return o + 8;
            o += 8;
        }
        return D;
    }

    void pack_gdl(i64 g) {
        const i64 end = gdl_end(g);
        const i64 span = end - g;
        if (span % 8) fail(name_ + ": GDL " + hex(g) + " not 8B aligned end");
        const i64 nslots = span / 8;
        gdl_newoff[g] = dstpos;
        dstpos += 16 * nslots;
        regions.push_back({g, 16 * nslots, gdl_newoff[g]});
    }

    void layout() {
        for (int i = 0; i < NS; i++) dstpos = add_region(4 * i, 8, dstpos);
        for (int i = 0; i < NT; i++) dstpos = add_region(4 * NS + 12 * i, 12, dstpos);

        for (auto [no, op] : placed) {
            const i64 data = src.be32o(no + 4);
            dstpos = add_region(no, 48, dstpos);
            node_newoff[no] = dstpos - 48;
            if (!N64_REC.count(op)) fail(name_ + ": unknown opcode " + std::to_string(op) + " at " + hex(no));
            const i64 psz = PC_REC.at(op);
            rec_newoff[data] = dstpos;
            dstpos = add_region(data, psz, dstpos);
            if (op == 4) {
                const i64 nv = src.bu16(data + 0x10), vo = src.be32o(data + 0xC);
                if (vo) {
                    if (nv == 0) {
                        zero_vtx.push_back(vo);
                    } else {
                        vtx_regions.emplace_back(vo, nv);
                        dstpos = add_region(vo, 16 * nv, dstpos);
                    }
                }
            } else if (op == 24) {
                const i64 nv = src.be16(data + 0xC), ncv = src.be16(data + 0xE);
                const i64 vo = src.be32o(data + 8), cvo = src.be32o(data + 0x10), puo = src.be32o(data + 0x14);
                if (nv && vo) {
                    vtx_regions.emplace_back(vo, nv);
                    dstpos = add_region(vo, 16 * nv, dstpos);
                }
                if (ncv && cvo) {
                    op24_is_collision.insert(cvo);
                    vtx_regions.emplace_back(cvo, ncv);
                    dstpos = add_region(cvo, 16 * ncv, dstpos);
                }
                if (nv && puo) {
                    dstpos = add_region(puo, 2 * nv, dstpos);
                    auto it = std::find_if(op24_pointusage.begin(), op24_pointusage.end(),
                                           [&](auto& e) { return e.first == puo; });
                    if (it == op24_pointusage.end()) op24_pointusage.emplace_back(puo, nv);
                    else it->second = nv;
                }
            } else if (op == 22) {
                const i64 nv = src.bs32(data), vo = src.be32o(data + 4);
                if (nv && vo) {
                    vtx_regions.emplace_back(vo, 4 * nv);
                    dstpos = add_region(vo, 16 * 4 * nv, dstpos);
                }
            }
        }

        // every record-referenced GDL, first-seen order
        std::vector<i64> ref_gdls;
        std::unordered_set<i64> ref_set;
        auto add_ref = [&](i64 q) {
            if (q && ref_set.insert(q).second) ref_gdls.push_back(q);
        };
        for (auto [no, op] : placed) {
            const i64 data = src.be32o(no + 4);
            if (op == 4 || op == 24) {
                add_ref(src.be32o(data + 0));
                add_ref(src.be32o(data + 4));
            } else if (op == 22) {
                add_ref(src.be32o(data + 8));
            }
        }
        const std::unordered_set<i64> visited(seq.begin(), seq.end());
        for (i64 g : ref_gdls)
            if (!visited.count(g)) unvisited.push_back(g);
        std::sort(unvisited.begin(), unvisited.end());
        all_gdls = unvisited;
        all_gdls.insert(all_gdls.end(), seq.begin(), seq.end());

        std::set<i64> obj_offs_now;
        for (auto& p : placed) obj_offs_now.insert(p.first);
        for (auto& r : rec_newoff) obj_offs_now.insert(r.first);
        for (auto& v : vtx_regions) obj_offs_now.insert(v.first);
        for (i64 g : all_gdls) obj_offs_now.insert(g);
        for (i64 vo : zero_vtx) {
            auto it = obj_offs_now.upper_bound(vo);
            const i64 nxt = it == obj_offs_now.end() ? D : *it;
            const i64 sz = nxt - vo;
            if (sz <= 0 || sz % 16) fail(name_ + ": zero-vtx array " + hex(vo) + " bad size " + hex(sz));
            vtx_regions.emplace_back(vo, sz / 16);
            dstpos = add_region(vo, sz, dstpos);
        }

        // embedded image blobs: G_SETTIMG seg-5 targets, each spanning to the next placed object, merged
        std::set<i64> blob_targets;
        for (i64 g : all_gdls) {
            i64 o = g;
            while (o + 8 <= D) {
                const uint32_t c = src.be32r(o) >> 24;
                if (c == 0xB8) break;
                if (c == 0xFD && (src.be32r(o + 4) >> 24) == 5) blob_targets.insert(src.be32r(o + 4) & MASK);
                o += 8;
            }
        }
        std::set<i64> placed_offs;
        for (auto& r : regions) placed_offs.insert(r.old);
        std::vector<std::pair<i64, i64>> merged;
        for (i64 t : blob_targets) {
            auto it = placed_offs.upper_bound(t);
            const i64 end = it == placed_offs.end() ? D : *it;
            if (!merged.empty() && t <= merged.back().second) merged.back().second = std::max(merged.back().second, end);
            else merged.emplace_back(t, end);
        }
        for (auto [t, e] : merged) {
            const i64 n = dstpos;
            dstpos = add_region(t, e - t, dstpos);
            blob_regions.push_back({t, e - t, n});
        }

        for (i64 g : unvisited) pack_gdl(g);
        for (i64 g : seq) pack_gdl(g);

        // remap index: regions sorted like Python tuples (old, osz, new)
        std::sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) {
            return std::tie(a.old, a.osz, a.nw) < std::tie(b.old, b.osz, b.nw);
        });
        rold.clear();
        for (auto& r : regions) rold.push_back(r.old);
    }

    // -1 = None
    i64 remap(i64 off) const {
        if (off == 0) return 0;
        const auto it = std::upper_bound(rold.begin(), rold.end(), off);
        if (it == rold.begin()) return -1;
        const Region& r = regions[static_cast<size_t>(it - rold.begin() - 1)];
        if (r.old <= off && off < r.old + r.osz) return r.nw + (off - r.old);
        return -1;
    }

    void put_f32(i64 o, i64 doff) {
        src.need(doff, 4);
        buf.bytes4(o, src.p[doff + 3], src.p[doff + 2], src.p[doff + 1], src.p[doff]);
    }
    void put_ptr(i64 o, i64 old_off) {
        if (!old_off) return;
        i64 n = remap(old_off);
        if (n < 0) fail(name_ + ": ptr " + hex(old_off) + " not in region map");
        buf.le32(o, 0x05000000u | (static_cast<uint32_t>(n) & MASK));
        buf.zero(o + 4, 4);
    }
    void put_gdl(i64 o, i64 g) {
        auto it = gdl_newoff.find(g);
        if (it == gdl_newoff.end()) fail(name_ + ": GDL " + hex(g) + " not packed");
        buf.le32(o, 0x05000000u | (static_cast<uint32_t>(it->second) & MASK));
    }

    void emit() {
        buf = Out(dstpos);
        for (int i = 0; i < NS; i++) put_ptr(8 * i, src.be32o(4 * i));
        for (int i = 0; i < NT; i++) {
            const i64 doff = 4 * NS + 12 * i, npos = 8 * NS + 12 * i;
            const uint32_t tid = src.be32r(doff);
            buf.le32(npos, tid);
            buf.copy(npos + 4, src, doff + 4, 8);
            if ((tid >> 24) == 5 && remap(tid & MASK) < 0)
                fail(name_ + ": texconfig seg-5 target " + hex(tid & MASK) + " not in a blob region");
        }

        for (auto [no, op] : placed) {
            const i64 npos = node_newoff.at(no);
            buf.le16(npos, src.bu16(no));
            put_ptr(npos + 8, src.be32o(no + 4));
            put_ptr(npos + 16, src.be32o(no + 8));
            put_ptr(npos + 24, src.be32o(no + 0xC));
            put_ptr(npos + 32, src.be32o(no + 0x10));
            put_ptr(npos + 40, src.be32o(no + 0x14));

            const i64 d = src.be32o(no + 4);
            const i64 r = rec_newoff.at(d);
            switch (op) {
            case 1:
                buf.le16(r, src.bu16(d));
                buf.le16(r + 2, src.bu16(d + 2));
                put_ptr(r + 8, src.be32o(d + 4));
                buf.le16(r + 16, src.bu16(d + 8));
                buf.le16(r + 18, src.bu16(d + 0xA));
                buf.le16(r + 20, src.bu16(d + 0xC));
                break;
            case 2:
            case 3:
                for (int i = 0; i < 3; i++) put_f32(r + 4 * i, d + 4 * i);
                buf.le16(r + 12, src.bu16(d + 0xC));
                for (int i = 0; i < 3; i++) buf.le16(r + 14 + 2 * i, src.bu16(d + 0xE + 2 * i));
                put_ptr(r + 24, src.be32o(d + 0x14));
                put_f32(r + 32, d + 0x18);
                break;
            case 4: {
                const i64 p = src.be32o(d), s = src.be32o(d + 4);
                if (p) put_gdl(r, p);
                if (s) put_gdl(r + 8, s);
                put_ptr(r + 24, src.be32o(d + 0xC));
                buf.le16(r + 32, src.bu16(d + 0x10));
                buf.u8(r + 34, src.at(d + 0x12));
                break;
            }
            case 8:
                put_f32(r, d);
                put_f32(r + 4, d + 4);
                put_ptr(r + 8, src.be32o(d + 8));
                buf.le16(r + 16, src.bu16(d + 0xC));
                break;
            case 9:
                for (int i = 0; i < 3; i++) put_f32(r + 4 * i, d + 4 * i);
                for (int i = 0; i < 3; i++) put_f32(r + 12 + 4 * i, d + 0xC + 4 * i);
                put_ptr(r + 24, src.be32o(d + 0x18));
                put_ptr(r + 32, src.be32o(d + 0x1C));
                buf.le16(r + 42, src.bu16(d + 0x22));
                break;
            case 10:
                buf.le32(r, src.be32r(d));
                for (int i = 0; i < 6; i++) put_f32(r + 4 + 4 * i, d + 4 + 4 * i);
                break;
            case 12:
                for (int i = 0; i < 3; i++) put_f32(r + 4 * i, d + 4 * i);
                for (int i = 0; i < 3; i++) put_f32(r + 12 + 4 * i, d + 0xC + 4 * i);
                put_ptr(r + 24, src.be32o(d + 0x18));
                put_f32(r + 32, d + 0x1C);
                buf.le16(r + 36, src.bu16(d + 0x20));
                break;
            case 13:
                for (int i = 0; i < 2; i++) put_f32(r + 4 * i, d + 4 * i);
                for (int i = 0; i < 2; i++) put_f32(r + 8 + 4 * i, d + 8 + 4 * i);
                put_ptr(r + 16, src.be32o(d + 0x10));
                put_ptr(r + 24, src.be32o(d + 0x14));
                put_f32(r + 32, d + 0x18);
                break;
            case 15:
                for (int i = 0; i < 3; i++) put_f32(r + 4 * i, d + 4 * i);
                for (int i = 0; i < 3; i++) put_f32(r + 12 + 4 * i, d + 0xC + 4 * i);
                put_f32(r + 24, d + 0x18);
                break;
            case 18:
                put_ptr(r, src.be32o(d));
                buf.le16(r + 8, src.bu16(d + 4));
                break;
            case 21:
                for (int i = 0; i < 3; i++) put_f32(r + 4 * i, d + 4 * i);
                buf.le16(r + 12, src.bu16(d + 0xC));
                buf.le16(r + 14, src.bu16(d + 0xE));
                put_f32(r + 16, d + 0x10);
                break;
            case 22: {
                buf.le32(r, src.be32r(d));
                put_ptr(r + 8, src.be32o(d + 4));
                const i64 p = src.be32o(d + 8);
                if (p) put_gdl(r + 16, p);
                break;
            }
            case 23:
                buf.le16(r, src.bu16(d));
                break;
            case 24: {
                const i64 p = src.be32o(d), s = src.be32o(d + 4);
                if (p) put_gdl(r, p);
                if (s) put_gdl(r + 8, s);
                put_ptr(r + 16, src.be32o(d + 8));
                buf.le16(r + 24, src.bu16(d + 0xC));
                buf.le16(r + 26, src.bu16(d + 0xE));
                put_ptr(r + 32, src.be32o(d + 0x10));
                put_ptr(r + 40, src.be32o(d + 0x14));
                buf.le16(r + 48, src.bu16(d + 0x18));
                buf.le16(r + 50, src.bu16(d + 0x1A));
                break;
            }
            default:
                break;
            }
        }

        // vertex arrays
        for (auto [vo, nv] : vtx_regions) {
            const i64 npos = remap(vo);
            if (npos < 0) fail(name_ + ": vertex array " + hex(vo) + " not in region map");
            const bool collision = op24_is_collision.count(vo) != 0;
            for (i64 i = 0; i < nv; i++) {
                const i64 o = vo + 16 * i, q = npos + 16 * i;
                for (int k = 0; k < 4; k++) buf.le16(q + 2 * k, src.bu16(o + 2 * k));
                if (collision) {
                    const uint32_t v = src.be32r(o + 8);
                    if (v == 0) {
                    } else if ((v >> 24) == 5) {
                        const i64 n = remap(v & MASK);
                        if (n < 0) fail(name_ + ": LinkedTo " + hex(v) + " not in map");
                        buf.le32(q + 8, 0x05000000u | (static_cast<uint32_t>(n) & MASK));
                    } else {
                        fail(name_ + ": LinkedTo " + hex(v) + " neither null nor seg-5");
                    }
                    buf.le16(q + 12, src.bu16(o + 0xC));
                    buf.le16(q + 14, src.bu16(o + 0xE));
                } else {
                    buf.le16(q + 8, src.bu16(o + 8));
                    buf.le16(q + 10, src.bu16(o + 0xA));
                    buf.copy(q + 12, src, o + 0xC, 4);
                }
            }
        }

        // PointUsage[]: s16 index chains, plain byteswap
        for (auto [puo, punv] : op24_pointusage) {
            const i64 pnpos = remap(puo);
            if (pnpos < 0) fail(name_ + ": PointUsage " + hex(puo) + " not in region map");
            for (i64 k = 0; k < punv; k++) buf.le16(pnpos + 2 * k, src.bu16(puo + 2 * k));
        }

        // GDLs: 16B slots {u64 w0; u64 w1}, seg-5 w1 of G_VTX / G_SETTIMG remapped
        for (i64 g : all_gdls) {
            const i64 end = gdl_end(g);
            const i64 npos = gdl_newoff.at(g);
            i64 j = 0;
            for (i64 o = g; o + 8 <= end; o += 8) {
                const uint32_t w0 = src.be32r(o);
                uint32_t w1 = src.be32r(o + 4);
                const i64 q = npos + 16 * j;
                const uint32_t c = w0 >> 24;
                if ((c == 0x04 || c == 0xFD) && (w1 >> 24) == 5) {
                    const i64 n = remap(w1 & MASK);
                    if (n < 0) fail(name_ + ": GDL " + hex(g) + " w1 " + hex(w1) + " not in map");
                    w1 = 0x05000000u | (static_cast<uint32_t>(n) & MASK);
                }
                buf.le32(q, w0);
                buf.le32(q + 4, 0);
                buf.le32(q + 8, w1);
                buf.le32(q + 12, 0);
                if (c == 0xB8) break;
                j++;
            }
        }

        // embedded image blobs: raw pixel copy
        for (auto& b : blob_regions) buf.copy(b.nw, src, b.old, b.osz);
    }
};

void convert_models(const Bytes& rom, const Meta& meta, Image& img) {
    auto find_row = [&](const std::string& name, std::string& key) -> const std::pair<i64, i64>* {
        for (const std::string& cand : {name, name.substr(1), name.substr(0, name.size() - 1),
                                        name.size() >= 2 ? name.substr(1, name.size() - 2) : std::string()}) {
            auto it = meta.files.find(cand);
            if (it != meta.files.end()) {
                key = cand;
                return &it->second;
            }
        }
        return nullptr;
    };
    std::vector<std::string> model_names;
    for (const auto& n : meta.table)
        if (!n.empty() && std::string("CGP").find(n[0]) != std::string::npos && ends_with(n, "Z") &&
            n.find("_stan") == std::string::npos)
            model_names.push_back(n);

    std::unordered_map<std::string, Bytes> results;
    for (const auto& name : model_names) {
        std::string key;
        const auto* row = find_row(name, key);
        if (!row) fail(name + ": no filelist row");
        const auto [addr, size] = *row;
        if (addr + 2 > static_cast<i64>(rom.size()) || size < 2) fail(name + ": outside the ROM");
        const i64 avail = std::min<i64>(size, static_cast<i64>(rom.size()) - addr);
        const Bytes dec = inflate_raw(rom.data() + addr + 2, static_cast<size_t>(avail - 2));
        const std::pair<int, int>* hdr = nullptr;
        for (const std::string& cand : {key, key.substr(1), key.substr(0, key.size() - 1),
                                        key.size() >= 2 ? key.substr(1, key.size() - 2) : std::string()}) {
            auto it = meta.nsnt.find(cand);
            if (it != meta.nsnt.end()) {
                hdr = &it->second;
                break;
            }
        }
        if (!hdr) fail(name + ": no header data");
        const Src src{dec.data(), static_cast<i64>(dec.size())};
        ModelConverter mc(src, hdr->first, hdr->second);
        results[name] = rz_compress(mc.run(name));
    }
    for (const auto& name : model_names) img.emit(name, results.at(name));
}

// =====================================================================================================================
// d69_emit.py: bg/*.seg (raw) + Tbg_*_stanZ (RZ) -> pccg
// =====================================================================================================================

Bytes convert_seg(const std::string& name, const Src& src) {
    const i64 D = src.n;
    if (D < 0x40) fail(name + ": too small");
    const uint32_t h0 = src.be32r(0);
    const i64 h1 = src.be32o(4), h2 = src.be32o(8), h3 = src.be32o(12), h4 = src.be32o(16);
    if (h0 != 0) fail(name + ": header word0 != 0");
    if (h4 != 0) fail(name + ": header word4 != 0");
    if (h2 == 0 || h2 <= h1) fail(name + ": portal offset <= room offset");
    const i64 room_end = (h3 && h3 > h1 && h3 < h2) ? h3 : h2;
    if ((room_end - h1) % 24 != 0) fail(name + ": room table extent not a multiple of 24");

    i64 o = h2;
    i64 n_portals = 0;
    std::vector<i64> portal_targets;
    bool terminated = false;
    while (o + 8 <= D) {
        const i64 v = src.be32o(o);
        if (v == 0) {
            terminated = true;
            break;
        }
        portal_targets.push_back(v);
        n_portals++;
        o += 8;
    }
    if (!terminated) fail(name + ": portal table terminator not found before EOF");
    const i64 portal_table_end = o + 8;
    const i64 portal_delta = 8 * (n_portals + 1);

    constexpr int ENVIRONMENTDATA_ALT = 100;
    std::vector<i64> alt_targets;
    if (h3 && room_end == h3) {
        for (i64 o2 = h3; o2 < h2;) {
            const uint8_t etype = src.at(o2);
            const int32_t data = src.bs32(o2 + 4);
            if (etype == ENVIRONMENTDATA_ALT && data) alt_targets.push_back(static_cast<uint32_t>(data) & MASK);
            o2 += 8;
            if (etype == 0) break;
        }
    }

    std::set<i64> targets(portal_targets.begin(), portal_targets.end());
    targets.insert(alt_targets.begin(), alt_targets.end());
    struct Blob {
        i64 start, size;
        bool record;
    };
    std::vector<Blob> blobs;
    i64 cursor = portal_table_end;
    for (i64 t : targets) {
        if (t < cursor) fail(name + ": portal target overlaps previous region");
        if (t > cursor) blobs.push_back({cursor, t - cursor, false});
        const int numPoints = src.at(t);
        if (numPoints == 0 || numPoints > 20) fail(name + ": portal target numPoints implausible");
        const i64 recsize = 4 + 12 * numPoints;
        if (t + recsize > D) fail(name + ": portal target overruns EOF");
        blobs.push_back({t, recsize, true});
        cursor = t + recsize;
    }
    if (cursor < D) blobs.push_back({cursor, D - cursor, false});

    Out out(D + portal_delta);
    out.le32(0, h0);
    out.le32(4, 0x0F000000u | static_cast<uint32_t>(h1));
    out.le32(8, 0x0F000000u | static_cast<uint32_t>(h2));
    out.le32(12, h3 ? (0x0F000000u | static_cast<uint32_t>(h3)) : 0);
    out.le32(16, 0);
    out.copy(20, src, 20, 0x40 - 20);

    for (i64 o2 = h1; o2 < room_end; o2 += 24) {
        for (int w = 0; w < 3; w++) {
            const i64 v = src.be32o(o2 + 4 * w);
            const uint32_t nv = v ? (0x0F000000u | ((v + portal_delta) & MASK)) : 0;
            out.le32(o2 + 4 * w, nv);
        }
        for (int w = 0; w < 3; w++) out.le32(o2 + 12 + 4 * w, src.be32r(o2 + 12 + 4 * w));
    }

    if (h3 && room_end == h3) {
        for (i64 o2 = h3; o2 < h2;) {
            const uint8_t etype = src.at(o2);
            out.u8(o2, etype);
            out.copy(o2 + 1, src, o2 + 1, 3);
            int32_t data = src.bs32(o2 + 4);
            if (etype == ENVIRONMENTDATA_ALT && data) data = static_cast<int32_t>(data + portal_delta);
            out.le32(o2 + 4, static_cast<uint32_t>(data));
            o2 += 8;
            if (etype == 0) break;
        }
    }

    i64 o2 = h2, pc_pos = h2;
    for (i64 idx = 0; idx <= n_portals; idx++) {
        const i64 off_val = src.be32o(o2);
        if (off_val) out.le32(pc_pos, 0x0F000000u | ((off_val + portal_delta) & MASK));
        else out.le32(pc_pos, 0);
        out.le32(pc_pos + 4, 0);
        out.copy(pc_pos + 8, src, o2 + 4, 4);
        out.le32(pc_pos + 12, 0);
        o2 += 8;
        pc_pos += 16;
    }

    for (const Blob& b : blobs) {
        const i64 dst = b.start + portal_delta;
        if (!b.record) {
            out.copy(dst, src, b.start, b.size);
            continue;
        }
        out.copy(dst, src, b.start, 4);
        for (i64 k = 0; k < (b.size - 4) / 4; k++) out.le32(dst + 4 + 4 * k, src.be32r(b.start + 4 + 4 * k));
    }
    return std::move(out.b);
}

Bytes convert_stan(const std::string& name, const Src& src) {
    static const int TILESIZES[] = {0x20, 0x20, 0x20, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0};
    const i64 D = src.n;
    if (D < 8) fail(name + ": too small");
    const int32_t stanfile = src.bs32(0);
    std::vector<uint32_t> offs;
    i64 o = 4;
    bool terminated = false;
    while (o + 4 <= D) {
        const uint32_t v = src.be32r(o);
        offs.push_back(v);
        o += 4;
        if (v == 0) {
            terminated = true;
            break;
        }
    }
    if (!terminated) fail(name + ": room-offset array terminator not found");
    const i64 N = static_cast<i64>(offs.size()) - 1;
    const i64 old_array_end = o;
    const i64 array_delta = 4 * (N + 2);
    const i64 new_tiledata_start = old_array_end + array_delta;

    Out out(D + array_delta);
    out.le32(0, static_cast<uint32_t>(stanfile));
    out.le32(4, 0);
    i64 p = 8;
    for (uint32_t v : offs) {
        out.le32(p, v ? static_cast<uint32_t>(v + array_delta) : 0);
        out.le32(p + 4, 0);
        p += 8;
    }

    i64 src_o = old_array_end, dst_o = new_tiledata_start;
    terminated = false;
    while (src_o + 4 <= D) {
        if (src.be32r(src_o) == 0) {
            out.le32(dst_o, 0);
            terminated = true;
            break;
        }
        out.bytes4(dst_o, src.at(src_o + 1), src.at(src_o), src.at(src_o + 2), src.at(src_o + 3));
        const uint16_t midhalf = src.bu16(src_o + 4), tailhalf = src.bu16(src_o + 6);
        out.le16(dst_o + 4, midhalf);
        out.le16(dst_o + 6, tailhalf);
        const int pointCount = (static_cast<int16_t>(tailhalf) >> 12) & 0xF;
        const int sz = TILESIZES[pointCount];
        if (sz == 0) fail(name + ": bad pointCount");
        if (src_o + sz > D) fail(name + ": tile overruns EOF");
        for (int i = 0; i < pointCount; i++) {
            const i64 po = src_o + 8 + 8 * i, qo = dst_o + 8 + 8 * i;
            for (int k = 0; k < 4; k++) out.le16(qo + 2 * k, src.bu16(po + 2 * k));
        }
        src_o += sz;
        dst_o += sz;
    }
    if (!terminated) fail(name + ": ran off EOF without an all-zero EOF sentinel");
    const i64 tail_src_o = src_o + 4, tail_dst_o = dst_o + 4;
    if (tail_src_o < D) out.copy(tail_dst_o, src, tail_src_o, D - tail_src_o);
    return std::move(out.b);
}

// =====================================================================================================================
// d88_propdefs.py: the propDefs polymorphic record stream
// =====================================================================================================================

const std::map<int, int> PROPDEF_N64_WORDS = {
    {1, 64},  {2, 2},  {3, 32},  {4, 33},  {5, 32},  {6, 59},  {7, 33},  {8, 34},  {9, 7},   {10, 64},
    {11, 149}, {12, 32}, {13, 54}, {14, 3},  {17, 32}, {18, 3},  {19, 4},  {20, 45}, {21, 34}, {22, 4},
    {23, 4},  {24, 1},  {25, 2},  {26, 2},  {27, 2},  {28, 2},  {29, 2},  {30, 4},  {31, 1},  {32, 4},
    {33, 5},  {34, 3},  {35, 4},  {36, 32}, {37, 10}, {38, 4},  {39, 44}, {40, 45}, {42, 32}, {43, 32},
    {44, 5},  {45, 56}, {46, 7},  {47, 37}, {48, 1}};
const std::map<int, int> PROPDEF_PC_BYTES = {
    {1, 296}, {2, 8},   {3, 144}, {5, 144}, {12, 144}, {17, 144}, {36, 144}, {42, 144}, {43, 144}, {4, 152},
    {6, 272}, {7, 152}, {8, 160}, {9, 32},  {10, 288}, {11, 664}, {13, 248}, {14, 32},  {18, 12},  {19, 32},
    {20, 200}, {21, 152}, {22, 24}, {23, 16}, {24, 4},  {25, 8},   {26, 8},   {27, 8},   {28, 8},   {29, 8},
    {30, 24}, {31, 4},  {32, 24}, {33, 24}, {34, 12},  {35, 24},  {37, 48},  {38, 32},  {39, 208}, {40, 208},
    {44, 40}, {45, 248}, {46, 28}, {47, 168}, {48, 4}};

constexpr int OBJ_N64_WORDS = 32;
constexpr int OBJ_PC_BYTES = 144;

struct TailDesc {
    std::set<int> ptr, hh;
};
const std::map<int, TailDesc> OBJ_TAIL_DESC = {
    {47, {{}, {}}},
    {39, {{41, 43}, {33}}},
    {40, {{43, 44}, {33}}},
    {45, {{32}, {}}},
    {13, {{49, 50, 51}, {}}},
    {20, {{}, {32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44}}},
};
const std::map<int, std::set<int>> OBJ_ID_WORDS = {{39, {32}}, {40, {32}}};

class PropDefs {
public:
    explicit PropDefs(const Src& s) : src(s) {}

    Bytes convert_stream(i64 start, i64 end) {
        Bytes out;
        i64 o = start;
        while (o < end) {
            if (o + 4 > end) fail("propDefs: truncated header at " + hex(o));
            const int t = src.at(o + 3);
            auto it = PROPDEF_N64_WORDS.find(t);
            if (it == PROPDEF_N64_WORDS.end()) fail("propDefs: unknown record type " + std::to_string(t) + " at " + hex(o));
            const int n64w = it->second;
            if (o + 4 * n64w > end) fail("propDefs: record overruns region end");
            const Bytes rec = convert_record(o, t);
            out.insert(out.end(), rec.begin(), rec.end());
            o += 4 * n64w;
            if (t == 48) break;
        }
        if (o != end) {
            const i64 pad = end - o;
            bool nonzero = false;
            for (i64 k = o; k < end; k++) nonzero |= src.at(k) != 0;
            if (pad < 0 || pad >= 16 || nonzero) fail("propDefs: walk ended at " + hex(o) + ", region end " + hex(end));
            for (i64 k = o; k < end; k++) out.push_back(src.at(k));
        }
        return out;
    }

private:
    const Src& src;

    // word helpers: write the 4 bytes of N64 word w (src offset so+4*w) at pc offset in a transformed order
    void bswap(Out& out, i64 pc, i64 so) { out.bytes4(pc, src.at(so + 3), src.at(so + 2), src.at(so + 1), src.at(so)); }
    void hdr(Out& out, i64 pc, i64 so) { out.bytes4(pc, src.at(so + 1), src.at(so), src.at(so + 2), src.at(so + 3)); }
    void hh(Out& out, i64 pc, i64 so) { out.bytes4(pc, src.at(so + 1), src.at(so), src.at(so + 3), src.at(so + 2)); }
    void verb(Out& out, i64 pc, i64 so) { out.bytes4(pc, src.at(so), src.at(so + 1), src.at(so + 2), src.at(so + 3)); }

    void object_prefix(Out& out, i64 so) {
        i64 pc = 0;
        for (int i = 0; i < OBJ_N64_WORDS; i++) {
            const i64 w = so + 4 * i;
            if (i == 4 || i == 5 || i == 26 || i == 27) {
                pc = (pc + 7) & ~i64(7);
                pc += 8;
            } else if (i == 0) {
                hdr(out, pc, w), pc += 4;
            } else if (i == 1) {
                hh(out, pc, w), pc += 4;
            } else if (i == 30 || i == 31) {
                verb(out, pc, w), pc += 4;
            } else {
                bswap(out, pc, w), pc += 4;
            }
        }
    }

    Bytes convert_record(i64 so, int t) {
        const int n64w = PROPDEF_N64_WORDS.at(t);
        Out out(PROPDEF_PC_BYTES.at(t));
        auto word = [&](int i) { return so + 4 * i; };
        static const std::set<int> OBJ_TYPES = {3, 5, 12, 17, 36, 42, 43};

        if (OBJ_TYPES.count(t)) {
            object_prefix(out, so);
        } else if (t == 1) {  // DOOR
            object_prefix(out, so);
            i64 pc = OBJ_PC_BYTES;
            for (int i = OBJ_N64_WORDS; i < n64w; i++) {
                const i64 w = word(i);
                if (i == 50 || i == 51 || i == 61 || i == 62) {
                    pc = (pc + 7) & ~i64(7);
                    pc += 8;
                } else if (i == 38) {
                    hh(out, pc, w), pc += 4;
                } else if (i == 47) {
                    out.bytes4(pc, src.at(w), src.at(w + 1), src.at(w + 3), src.at(w + 2)), pc += 4;
                } else if (i == 49) {
                    out.bytes4(pc, src.at(w + 1), src.at(w), src.at(w + 2), src.at(w + 3)), pc += 4;
                } else {
                    bswap(out, pc, w), pc += 4;
                }
            }
        } else if (t == 4 || t == 21 || t == 7) {
            object_prefix(out, so);
            i64 pc = OBJ_PC_BYTES;
            for (int i = OBJ_N64_WORDS; i < n64w; i++) bswap(out, pc, word(i)), pc += 4;
        } else if (t == 8) {
            object_prefix(out, so);
            const i64 w = word(32);
            out.bytes4(144, src.at(w), src.at(w + 1), src.at(w + 3), src.at(w + 2));
        } else if (t == 10 || t == 11) {
            object_prefix(out, so);
            const int nmon = t == 10 ? 1 : 4;
            i64 pc = OBJ_PC_BYTES + 128 * nmon;
            for (int i = OBJ_N64_WORDS + 29 * nmon; i < n64w; i++) {
                if (t == 11) verb(out, pc, word(i));
                else bswap(out, pc, word(i));
                pc += 4;
            }
        } else if (t == 6) {
            object_prefix(out, so);
            bswap(out, 144, word(32));
            i64 pc = 212;
            for (int i = 49; i < n64w; i++) bswap(out, pc, word(i)), pc += 4;
        } else if (t == 22) {
            hdr(out, 0, word(0));
            hh(out, 4, word(1));
        } else if (t == 37) {
            hdr(out, 0, word(0));
            for (int i = 1; i < 9; i++) bswap(out, 4 * i, word(i));
        } else if (t == 14 || t == 19 || t == 38 || t == 44) {
            const int nidx = t == 44 ? 3 : 2;
            hdr(out, 0, word(0));
            for (int k = 0; k < nidx; k++) bswap(out, 8 + 8 * k, word(k + 1));
        } else if (t == 18) {
            hdr(out, 0, word(0));
            bswap(out, 4, word(1));
            const i64 w = word(2);
            out.bytes4(8, src.at(w + 1), src.at(w), src.at(w + 2), src.at(w + 3));
        } else if (t == 9) {
            hdr(out, 0, word(0));
            for (int i = 1; i < 6; i++) hh(out, 4 * i, word(i));
        } else if (OBJ_TAIL_DESC.count(t)) {
            object_prefix(out, so);
            const TailDesc& d = OBJ_TAIL_DESC.at(t);
            static const std::set<int> none;
            auto idw = OBJ_ID_WORDS.find(t);
            const std::set<int>& ids = idw == OBJ_ID_WORDS.end() ? none : idw->second;
            i64 pc = OBJ_PC_BYTES;
            for (int i = OBJ_N64_WORDS; i < n64w; i++) {
                const i64 w = word(i);
                if (ids.count(i)) {
                    pc = (pc + 7) & ~i64(7);
                    bswap(out, pc, w);
                    pc += 8;
                } else if (d.ptr.count(i)) {
                    pc = (pc + 7) & ~i64(7);
                    pc += 8;
                } else if (d.hh.count(i)) {
                    hh(out, pc, w), pc += 4;
                } else {
                    bswap(out, pc, w), pc += 4;
                }
            }
        } else if (t == 30 || t == 32 || t == 33 || t == 35) {
            hdr(out, 0, word(0));
            for (int i = 1; i < n64w - 1; i++) bswap(out, 4 * i, word(i));
        } else {  // generic: header word + plain bswap32
            hdr(out, 0, word(0));
            for (int i = 1; i < n64w; i++) bswap(out, 4 * i, word(i));
        }
        return std::move(out.b);
    }
};

// =====================================================================================================================
// d88_emit.py: Usetup*Z / Ump_setup*Z stage setups -> appended to pccg
// =====================================================================================================================

Bytes convert_usetup(const std::string& name, const Src& src) {
    const i64 D = src.n;
    if (D < 0x28) fail(name + ": too small");
    enum Field { PATHWAYPOINTS, WAYPOINTGROUPS, INTRO, PROPDEFS, PATROLPATHS, AILISTS, PADS, BOUNDPADS, PADNAMES,
                 BOUNDPADNAMES, NFIELDS };
    i64 H[NFIELDS];
    for (int i = 0; i < NFIELDS; i++) {
        H[i] = src.be32r(4 * i);
        if (H[i] != 0 && H[i] >= D) fail(name + ": header field offset >= filesize");
    }

    struct Walk {
        std::vector<i64> starts;
        i64 term = -1;  // -1 = None
    };
    auto walk_fixed = [&](i64 off, i64 recsize, i64 term_off, bool s32neg) {
        Walk w;
        if (off == 0) return w;
        i64 o = off;
        for (int guard = 0; guard < 100000; guard++) {
            const bool term = s32neg ? src.bs32(o + term_off) < 0 : src.be32r(o + term_off) == 0;
            if (term) {
                w.term = o;
                return w;
            }
            w.starts.push_back(o);
            o += recsize;
        }
        fail(name + ": runaway walk_fixed at " + hex(off));
    };

    struct Grow {
        Field f;
        Walk w;
        i64 oldsz, newsz;
    };
    std::vector<Grow> grow = {
        {PADS, walk_fixed(H[PADS], 44, 0x24, false), 44, 56},
        {BOUNDPADS, walk_fixed(H[BOUNDPADS], 68, 0x24, false), 68, 80},
        {WAYPOINTGROUPS, walk_fixed(H[WAYPOINTGROUPS], 12, 0, false), 12, 24},
        {PATHWAYPOINTS, walk_fixed(H[PATHWAYPOINTS], 16, 0, true), 16, 24},
        {PATROLPATHS, walk_fixed(H[PATROLPATHS], 8, 0, false), 8, 16},
        {AILISTS, walk_fixed(H[AILISTS], 8, 0, false), 8, 16},
        {PADNAMES, walk_fixed(H[PADNAMES], 4, 0, false), 4, 8},
        {BOUNDPADNAMES, walk_fixed(H[BOUNDPADNAMES], 4, 0, false), 4, 8},
    };
    auto starts_of = [&](Field f) -> const std::vector<i64>& {
        for (auto& g : grow)
            if (g.f == f) return g.w.starts;
        fail("bad field");
    };

    static const std::map<int, int> INTRO_SZ = {{0, 12}, {1, 16}, {2, 16}, {3, 32}, {4, 8},
                                                {5, 8},  {6, 40}, {7, 12}, {8, 8},  {9, 4}};
    std::vector<std::pair<i64, int>> intro_recs;
    i64 intro_end = H[INTRO];
    if (H[INTRO]) {
        i64 o = H[INTRO];
        bool ended = false;
        for (int guard = 0; guard < 5000; guard++) {
            const int t = src.bs32(o);
            auto it = INTRO_SZ.find(t);
            if (it == INTRO_SZ.end()) fail(name + ": unknown intro type " + std::to_string(t) + " at " + hex(o));
            intro_recs.emplace_back(o, t);
            o += it->second;
            if (t == 9) {
                ended = true;
                break;
            }
        }
        if (!ended) fail(name + ": intro walk runaway");
        intro_end = o;
    }

    // leaf sub-regions: s32 ID arrays / C strings (insertion-ordered by start)
    enum Kind { HEADER, GROW, INTROK, PROPDEFSK, LEAF_S32, LEAF_CSTR, LEAF_AI, GAP };
    struct Leaf {
        i64 start;
        Kind kind;
        i64 end;
    };
    std::vector<Leaf> leaves;
    std::unordered_set<i64> leaf_starts;
    auto walk_s32arr = [&](i64 off) -> i64 {
        i64 o = off;
        for (int guard = 0; guard < 100000; guard += 2) {
            if (src.bs32(o) < 0) return o + 4;
            o += 4;
        }
        fail(name + ": runaway s32 array at " + hex(off));
    };
    auto walk_cstr = [&](i64 off) -> i64 {
        for (i64 o = off; o < D; o++)
            if (src.at(o) == 0) return o + 1;
        fail(name + ": unterminated string at " + hex(off));
    };
    auto add_leaf = [&](i64 start, Kind kind) {
        if (start == 0 || leaf_starts.count(start)) return;
        const i64 end = kind == LEAF_S32 ? walk_s32arr(start) : walk_cstr(start);
        leaves.push_back({start, kind, end});
        leaf_starts.insert(start);
    };
    for (i64 o : starts_of(PATHWAYPOINTS)) add_leaf(src.be32r(o + 4), LEAF_S32);
    for (i64 o : starts_of(WAYPOINTGROUPS)) {
        add_leaf(src.be32r(o + 0), LEAF_S32);
        add_leaf(src.be32r(o + 4), LEAF_S32);
    }
    for (i64 o : starts_of(PATROLPATHS)) add_leaf(src.be32r(o + 0), LEAF_S32);
    for (i64 o : starts_of(PADS)) add_leaf(src.be32r(o + 0x24), LEAF_CSTR);
    for (i64 o : starts_of(BOUNDPADS)) add_leaf(src.be32r(o + 0x24), LEAF_CSTR);
    for (i64 o : starts_of(PADNAMES)) add_leaf(src.be32r(o), LEAF_CSTR);
    for (i64 o : starts_of(BOUNDPADNAMES)) add_leaf(src.be32r(o), LEAF_CSTR);
    std::set<i64> ai_stream_starts;
    for (i64 o : starts_of(AILISTS)) {
        const i64 s = src.be32r(o);
        if (s && !leaf_starts.count(s)) ai_stream_starts.insert(s);
    }

    struct Region {
        i64 start, end;  // end -1 = resolve to the next start
        Kind kind;
        int grow_idx;
    };
    std::vector<Region> regions;
    regions.push_back({0, 0x28, HEADER, -1});
    for (size_t gi = 0; gi < grow.size(); gi++)
        if (grow[gi].w.term >= 0)
            regions.push_back({H[grow[gi].f], grow[gi].w.term + grow[gi].oldsz, GROW, static_cast<int>(gi)});
    if (H[INTRO]) regions.push_back({H[INTRO], intro_end, INTROK, -1});
    if (H[PROPDEFS]) regions.push_back({H[PROPDEFS], -1, PROPDEFSK, -1});
    for (const Leaf& l : leaves) regions.push_back({l.start, l.end, l.kind, -1});
    for (i64 s : ai_stream_starts) regions.push_back({s, -1, LEAF_AI, -1});
    std::stable_sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) { return a.start < b.start; });
    for (size_t i = 0; i < regions.size(); i++)
        if (regions[i].end < 0) regions[i].end = i + 1 < regions.size() ? regions[i + 1].start : D;

    std::vector<Region> tiled;
    i64 cursor = 0;
    for (const Region& r : regions) {
        if (r.start < cursor) fail(name + ": region overlap at " + hex(r.start));
        if (r.start > cursor) tiled.push_back({cursor, r.start, GAP, -1});
        if (r.end < r.start) fail(name + ": region has negative length");
        tiled.push_back(r);
        cursor = r.end;
    }
    if (cursor < D) tiled.push_back({cursor, D, GAP, -1});
    else if (cursor > D) fail(name + ": regions overrun EOF");

    Bytes propdefs_pc;
    bool have_propdefs = false;
    if (H[PROPDEFS]) {
        for (const Region& r : tiled)
            if (r.kind == PROPDEFSK) {
                PropDefs pd(src);
                propdefs_pc = pd.convert_stream(H[PROPDEFS], r.end);
                have_propdefs = true;
                break;
            }
    }

    std::unordered_map<i64, i64> delta_at;
    i64 cum = 0x28;
    for (const Region& r : tiled) {
        if (r.kind == HEADER) {
            delta_at[r.start] = 0;
            continue;
        }
        delta_at[r.start] = cum;
        if (r.kind == GROW) {
            const Grow& g = grow[r.grow_idx];
            const i64 n = static_cast<i64>(g.w.starts.size());
            cum += (n + 1) * (g.newsz - g.oldsz);
        } else if (r.kind == PROPDEFSK && have_propdefs) {
            cum += static_cast<i64>(propdefs_pc.size()) - (r.end - r.start);
        }
    }
    auto reloc = [&](i64 off) -> i64 {
        if (off == 0) return 0;
        auto it = delta_at.find(off);
        return off + (it == delta_at.end() ? 0 : it->second);
    };

    Out out(D + cum);
    for (int i = 0; i < NFIELDS; i++) {
        out.le32(8 * i, static_cast<uint32_t>(reloc(H[i])));
        out.le32(8 * i + 4, 0);
    }
    auto emit_pad = [&](i64 dst_o, i64 src_o, bool bbox) {
        for (int k = 0; k < 9; k++) out.le32(dst_o + 4 * k, src.be32r(src_o + 4 * k));
        out.le32(dst_o + 0x28, static_cast<uint32_t>(reloc(src.be32r(src_o + 0x24))));
        out.le32(dst_o + 0x2c, 0);
        out.zero(dst_o + 0x30, 8);
        if (bbox)
            for (int k = 0; k < 6; k++) out.le32(dst_o + 0x38 + 4 * k, src.be32r(src_o + 0x2c + 4 * k));
    };
    auto ptr_or_zero = [&](i64 o, i64 v) { out.le32(o, v ? static_cast<uint32_t>(reloc(v)) : 0); };
    for (const Grow& g : grow) {
        if (g.w.term < 0) continue;
        std::vector<i64> all = g.w.starts;
        all.push_back(g.w.term);
        const i64 table_base = reloc(H[g.f]);
        for (size_t idx = 0; idx < all.size(); idx++) {
            const i64 so = all[idx];
            const i64 d = table_base + static_cast<i64>(idx) * g.newsz;
            switch (g.f) {
            case PADS: emit_pad(d, so, false); break;
            case BOUNDPADS: emit_pad(d, so, true); break;
            case WAYPOINTGROUPS:
                ptr_or_zero(d, src.be32r(so + 0));
                out.le32(d + 4, 0);
                ptr_or_zero(d + 8, src.be32r(so + 4));
                out.le32(d + 12, 0);
                out.le32(d + 16, src.be32r(so + 8));
                out.le32(d + 20, 0);
                break;
            case PATHWAYPOINTS:
                out.le32(d, src.be32r(so + 0));
                out.le32(d + 4, 0);
                ptr_or_zero(d + 8, src.be32r(so + 4));
                out.le32(d + 12, 0);
                out.le32(d + 16, src.be32r(so + 8));
                out.le32(d + 20, src.be32r(so + 12));
                break;
            case PATROLPATHS:
                ptr_or_zero(d, src.be32r(so + 0));
                out.le32(d + 4, 0);
                out.u8(d + 8, src.at(so + 4));
                out.u8(d + 9, src.at(so + 5));
                out.le16(d + 10, src.bu16(so + 6));
                out.le32(d + 12, 0);
                break;
            case AILISTS:
                ptr_or_zero(d, src.be32r(so + 0));
                out.le32(d + 4, 0);
                out.le32(d + 8, src.be32r(so + 4));
                out.le32(d + 12, 0);
                break;
            case PADNAMES:
            case BOUNDPADNAMES:
                ptr_or_zero(d, src.be32r(so));
                out.le32(d + 4, 0);
                break;
            default: break;
            }
        }
    }

    const i64 intro_base_delta = H[INTRO] ? (delta_at.count(H[INTRO]) ? delta_at[H[INTRO]] : 0) : 0;
    for (auto [o, t] : intro_recs) {
        const i64 d = o + intro_base_delta;
        if (t == 6) {
            for (int k = 0; k < 10; k++) {
                if (k == 7) {
                    out.le16(d + 4 * k, src.bu16(o + 4 * k));
                    out.le16(d + 4 * k + 2, src.bu16(o + 4 * k + 2));
                } else {
                    out.le32(d + 4 * k, src.be32r(o + 4 * k));
                }
            }
        } else {
            for (int k = 0; k < INTRO_SZ.at(t) / 4; k++) out.le32(d + 4 * k, src.be32r(o + 4 * k));
        }
    }

    for (const Leaf& l : leaves) {
        const i64 d = reloc(l.start);
        if (l.kind == LEAF_S32) {
            for (i64 o = l.start; o < l.end; o += 4) out.le32(d + (o - l.start), src.be32r(o));
        } else {
            out.copy(d, src, l.start, l.end - l.start);
        }
    }

    for (const Region& r : tiled) {
        if (r.kind == PROPDEFSK) {
            const i64 d = reloc(r.start);
            out.ensure(d + static_cast<i64>(propdefs_pc.size()));
            std::copy(propdefs_pc.begin(), propdefs_pc.end(), out.b.begin() + d);
        } else if (r.kind == LEAF_AI || r.kind == GAP) {
            out.copy(reloc(r.start), src, r.start, r.end - r.start);
        }
    }
    return std::move(out.b);
}

void convert_bg_and_setups(const Bytes& rom, const Meta& meta, Image& img) {
    auto rom_slice = [&](i64 addr, i64 size) {
        const i64 a = std::min<i64>(addr, static_cast<i64>(rom.size()));
        const i64 e = std::min<i64>(addr + size, static_cast<i64>(rom.size()));
        return Src{rom.data() + a, e - a};
    };
    auto decompress_rz = [&](const std::string& name, const Src& comp) {
        if (comp.n < 2 || comp.p[0] != 0x11 || comp.p[1] != 0x72) fail(name + ": bad RZ magic");
        return inflate_raw(comp.p + 2, static_cast<size_t>(comp.n - 2));
    };

    // d69: table order
    std::unordered_set<std::string> existing;
    for (const auto& name : meta.table) {
        if (starts_with(name, "bg/") && ends_with(name, ".seg")) {
            std::string base = name.substr(name.rfind('/') + 1);
            base = base.substr(0, base.size() - 4);
            auto it = meta.files.find(base);
            if (it == meta.files.end() || it->second.second == 0) continue;
            img.emit(name, convert_seg(name, rom_slice(it->second.first, it->second.second)));
            existing.insert(name);
        } else if ((starts_with(name, "Tbg_") || name == "TbgxZ") && ends_with(name, "Z")) {
            auto it = meta.files.find(name);
            if (it == meta.files.end() || it->second.second == 0) continue;
            const Bytes dec = decompress_rz(name, rom_slice(it->second.first, it->second.second));
            img.emit(name, rz_compress(convert_stan(name, Src{dec.data(), static_cast<i64>(dec.size())})));
            existing.insert(name);
        }
    }

    // d88: sorted unique setup names, appended
    std::set<std::string> setups;
    for (const auto& n : meta.table)
        if ((starts_with(n, "Usetup") || starts_with(n, "Ump_setup")) && ends_with(n, "Z")) setups.insert(n);
    for (const auto& name : setups) {
        if (existing.count(name)) continue;
        auto it = meta.files.find(name);
        if (it == meta.files.end() || it->second.second == 0) continue;
        const Bytes dec = decompress_rz(name, rom_slice(it->second.first, it->second.second));
        img.emit(name, rz_compress(convert_usetup(name, Src{dec.data(), static_cast<i64>(dec.size())})));
    }
}

// ---------------------------------------------------------------- output ---

void write_file(const std::string& path, const void* data, size_t n) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) fail("can't write " + path);
    const bool ok = std::fwrite(data, 1, n, f) == n;
    if (std::fclose(f) != 0 || !ok) fail("can't write " + path);
}

// pad_end: d43 pads pcmodels.bin to 16 bytes; d69/d88 leave pccg.bin unpadded.
void write_image(const std::string& dir, const char* bin_name, const Image& img, bool pad_end) {
    ::mkdir(dir.c_str(), 0755);
    Bytes data = img.data;
    if (pad_end) data.resize((data.size() + 15) & ~size_t(15), 0);
    std::string csv = "name,offset,size\r\n";  // Python's csv module ends rows with \r\n
    for (const auto& [name, off, size] : img.manifest)
        csv += name + "," + std::to_string(off) + "," + std::to_string(size) + "\r\n";
    // the manifest last: the port treats a present manifest as a finished conversion
    write_file(dir + "/" + bin_name, data.data(), data.size());
    write_file(dir + "/manifest.csv", csv.data(), csv.size());
}

Bytes read_rom(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) fail("can't read " + path);
    return Bytes(std::istreambuf_iterator<char>(in), {});
}

}  // namespace

// Converts <rom> into <data_dir>/pcmodels-<region>/ and <data_dir>/pccg-<region>/. Returns 0 on success; on failure
// writes the reason to `error` (if non-null) and returns 1. Takes about a second.
extern "C" int plm_ge_convert(const char* rom_path, const char* meta_path, const char* data_dir, const char* region,
                              char* error, size_t error_size) {
    try {
        const Bytes rom = read_rom(rom_path);
        const Meta meta = read_meta(meta_path);
        Image models, cg;
        convert_models(rom, meta, models);
        convert_bg_and_setups(rom, meta, cg);
        const std::string base(data_dir);
        write_image(base + "/pccg-" + region, "pccg.bin", cg, false);
        write_image(base + "/pcmodels-" + region, "pcmodels.bin", models, true);
        return 0;
    } catch (const std::exception& e) {
        if (error && error_size) std::snprintf(error, error_size, "%s", e.what());
        return 1;
    }
}

#ifdef GE_CONVERT_MAIN
int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: %s <rom.z64> <ge007-convert-meta.txt> <data dir> <region>\n", argv[0]);
        return 2;
    }
    char err[512];
    if (plm_ge_convert(argv[1], argv[2], argv[3], argv[4], err, sizeof err) != 0) {
        std::fprintf(stderr, "conversion failed: %s\n", err);
        return 1;
    }
    std::printf("converted into %s\n", argv[3]);
    return 0;
}
#endif
