// arm64_x18_rewrite.h -- virtual x18: take x18 out of a library's code at load.
//
// Windows on Arm keeps the TEB in x18 (arm64_x18.h), so a library that uses it as scratch cannot run as it is. Here
// each instruction that names x18 becomes `b thunk`; the thunk runs the instruction on another register that holds
// the library's x18 for the length of the instruction, taken from and put back in a per-thread slot. x18 itself is
// never changed, so the TEB stays valid for every exception and Windows call. The slots are TLS slots, which live in
// the TEB at a fixed offset, so x18 is also the base register that reaches them.
//
// Nothing here calls Windows: the planning and the thunks are plain functions on bytes, so a test can run them
// anywhere.

#pragma once

#include "winapk/arm64_x18.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace winapk::arm64 {

// ---- operands ---------------------------------------------------------------------------------------------

namespace x18_flow {

inline int64_t sext(uint64_t v, unsigned bits) { return static_cast<int64_t>(v << (64 - bits)) >> (64 - bits); }

inline bool is_b(uint32_t w) { return (w & 0xfc000000) == 0x14000000; }
inline bool is_bl(uint32_t w) { return (w & 0xfc000000) == 0x94000000; }
inline bool is_bcond(uint32_t w) { return (w & 0xff000010) == 0x54000000; }
inline bool is_cbz(uint32_t w) { return (w & 0x7e000000) == 0x34000000; }
inline bool is_tbz(uint32_t w) { return (w & 0x7e000000) == 0x36000000; }
inline bool is_br(uint32_t w) {  // BR, BRAA, BRAAZ and their B-key forms: not RET
    return (w & 0xfffffc1f) == 0xd61f0000 || (w & 0xfffff800) == 0xd61f0800 || (w & 0xfffff800) == 0xd71f0800;
}
inline bool is_ret(uint32_t w) {
    return (w & 0xfffffc1f) == 0xd65f0000 || w == 0xd65f0bff || w == 0xd65f0fff || w == 0xd69f03e0 || w == 0xd69f0bff ||
           w == 0xd69f0fff;
}
inline bool ends_block(uint32_t w) { return is_b(w) || is_br(w) || is_ret(w); }  // control never falls through
inline bool is_literal_load(uint32_t w) { return (w & 0x3b000000) == 0x18000000; }

// The target of a direct branch at `pc`; false if `w` is not one.
inline bool direct_target(uint32_t w, uint64_t pc, uint64_t* t) {
    if (is_b(w) || is_bl(w)) *t = pc + static_cast<uint64_t>(sext(w & 0x3ffffff, 26) * 4);
    else if (is_bcond(w) || is_cbz(w)) *t = pc + static_cast<uint64_t>(sext((w >> 5) & 0x7ffff, 19) * 4);
    else if (is_tbz(w)) *t = pc + static_cast<uint64_t>(sext((w >> 5) & 0x3fff, 14) * 4);
    else return false;
    return true;
}

// The bytes a literal load reads (0: a prefetch).
inline unsigned literal_size(uint32_t w) {
    const unsigned opc = w >> 30;
    if (w & (1u << 26)) return opc == 0 ? 4 : opc == 1 ? 8 : 16;
    return opc == 0 ? 4 : opc == 1 ? 8 : opc == 2 ? 4 : 0;
}

}  // namespace x18_flow

// Which general-register fields of an instruction hold x18, and whether it is read, written, or both.
// Only forms whose every operand is known are Rewritable; anything else that names x18 is Unsupported, and a
// library with one is refused.
struct X18Operands {
    enum Kind : uint8_t {
        None,           // no field names x18
        Rewritable,     // the instruction can run on another register
        CompareBranch,  // cbz, cbnz, tbz, tbnz: pc-relative, with x18 as the tested register
        Unsupported,
    };
    enum Field : uint8_t { Rd = 1, Rn = 2, Rm = 4, Ra = 8 };  // bit masks of `fields`
    Kind kind = None;
    uint8_t fields = 0;    // the fields that hold x18
    uint32_t named = 0;    // bit n: register n is a general-register operand of the instruction (n = 31: sp or xzr)
    bool reads = false;    // x18's value is an input
    bool writes = false;   // x18 is an output
};

inline unsigned x18_field_shift(unsigned bit) { return bit == 1 ? 0 : bit == 2 ? 5 : bit == 4 ? 16 : 10; }

inline X18Operands x18_operands(uint32_t i) {
    using namespace x18_detail;
    X18Operands o;
    bool ok = false, cb = false;
    auto gp = [&](X18Operands::Field f, bool rd, bool wr) {
        const uint32_t r = (i >> x18_field_shift(f)) & 31;
        o.named |= 1u << r;
        if (r == kX18) {
            o.fields |= f;
            o.reads |= rd;
            o.writes |= wr;
        }
    };
    constexpr X18Operands::Field D = X18Operands::Rd, N = X18Operands::Rn, M = X18Operands::Rm, A = X18Operands::Ra;
    const uint32_t op0 = bits(i, 28, 25);

    if ((op0 & 0xe) == 0x8) {  // data processing, immediate
        if (bits(i, 28, 24) == 0x10) {
            // ADR, ADRP: pc-relative, not supported
        } else {
            switch (bits(i, 25, 23)) {
                case 0x5: gp(D, bits(i, 30, 29) == 3, true); ok = bits(i, 30, 29) != 1; break;  // movz, movn, movk
                case 0x7: gp(D, false, true); gp(N, true, false); gp(M, true, false); ok = true; break;  // extr
                case 0x4: gp(D, false, true); gp(N, true, false); ok = true; break;                      // logical
                case 0x6: gp(D, bits(i, 30, 29) == 1, true); gp(N, true, false); ok = true; break;      // bitfield
                case 0x2: gp(D, false, true); gp(N, true, false); ok = true; break;                      // add, sub
                default: break;
            }
        }
    } else if ((op0 & 0xe) == 0xa) {  // branches, system
        if (bits(i, 30, 25) == 0x1a || bits(i, 30, 25) == 0x1b) {
            gp(D, true, false);
            ok = cb = true;
        } else if (bits(i, 31, 22) == 0x354) {  // msr, mrs, sys, sysl: the L bit says which way Rt goes
            const bool l = bits(i, 21, 21) != 0;
            gp(D, !l, l);
            ok = true;
        }
    } else if ((op0 & 0x5) == 0x4) {  // loads and stores
        const bool v = bits(i, 26, 26) != 0;
        const uint32_t cls = bits(i, 29, 27), opc = bits(i, 23, 22), op4 = bits(i, 11, 10), size = bits(i, 31, 30);
        if (cls == 0x1 && v) {  // SIMD structures: the base, and with post-index its increment register
            const bool post = bits(i, 23, 23) != 0;
            gp(N, true, post);
            if (post) gp(M, true, false);
            ok = true;
        } else if (cls == 0x5) {  // pairs
            const uint32_t mode = bits(i, 24, 23);
            const bool load = bits(i, 22, 22) != 0;
            if (!v) {
                gp(D, !load, load);
                gp(A, !load, load);
            }
            gp(N, true, mode == 1 || mode == 3);
            ok = true;
        } else if (cls == 0x7) {  // one register
            const bool prfm = !v && size == 3 && opc == 2;
            const bool load = opc != 0;
            const bool data = !v && !prfm;
            if (bits(i, 24, 24)) {  // unsigned offset
                if (data) gp(D, !load, load);
                gp(N, true, false);
                ok = true;
            } else if (!bits(i, 21, 21)) {  // unscaled, post-index, unprivileged, pre-index
                if (data) gp(D, !load, load);
                gp(N, true, op4 == 1 || op4 == 3);
                ok = !prfm || op4 == 0;  // PRFUM is the only prefetch among these; the others are undefined
            } else if (op4 == 2) {  // register offset
                if (data) gp(D, !load, load);
                gp(N, true, false);
                gp(M, true, false);
                ok = true;
            }
        }
    } else if ((op0 & 0x7) == 0x5) {  // data processing, register
        const uint32_t t = bits(i, 28, 21);
        if ((t >> 3) == 0x0a || (t >> 3) == 0x0b || (t == 0xd0 && bits(i, 15, 10) == 0) || t == 0xd4 ||
            (t == 0xd6 && !bits(i, 30, 30))) {
            gp(D, false, true);
            gp(N, true, false);
            gp(M, true, false);
            ok = true;
        } else if (t == 0xd2 && !bits(i, 10, 10) && !bits(i, 4, 4)) {  // ccmp, ccmn: no destination
            gp(N, true, false);
            if (!bits(i, 11, 11)) gp(M, true, false);
            ok = true;
        } else if (t == 0xd6 && bits(i, 30, 30) && bits(i, 20, 16) == 0) {  // rbit, rev, clz, cls
            gp(D, false, true);
            gp(N, true, false);
            ok = true;
        } else if ((t >> 3) == 0x1b) {  // madd, msub, smaddl...
            gp(D, false, true);
            gp(N, true, false);
            gp(M, true, false);
            gp(A, true, false);
            ok = true;
        }
    } else if ((op0 & 0x7) == 0x7) {  // SIMD and FP: the moves to and from a general register
        if (bits(i, 28, 24) == 0x1e && bits(i, 30, 29) == 0 && (bits(i, 21, 21) == 0 || bits(i, 15, 10) == 0)) {
            const uint32_t opcode = bits(i, 18, 16);
            if (opcode == 2 || opcode == 3 || (bits(i, 21, 21) && opcode == 7)) gp(N, true, false);
            else gp(D, false, true);
            ok = true;
        } else if (bits(i, 31, 31) == 0 && bits(i, 29, 29) == 0 && bits(i, 28, 21) == 0x70 && bits(i, 15, 15) == 0 &&
                   bits(i, 10, 10) == 1) {
            const uint32_t imm4 = bits(i, 14, 11);
            if (imm4 == 1 || imm4 == 3) { gp(N, true, false); ok = true; }
            else if (imm4 == 5 || imm4 == 7) { gp(D, false, true); ok = true; }
        }
    }

    if (!o.fields) {
        o.kind = uses_x18(i) ? X18Operands::Unsupported : X18Operands::None;
        return o;
    }
    o.kind = ok ? (cb ? X18Operands::CompareBranch : X18Operands::Rewritable) : X18Operands::Unsupported;
    return o;
}

// ---- thunks -----------------------------------------------------------------------------------------------

// Where the two per-thread slots are: byte offsets from the TEB (so from x18). `value` holds the library's x18,
// `scratch` holds the register the thunk borrows.
struct X18Slots {
    uint32_t value = 0;
    uint32_t scratch = 0;
};

namespace x18_thunk {

inline uint32_t ldr_x18(uint32_t rt, uint32_t off) { return 0xf9400000u | (off / 8) << 10 | 18u << 5 | rt; }
inline uint32_t str_x18(uint32_t rt, uint32_t off) { return 0xf9000000u | (off / 8) << 10 | 18u << 5 | rt; }

// B from `from` to `to`, if it reaches (+-128 MB).
inline bool branch(uint64_t from, uint64_t to, uint32_t* w) {
    const int64_t d = static_cast<int64_t>(to - from);
    if ((d & 3) || d < -(int64_t{1} << 27) || d >= (int64_t{1} << 27)) return false;
    *w = 0x14000000u | (static_cast<uint32_t>(d >> 2) & 0x3ffffff);
    return true;
}

}  // namespace x18_thunk

constexpr size_t kX18ThunkMaxWords = 7;

// Appends the thunk for the instruction `w` at `site` to `out`, to be placed at `thunk`; the site is then
// replaced by `b thunk` (x18_thunk::branch). False, with *why, when it cannot be done.
inline bool make_x18_thunk(uint32_t w, uint64_t site, uint64_t thunk, const X18Slots& slots, std::vector<uint32_t>* out,
                           std::string* why) {
    using namespace x18_thunk;
    const X18Operands o = x18_operands(w);
    if (o.kind != X18Operands::Rewritable && o.kind != X18Operands::CompareBranch) {
        *why = "an instruction form that is not supported";
        return false;
    }
    // The borrowed register: any that the instruction does not name; it is saved and restored, so it need not be free.
    uint32_t s = 32;
    for (uint32_t r : {9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u, 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u}) {
        if (!(o.named >> r & 1)) { s = r; break; }
    }
    if (s == 32) {
        *why = "no register left to borrow";
        return false;
    }
    uint32_t ins = w;
    for (unsigned f : {1u, 2u, 4u, 8u}) {
        if (o.fields & f) ins = (ins & ~(31u << x18_field_shift(f))) | (s << x18_field_shift(f));
    }
    if (x18_operands(ins).fields != 0 || uses_x18(ins)) {
        *why = "the rewritten instruction still names x18";
        return false;
    }
    const size_t base = out->size();
    auto at = [&](size_t n) { return thunk + (n - base) * 4; };
    out->push_back(str_x18(s, slots.scratch));
    if (o.kind == X18Operands::CompareBranch) {
        // cbz xS, taken; restore; b site+4; taken: restore; b target. The test stays in the thunk, so its range
        // does not matter, and neither the stack nor the flags are touched.
        uint64_t target;
        if (!x18_flow::direct_target(w, site, &target)) {
            *why = "a compare-and-branch with no target";
            return false;
        }
        out->push_back(ldr_x18(s, slots.value));
        if (x18_flow::is_cbz(w)) ins = (ins & ~(0x7ffffu << 5)) | (3u << 5);
        else ins = (ins & ~(0x3fffu << 5)) | (3u << 5);
        out->push_back(ins);
        uint32_t b1, b2;
        out->push_back(ldr_x18(s, slots.scratch));
        if (!branch(at(out->size()), site + 4, &b1)) { *why = "the thunk is out of branch range"; return false; }
        out->push_back(b1);
        out->push_back(ldr_x18(s, slots.scratch));
        if (!branch(at(out->size()), target, &b2)) { *why = "the thunk is out of branch range"; return false; }
        out->push_back(b2);
        return true;
    }
    if (o.reads) out->push_back(ldr_x18(s, slots.value));
    out->push_back(ins);
    if (o.writes) out->push_back(str_x18(s, slots.value));
    out->push_back(ldr_x18(s, slots.scratch));
    uint32_t b;
    if (!branch(at(out->size()), site + 4, &b)) { *why = "the thunk is out of branch range"; return false; }
    out->push_back(b);
    return true;
}

// ---- code or data -----------------------------------------------------------------------------------------
//
// Only words that are code may be patched: a data word that decodes like an x18 instruction would be corrupted.
// .eh_frame gives the function ranges; literal pools and jump tables sit inside them, so inside a function a word
// is code only if control can reach it:
//   - a word a `ldr literal` loads is data;
//   - a function with no indirect `br` is followed from its start and its direct branch targets, so a word after
//     an unconditional branch that nothing targets is data;
//   - a function with an indirect `br` (a switch) may enter at any block start: every direct target and every word
//     after an unconditional `b`, `ret` or `br`. A data word right after such a branch is then not told from code,
//     except that its block must be plausible code up to a terminator (no word of the reserved space).

struct X18Site {
    uint64_t vaddr = 0;   // where the loader maps it, before the load bias
    uint64_t offset = 0;  // in the file
    uint32_t word = 0;
};

struct X18Plan {
    std::vector<X18Site> sites;    // every word that names x18, in a function and reachable
    size_t outside_functions = 0;  // words naming x18 outside every .eh_frame function that are not text
    size_t literal_data = 0;       // ... a literal load reads
    size_t unreachable = 0;        // ... no control flow reaches (a function without an indirect br)
    size_t implausible = 0;        // ... in a block that is not plausible code (a function with one)
    size_t first_suspect = 0;      // file offset of the first word counted above
    bool ok = false;               // false: not a readable arm64 ELF file, or it has no function ranges
    bool risky() const { return outside_functions || literal_data || unreachable || implausible; }
};

inline X18Plan plan_x18_sites(const uint8_t* file, size_t size) {
    using namespace x18_flow;
    X18Plan plan;
    const ElfCode code = elf_code(file, size);
    if (!code.ok || code.functions.empty()) return plan;
    plan.ok = true;
    auto names_x18 = [](uint32_t w) { return uses_x18(w) || x18_operands(w).fields != 0; };
    auto suspect = [&](uint64_t off) {
        if (!plan.first_suspect) plan.first_suspect = static_cast<size_t>(off);
    };

    std::set<uint64_t> literals;
    for (const ExecRange& e : code.ranges) {
        for (uint64_t at = 0; at + 4 <= e.size; at += 4) {
            const uint64_t va = e.vaddr + at;
            if (!in_functions(code.functions, va)) continue;
            uint32_t w;
            std::memcpy(&w, file + e.offset + at, 4);
            if (!is_literal_load(w)) continue;
            const uint64_t t = va + static_cast<uint64_t>(sext((w >> 5) & 0x7ffff, 19) * 4);
            for (unsigned n = 0; n < literal_size(w); n += 4) literals.insert(t + n);
        }
    }

    for (const ExecRange& e : code.ranges) {
        uint64_t done_to = 0;  // ranges are sorted; one that overlaps the last is skipped, so no word is taken twice
        for (const FdeRange& f : code.functions) {
            if (f.lo < e.vaddr || f.hi > e.vaddr + e.size || (f.lo & 3) || (f.hi & 3) || f.hi <= f.lo ||
                f.lo < done_to) {
                continue;
            }
            done_to = f.hi;
            const size_t n = static_cast<size_t>((f.hi - f.lo) / 4);
            std::vector<uint32_t> w(n);
            std::memcpy(w.data(), file + e.offset + (f.lo - e.vaddr), n * 4);
            bool any = false, has_br = false;
            for (uint32_t x : w) {
                any = any || names_x18(x);
                has_br = has_br || is_br(x);
            }
            if (!any) continue;
            // Block starts, and which words something enters.
            std::vector<char> lead(n + 1, 0), targeted(n + 1, 0), reach(n, 1);
            std::vector<size_t> bad_before, start_of, term_of;
            lead[0] = 1;
            for (size_t i = 0; i < n; ++i) {
                uint64_t t;
                if (direct_target(w[i], f.lo + i * 4, &t) && t >= f.lo && t < f.hi) {
                    lead[(t - f.lo) / 4] = targeted[(t - f.lo) / 4] = 1;
                }
                if (ends_block(w[i])) lead[i + 1] = 1;
            }
            if (!has_br) {
                bool live = true;
                for (size_t i = 0; i < n; ++i) {
                    live = live || targeted[i];
                    reach[i] = live;
                    if (ends_block(w[i])) live = false;
                }
            }
            for (size_t i = 0; i < n; ++i) {
                if (!names_x18(w[i])) continue;
                const uint64_t va = f.lo + i * 4;
                const uint64_t off = e.offset + (va - e.vaddr);
                if (literals.count(va)) {
                    ++plan.literal_data;
                    suspect(off);
                } else if (!reach[i]) {
                    ++plan.unreachable;
                    suspect(off);
                } else if (has_br) {
                    if (bad_before.empty()) {  // once per function: reserved-space words, blocks ending in a terminator
                        bad_before.assign(n + 1, 0);
                        start_of.assign(n, 0);
                        term_of.assign(n, n);
                        for (size_t k = 0; k < n; ++k) {
                            bad_before[k + 1] = bad_before[k] + (((w[k] >> 25) & 0xf) == 0 && (w[k] >> 31) == 0);
                            start_of[k] = lead[k] ? k : start_of[k - 1];
                        }
                        for (size_t k = n; k-- > 0;) {
                            term_of[k] = ends_block(w[k]) ? k : (k + 1 < n ? term_of[k + 1] : n);
                        }
                    }
                    const bool plausible = term_of[i] < n && bad_before[term_of[i] + 1] == bad_before[start_of[i]];
                    if (!plausible) {
                        ++plan.implausible;
                        suspect(off);
                    }
                }
                plan.sites.push_back({va, off, w[i]});
            }
        }
        for (uint64_t at = 0; at + 4 <= e.size; at += 4) {
            if (in_functions(code.functions, e.vaddr + at)) continue;
            uint32_t w;
            std::memcpy(&w, file + e.offset + at, 4);
            if (looks_like_text(w) || !names_x18(w)) continue;
            ++plan.outside_functions;
            suspect(e.offset + at);
        }
    }
    std::sort(plan.sites.begin(), plan.sites.end(),
              [](const X18Site& a, const X18Site& b) { return a.vaddr < b.vaddr; });
    return plan;
}

}  // namespace winapk::arm64
