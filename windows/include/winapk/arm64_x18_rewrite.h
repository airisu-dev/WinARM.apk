// arm64_x18_rewrite.h -- virtual x18: take x18 out of a library's code at load.
//
// Windows on Arm keeps the TEB in x18 (arm64_x18.h), so a library that uses it as scratch cannot run as it is. Here
// each instruction that names x18 becomes `b thunk` (`bl thunk` for `blr x18`); the thunk runs the instruction on
// another register that holds the library's x18 for the length of the instruction, taken from and put back in a
// per-thread slot. A branch through x18 cannot put anything back: it clobbers a register that is dead where it may
// land. x18 itself is never changed, so the TEB stays valid for every exception and Windows call. The slots are
// TLS slots, which live in the TEB at a fixed offset, so x18 is also the base register that reaches them.
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
// blr and its authenticated forms.
inline bool is_call(uint32_t w) {
    return is_bl(w) || (w & 0xfffffc1f) == 0xd63f0000 || (w & 0xfffff800) == 0xd63f0800 ||
           (w & 0xfffff800) == 0xd73f0800;
}
inline bool is_literal_load(uint32_t w) { return (w & 0x3b000000) == 0x18000000; }
// A load of a general register with a register offset (ldr, ldrsw, ldrb...): a table lookup.
inline bool is_indexed_load(uint32_t w) { return (w & 0x3f200c00) == 0x38200800 && ((w >> 22) & 3) != 0; }
// add xD, xN, xM (shifted or extended register).
inline bool is_add_x(uint32_t w, uint32_t rd) { return (w & 0xff000000) == 0x8b000000 && (w & 31) == rd; }

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
// Only forms whose every operand is known are accepted (Rewritable, CompareBranch, IndirectBranch, IndirectCall,
// PcRelAddress); anything else that names x18 is Unsupported, and a library with one is refused.
struct X18Operands {
    enum Kind : uint8_t {
        None,           // no field names x18
        Rewritable,     // the instruction can run on another register
        CompareBranch,  // cbz, cbnz, tbz, tbnz: pc-relative, with x18 as the tested register
        IndirectBranch, // br x18, ret x18: control leaves, so nothing can be restored after it
        IndirectCall,   // blr x18
        PcRelAddress,   // adr x18, adrp x18
        Unsupported,
    };
    enum Field : uint8_t { Rd = 1, Rn = 2, Rm = 4, Ra = 8 };  // bit masks of `fields`
    Kind kind = None;
    bool complete = false;  // every general-register operand of the word is in `named`
    uint8_t fields = 0;    // the fields that hold x18
    uint32_t named = 0;    // bit n: register n is a general-register operand of the instruction (n = 31: sp or xzr)
    uint32_t read_mask = 0;   // bit n: register n is read by the instruction
    uint32_t write_mask = 0;  // bit n: register n is written
    bool reads = false;    // x18's value is an input
    bool writes = false;   // x18 is an output
};

inline unsigned x18_field_shift(unsigned bit) { return bit == 1 ? 0 : bit == 2 ? 5 : bit == 4 ? 16 : 10; }

inline X18Operands x18_operands(uint32_t i) {
    using namespace x18_detail;
    X18Operands o;
    bool ok = false, cb = false, complete = false;
    X18Operands::Kind special = X18Operands::None;
    auto gp = [&](X18Operands::Field f, bool rd, bool wr) {
        const uint32_t r = (i >> x18_field_shift(f)) & 31;
        o.named |= 1u << r;
        if (rd) o.read_mask |= 1u << r;
        if (wr) o.write_mask |= 1u << r;
        if (r == kX18) {
            o.fields |= f;
            o.reads |= rd;
            o.writes |= wr;
        }
    };
    constexpr X18Operands::Field D = X18Operands::Rd, N = X18Operands::Rn, M = X18Operands::Rm, A = X18Operands::Ra;
    const uint32_t op0 = bits(i, 28, 25);

    if ((op0 & 0xe) == 0x8) {  // data processing, immediate
        if (bits(i, 28, 24) == 0x10) {  // ADR, ADRP
            gp(D, false, true);
            ok = true;
            special = X18Operands::PcRelAddress;
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
        if (x18_flow::is_b(i) || x18_flow::is_bl(i) || x18_flow::is_bcond(i) || bits(i, 31, 24) == 0xd4) {
            ok = true;  // no general register
        } else if ((i & 0xfffffc1f) == 0xd61f0000 || (i & 0xfffffc1f) == 0xd63f0000 || (i & 0xfffffc1f) == 0xd65f0000) {
            gp(N, true, false);  // br, blr, ret
            ok = true;
            special = (i & 0xfffffc1f) == 0xd63f0000 ? X18Operands::IndirectCall : X18Operands::IndirectBranch;
        } else if (bits(i, 30, 25) == 0x1a || bits(i, 30, 25) == 0x1b) {
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
    } else if ((op0 & 0x7) == 0x7) {  // SIMD and FP: only the moves to and from a general register name one
        ok = true;
        if (bits(i, 28, 24) == 0x1e && bits(i, 30, 29) == 0 && (bits(i, 21, 21) == 0 || bits(i, 15, 10) == 0)) {
            const uint32_t opcode = bits(i, 18, 16);
            if (opcode == 2 || opcode == 3 || (bits(i, 21, 21) && opcode == 7)) gp(N, true, false);
            else gp(D, false, true);
            ok = true;
        } else if (bits(i, 31, 31) == 0 && bits(i, 29, 29) == 0 && bits(i, 28, 21) == 0x70 && bits(i, 15, 15) == 0 &&
                   bits(i, 10, 10) == 1) {
            const uint32_t imm4 = bits(i, 14, 11);
            if (imm4 == 1 || imm4 == 3) gp(N, true, false);
            else if (imm4 == 5 || imm4 == 7) gp(D, false, true);
        }
    }

    complete = ok;
    o.complete = complete;
    if (!o.fields) {
        o.kind = uses_x18(i) ? X18Operands::Unsupported : X18Operands::None;
        return o;
    }
    o.kind = !ok ? X18Operands::Unsupported
                 : special != X18Operands::None ? special
                 : cb ? X18Operands::CompareBranch : X18Operands::Rewritable;
    return o;
}

// The general registers a word may name, as a bit set (31: sp or xzr). A word whose operands the decoder does not
// fully know is taken to name every register its four register fields can hold.
inline uint32_t x18_gpr_named(uint32_t i) {
    const X18Operands o = x18_operands(i);
    if (o.complete) return o.named;
    return 1u << (i & 31) | 1u << ((i >> 5) & 31) | 1u << ((i >> 10) & 31) | 1u << ((i >> 16) & 31);
}

// The registers a word reads and writes, by number. A word the decoder does not fully know reads whatever its four
// register fields hold, which is the safe answer for the question this is used for: is a register live?
inline void x18_gpr_access(uint32_t i, uint32_t* reads, uint32_t* writes) {
    const X18Operands o = x18_operands(i);
    if (o.complete) {
        *reads = o.read_mask;
        *writes = o.write_mask;
    } else {
        *reads = x18_gpr_named(i);
        *writes = 0;
    }
}

// The register an indirect branch through x18 may clobber: one that is not in `live` (as bits), the registers
// something may still read. x16 and x17 first: they are the intra-procedure-call scratch registers. Then x15..x9,
// x8..x0, and the link register last, which is dead in a non-leaf function between its prologue and its
// epilogue. 0xff when none is free.
inline uint8_t x18_pick_borrow(uint32_t live) {
    for (uint32_t r : {16u, 17u, 15u, 14u, 13u, 12u, 11u, 10u, 9u, 8u, 7u, 6u, 5u, 4u, 3u, 2u, 1u, 0u, 30u}) {
        if (!(live >> r & 1)) return static_cast<uint8_t>(r);
    }
    return 0xff;
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

// Appends the thunk for the instruction `w` at `site` to `out`, to be placed at `thunk`, and sets *patch to the word
// that replaces the site: `b thunk`, or `bl thunk` for `blr x18` so that the call returns to site + 4. `borrow` is
// the register an indirect branch or call may clobber (x18_pick_borrow, 0xff for none); the other forms save and
// restore the one they pick. False, with *why, when it cannot be done.
inline bool make_x18_thunk(uint32_t w, uint64_t site, uint64_t thunk, const X18Slots& slots, uint8_t borrow,
                           std::vector<uint32_t>* out, uint32_t* patch, std::string* why) {
    using namespace x18_thunk;
    const X18Operands o = x18_operands(w);
    if (o.kind != X18Operands::Rewritable && o.kind != X18Operands::CompareBranch &&
        o.kind != X18Operands::IndirectBranch && o.kind != X18Operands::IndirectCall &&
        o.kind != X18Operands::PcRelAddress) {
        *why = "an instruction form that is not supported";
        return false;
    }
    if (!branch(site, thunk, patch)) {
        *why = "the thunk is out of branch range";
        return false;
    }
    if (o.kind == X18Operands::IndirectBranch || o.kind == X18Operands::IndirectCall) {
        if (borrow >= 18 && (borrow != 30 || o.kind == X18Operands::IndirectCall)) {
            *why = "no register is free to hold the target of the indirect branch";
            return false;
        }
        // Nothing is restored: after a branch there is no place for it, and a call may clobber x16 and x17.
        out->push_back(ldr_x18(borrow, slots.value));
        out->push_back(0xd61f0000u | static_cast<uint32_t>(borrow) << 5);  // br xR
        if (o.kind == X18Operands::IndirectCall) *patch |= 0x80000000u;      // bl
        return true;
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
    if (o.kind == X18Operands::PcRelAddress) {
        // The same absolute address, built from the thunk's own pc: adrp reaches +-4 GB, add fills in the low bits.
        const int64_t imm = x18_flow::sext(((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3), 21);
        const bool page = (w >> 31) != 0;
        const uint64_t target = page ? (site & ~uint64_t{0xfff}) + static_cast<uint64_t>(imm * 4096)
                                     : site + static_cast<uint64_t>(imm);
        const int64_t pages = static_cast<int64_t>(target >> 12) - static_cast<int64_t>(at(out->size() + 1) >> 12);
        if (pages < -(int64_t{1} << 20) || pages >= (int64_t{1} << 20)) {
            *why = "the target of adr or adrp is out of reach of the thunk";
            return false;
        }
        out->push_back(str_x18(s, slots.scratch));
        out->push_back(0x90000000u | (static_cast<uint32_t>(pages) & 3) << 29 |
                       ((static_cast<uint32_t>(pages) >> 2) & 0x7ffff) << 5 | s);  // adrp xS
        if (target & 0xfff) out->push_back(0x91000000u | static_cast<uint32_t>(target & 0xfff) << 10 | s << 5 | s);
        out->push_back(str_x18(s, slots.value));
        out->push_back(ldr_x18(s, slots.scratch));
        uint32_t b;
        if (!branch(at(out->size()), site + 4, &b)) { *why = "the thunk is out of branch range"; return false; }
        out->push_back(b);
        return true;
    }
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

// Does the indirect branch w[i] jump through a table? Compilers emit: an indexed load of an offset, an add of the
// table's base, the branch (other instructions may sit between). A tail call through a pointer has no such add.
inline bool x18_table_dispatch(const uint32_t* w, size_t i) {
    const uint32_t rn = (w[i] >> 5) & 31;
    bool added = false;
    uint32_t wanted = 0;  // the registers the add reads: one of them comes from the table
    size_t k = i;
    for (size_t n = 0; n < 16 && k > 0; ++n) {
        --k;
        if (x18_flow::ends_block(w[k])) break;
        uint32_t reads, writes;
        x18_gpr_access(w[k], &reads, &writes);
        if (!x18_operands(w[k]).complete) {
            if (!added && (x18_gpr_named(w[k]) >> rn & 1)) return false;  // it may write the target
            continue;
        }
        if (!added) {
            if (!(writes >> rn & 1)) continue;
            if (!x18_flow::is_add_x(w[k], rn)) return false;
            added = true;
            wanted = reads;
        } else if (x18_flow::is_indexed_load(w[k]) && (writes & wanted)) {
            return true;
        }
    }
    return false;
}

struct X18Site {
    uint64_t vaddr = 0;   // where the loader maps it, before the load bias
    uint64_t offset = 0;  // in the file
    uint32_t word = 0;
    uint8_t borrow = 0xff;  // for an indirect branch or call: the register its thunk clobbers (0xff: none is free)
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

    // Function groups, for the register an indirect branch through x18 clobbers: a function with the functions it is
    // joined to by direct branches that are not calls (a hot/cold split, a tail call), and the registers that may be
    // live where control can land in them (x0..x17 and the link register). The branch may land on any block start of
    // the group: a function start, a direct branch target, a word after a block end. A register is live at one if
    // the straight-line code from it reads the register before writing it, and before a call, which clobbers x0..x17
    // and the link register. Built when the first
    // such branch is met.
    const std::vector<FdeRange>& fns = code.functions;
    std::vector<size_t> parent;
    std::vector<uint32_t> group_live;  // at each group's root
    auto find_root = [&](size_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    auto function_at = [&](uint64_t va) {
        size_t lo = 0, hi = fns.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (fns[mid].lo <= va) lo = mid + 1; else hi = mid;
        }
        return lo > 0 && va < fns[lo - 1].hi ? lo - 1 : fns.size();
    };
    auto build_groups = [&]() {
        constexpr uint32_t kArgs = 0x1ff;     // x0..x8: arguments, and the results of a call
        constexpr uint32_t kAll = 0x3ffff | 1u << 30;  // x0..x17 and the link register
        constexpr uint32_t kLink = 1u << 30;
        constexpr size_t kScanLimit = 512;    // words looked at from one block start; more counts as live
        parent.resize(fns.size());
        for (size_t i = 0; i < fns.size(); ++i) parent[i] = i;
        std::vector<uint64_t> targets;  // every direct branch target that is not a call
        auto word_of = [&](const ExecRange& e, uint64_t va) {
            uint32_t w;
            std::memcpy(&w, file + e.offset + (va - e.vaddr), 4);
            return w;
        };
        // The functions that are read: inside one executable range, whole words, and not overlapping an earlier one
        // (the ranges come from the file, so each word is taken once).
        std::vector<const ExecRange*> range(fns.size(), nullptr);
        uint64_t done_to = 0;
        for (size_t i = 0; i < fns.size(); ++i) {
            const FdeRange& f = fns[i];
            if (f.hi <= f.lo || f.lo < done_to || (f.lo & 3) || (f.hi & 3)) continue;
            for (const ExecRange& e : code.ranges) {
                if (f.lo >= e.vaddr && f.hi <= e.vaddr + e.size) range[i] = &e;
            }
            if (range[i]) done_to = f.hi;
        }
        for (size_t i = 0; i < fns.size(); ++i) {
            if (!range[i]) continue;
            for (uint64_t va = fns[i].lo; va < fns[i].hi; va += 4) {
                const uint32_t w = word_of(*range[i], va);
                uint64_t t;
                if (!direct_target(w, va, &t) || is_bl(w)) continue;
                targets.push_back(t);
                if (t < fns[i].lo || t >= fns[i].hi) {
                    const size_t j = function_at(t);
                    if (j != fns.size()) parent[find_root(i)] = find_root(j);
                }
            }
        }
        std::sort(targets.begin(), targets.end());
        auto is_target = [&](uint64_t va) { return std::binary_search(targets.begin(), targets.end(), va); };
        std::vector<uint32_t> live(fns.size(), 0);
        for (size_t i = 0; i < fns.size(); ++i) {
            if (!range[i]) {
                live[i] = kAll;
                continue;
            }
            const size_t n = static_cast<size_t>((fns[i].hi - fns[i].lo) / 4);
            std::vector<uint32_t> fw(n);
            for (size_t k = 0; k < n; ++k) fw[k] = word_of(*range[i], fns[i].lo + k * 4);
            size_t budget = 64 * n + 4096;  // words looked at in all, so hostile code cannot make this quadratic
            bool start = true;              // the word is a block start
            for (size_t k = 0; k < n && live[i] != kAll; ++k) {
                const uint64_t va = fns[i].lo + k * 4;
                const bool block_start = start;
                // A case label can follow a call that does not return, or a trap, as well as a branch.
                start = ends_block(fw[k]) || is_call(fw[k]) || (fw[k] & 0xffe0001f) == 0xd4200000 || fw[k] >> 16 == 0;
                if (!block_start && !is_target(va)) continue;
                // A function that nothing branches to is entered by calls, with the arguments and the link register
                // live; a table dispatch does not land there. One that is branched to (a cold part, a tail call)
                // may be landed on with anything live.
                const size_t at_fn = function_at(va);
                uint32_t seen = at_fn != fns.size() && fns[at_fn].lo == va && !is_target(va) ? kArgs | kLink : 0;
                for (size_t m = k, steps = 0; m < n; ++m, ++steps, --budget) {
                    const uint32_t x = fw[m];
                    uint32_t reads, writes;
                    x18_gpr_access(x, &reads, &writes);
                    uint64_t t;
                    const bool direct = direct_target(x, fns[i].lo + m * 4, &t) && !is_bl(x);
                    const size_t j = direct ? function_at(t) : fns.size();
                    const bool call = is_call(x);
                    if ((x & 0xff000000) == 0xd4000000) reads |= kArgs;  // svc: the system call's arguments
                    // What leaves the function takes the link register along, and what it needs: a return its
                    // results; a call, a tail call or an indirect branch that is not a table dispatch the arguments.
                    if (is_ret(x)) reads |= kLink | 0x3;
                    const bool leaves = direct && (j == fns.size() || fns[j].lo == t);
                    if ((is_br(x) && !x18_table_dispatch(fw.data(), m)) || leaves) reads |= kLink | kArgs;
                    if (call) reads |= kArgs;
                    live[i] |= reads & kAll & ~seen;
                    seen |= (reads | writes) & kAll;
                    if (call) seen |= kAll;  // a call clobbers all of them
                    if ((seen & kAll) == kAll || ends_block(x)) break;
                    if (steps >= kScanLimit || budget == 0) {
                        live[i] |= kAll & ~seen;
                        break;
                    }
                }
                if (budget == 0) live[i] = kAll;
            }
        }
        group_live.assign(fns.size(), 0);
        for (size_t i = 0; i < fns.size(); ++i) group_live[find_root(i)] |= live[i];
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
        for (size_t fi = 0; fi < fns.size(); ++fi) {
            const FdeRange& f = fns[fi];
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
                X18Site site{va, off, w[i], 0xff};
                const X18Operands::Kind kind = x18_operands(w[i]).kind;
                if (kind == X18Operands::IndirectCall) {
                    site.borrow = 16;  // x16 may be clobbered by any call (AAPCS64 intra-procedure-call scratch)
                } else if (kind == X18Operands::IndirectBranch) {
                    if (parent.empty()) build_groups();
                    // The link register is borrowed only for a table dispatch (an indexed load, then an add of the
                    // base), never for a tail call through a pointer, which needs it to return.
                    uint32_t live = group_live[find_root(fi)];
                    if (!x18_table_dispatch(w.data(), i)) live |= 1u << 30 | 0x1ff;
                    site.borrow = x18_pick_borrow(live);
                }
                plan.sites.push_back(site);
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
