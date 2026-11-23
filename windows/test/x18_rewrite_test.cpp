// x18_rewrite_test.cpp -- virtual x18 (include/winapk/arm64_x18_rewrite.h).
//
// The encodings are llvm-mc's for the text beside each. The thunks are executed: each case runs the generated
// thunk on this thread with chosen register, flag and memory contents, and checks the virtual x18, the outputs, and
// that every other register and the flags are as they were. Needs Windows on Arm (x18 is the TEB here).

#include "winapk/arm64_x18_rewrite.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <thread>

using namespace winapk::arm64;

namespace {

int g_fail = 0;
void check(bool ok, const char* what, const char* detail = "") {
    if (!ok) {
        ++g_fail;
        std::printf("FAIL: %s %s\n", what, detail);
    }
}

// ---- decoding ------------------------------------------------------------------------------------------

struct Decode {
    uint32_t word;
    X18Operands::Kind kind;
    uint8_t fields;
    bool reads, writes;
    const char* text;
};
constexpr auto kRw = X18Operands::Rewritable;
constexpr auto kCb = X18Operands::CompareBranch;
constexpr auto kNo = X18Operands::None;
constexpr auto kUn = X18Operands::Unsupported;
constexpr auto kIb = X18Operands::IndirectBranch;
constexpr auto kIc = X18Operands::IndirectCall;
constexpr auto kPc = X18Operands::PcRelAddress;
constexpr uint8_t D = X18Operands::Rd, N = X18Operands::Rn, M = X18Operands::Rm, A = X18Operands::Ra;

const Decode kDecode[] = {
    {0x91000652, kRw, D | N, true, true, "add x18, x18, #1"},
    {0xaa0003f2, kRw, D, false, true, "mov x18, x0"},
    {0xaa1203e0, kRw, M, true, false, "mov x0, x18"},
    {0x528000b2, kRw, D, false, true, "mov w18, #5"},
    {0xf2b7ddf2, kRw, D, true, true, "movk x18, #0xbeef, lsl #16"},
    {0xb3781c12, kRw, D, true, true, "bfi x18, x0, #8, #8"},
    {0xf8008642, kRw, N, true, true, "str x2, [x18], #8"},
    {0xf85f8e42, kRw, N, true, true, "ldr x2, [x18, #-8]!"},
    {0xa9400640, kRw, N, true, false, "ldp x0, x1, [x18]"},
    {0xa9004822, kRw, A, true, false, "stp x2, x18, [x1]"},
    {0xa9404c32, kRw, D, false, true, "ldp x18, x19, [x1]"},
    {0xa9bf4c32, kRw, D, true, false, "stp x18, x19, [x1, #-16]!"},
    {0xb8727822, kRw, M, true, false, "ldr w2, [x1, x18, lsl #2]"},
    {0xfa400240, kRw, N, true, false, "ccmp x18, x0, #0, eq"},
    {0xd53b4212, kRw, D, false, true, "mrs x18, nzcv"},
    {0x9a920020, kRw, M, true, false, "csel x0, x1, x18, eq"},
    {0x9b024820, kRw, A, true, false, "madd x0, x1, x2, x18"},
    {0x0d409240, kRw, N, true, false, "ld1 { v0.s }[1], [x18]"},
    {0xf8800240, kRw, N, true, false, "prfum pldl1keep, [x18]"},
    {0xb4000072, kCb, D, true, false, "cbz x18, #12"},
    {0x36180072, kCb, D, true, false, "tbz w18, #3, #12"},
    {0x91000400, kNo, 0, false, false, "add x0, x0, #1"},
    {0xd61f0240, kIb, N, true, false, "br x18"},
    {0xd65f0240, kIb, N, true, false, "ret x18"},
    {0xd63f0240, kIc, N, true, false, "blr x18"},
    {0x10000812, kPc, D, false, true, "adr x18, #256"},
    {0x10fffe12, kPc, D, false, true, "adr x18, #-64"},
    {0xb0000012, kPc, D, false, true, "adrp x18, #4096"},
    {0xd0fffff2, kPc, D, false, true, "adrp x18, #-8192"},
    {0xd61f0200, kNo, 0, false, false, "br x16"},
};

// ---- running a thunk ---------------------------------------------------------------------------------------

// r[0..17] = x0..x17, r[19..28] = x19..x28, r[29] = nzcv. In, then out. x28 holds the return address while the
// code runs and its input value after.
extern "C" __attribute__((naked)) void run_regs(void* fn, uint64_t* r) {
    __asm__ volatile(
        "sub sp, sp, #112\n"
        "stp x29, x30, [sp]\n"
        "stp x19, x20, [sp, #16]\n"
        "stp x21, x22, [sp, #32]\n"
        "stp x23, x24, [sp, #48]\n"
        "stp x25, x26, [sp, #64]\n"
        "stp x27, x28, [sp, #80]\n"
        "mov x30, x0\n"
        "mov x29, x1\n"
        "ldr x0, [x29, #232]\n"
        "msr nzcv, x0\n"
        "ldp x0, x1, [x29, #0]\n"
        "ldp x2, x3, [x29, #16]\n"
        "ldp x4, x5, [x29, #32]\n"
        "ldp x6, x7, [x29, #48]\n"
        "ldp x8, x9, [x29, #64]\n"
        "ldp x10, x11, [x29, #80]\n"
        "ldp x12, x13, [x29, #96]\n"
        "ldp x14, x15, [x29, #112]\n"
        "ldp x16, x17, [x29, #128]\n"
        "ldp x19, x20, [x29, #152]\n"
        "ldp x21, x22, [x29, #168]\n"
        "ldp x23, x24, [x29, #184]\n"
        "ldp x25, x26, [x29, #200]\n"
        "ldp x27, x28, [x29, #216]\n"
        "adr x28, 2f\n"
        "blr x30\n"
        "2:\n"
        "ldr x28, [x29, #224]\n"
        "stp x0, x1, [x29, #0]\n"
        "stp x2, x3, [x29, #16]\n"
        "stp x4, x5, [x29, #32]\n"
        "stp x6, x7, [x29, #48]\n"
        "stp x8, x9, [x29, #64]\n"
        "stp x10, x11, [x29, #80]\n"
        "stp x12, x13, [x29, #96]\n"
        "stp x14, x15, [x29, #112]\n"
        "stp x16, x17, [x29, #128]\n"
        "stp x19, x20, [x29, #152]\n"
        "stp x21, x22, [x29, #168]\n"
        "stp x23, x24, [x29, #184]\n"
        "stp x25, x26, [x29, #200]\n"
        "stp x27, x28, [x29, #216]\n"
        "mrs x0, nzcv\n"
        "str x0, [x29, #232]\n"
        "ldp x19, x20, [sp, #16]\n"
        "ldp x21, x22, [sp, #32]\n"
        "ldp x23, x24, [sp, #48]\n"
        "ldp x25, x26, [sp, #64]\n"
        "ldp x27, x28, [sp, #80]\n"
        "ldp x29, x30, [sp]\n"
        "add sp, sp, #112\n"
        "ret\n" ::: "memory");
}

constexpr uint64_t kBuf = 0xb0f0000000000000ull;  // kBuf + n: the address of byte n of the memory block
// kCode + n: n bytes from the start of the code block, which may be negative (the bias keeps the marker intact).
constexpr uint64_t kCodeBias = 0x10000000;
constexpr uint64_t kCode = 0xc0de000000000000ull + kCodeBias;
constexpr size_t kCodeSize = 0x4000, kSiteAt = 0x400, kBranchTarget = 0x200, kCallee = 0x240, kThunkAt = 0x2100;
constexpr uint32_t kN = 0x80000000, kZ = 0x40000000, kC = 0x20000000;

struct Reg {
    int idx;
    uint64_t val;
};

struct Run {
    const char* text;
    uint32_t word;
    uint64_t v_in;
    std::vector<Reg> in;
    uint32_t flags_in;
    uint64_t v_out;
    std::vector<Reg> out;   // registers expected to change; every other register must be as it was
    uint32_t flags_out;
    bool check_mem;
    uint64_t mem_out[4];
    uint8_t borrow = 16;  // the register an indirect branch or call clobbers
};

constexpr uint64_t kMem0[4] = {0x0000222200001111ull, 0x3333, 0x4444, 0x5555};

DWORD g_v = 0, g_s = 0;
uint32_t g_voff = 0, g_soff = 0;
uint8_t* g_code = nullptr;

uint64_t resolve(uint64_t v, const uint64_t* mem) {
    if ((v & 0xffff000000000000ull) == kBuf) return reinterpret_cast<uint64_t>(mem) + (v & 0xffff);
    if ((v & 0xffff000000000000ull) == 0xc0de000000000000ull) {
        return reinterpret_cast<uint64_t>(g_code) + (v & 0xffffffffffffull) - kCodeBias;
    }
    return v;
}

void run_case(const Run& c) {
    char name[160];
    std::snprintf(name, sizeof name, "[%s]", c.text);
    X18Slots slots{g_voff, g_soff};
    std::vector<uint32_t> thunk;
    std::string why;
    const uint64_t base = reinterpret_cast<uint64_t>(g_code);
    const uint64_t site = base + kSiteAt, at = base + kThunkAt;
    uint32_t jump = 0;
    if (!make_x18_thunk(c.word, site, at, slots, c.borrow, &thunk, &jump, &why)) {
        check(false, name, why.c_str());
        return;
    }
    check(thunk.size() <= kX18ThunkMaxWords, name, "thunk size");
    // Thunks return through x28, not x30: blr x18 has overwritten x30.
    const uint32_t site_code[] = {jump, 0xd280002f /* movz x15, #1 */, 0xd61f0380 /* br x28 */,
                                  0xd280004f /* movz x15, #2 */, 0xd61f0380};
    const uint32_t branch_target[] = {0xd280006f /* movz x15, #3 */, 0xd61f0380 /* br x28 */};
    const uint32_t callee[] = {0xaa1e03ed /* mov x13, x30 */, 0xd28000ee /* movz x14, #7 */, 0xd65f03c0 /* ret */};
    std::memcpy(g_code + kSiteAt, site_code, sizeof site_code);
    std::memcpy(g_code + kBranchTarget, branch_target, sizeof branch_target);
    std::memcpy(g_code + kCallee, callee, sizeof callee);
    std::memcpy(g_code + kThunkAt, thunk.data(), thunk.size() * 4);
    FlushInstructionCache(GetCurrentProcess(), g_code, kCodeSize);

    uint64_t mem[4];
    std::memcpy(mem, kMem0, sizeof mem);
    uint64_t r[30] = {};
    // Distinct, and none looks like an address.
    for (int i = 0; i < 29; ++i) r[i] = 0x0101010101010101ull * (i + 1) + 0x1000000000000000ull;
    r[18] = 0;
    for (const Reg& x : c.in) r[x.idx] = resolve(x.val, mem);
    r[29] = c.flags_in;
    uint64_t before[30];
    std::memcpy(before, r, sizeof r);
    TlsSetValue(g_v, reinterpret_cast<void*>(resolve(c.v_in, mem)));
    TlsSetValue(g_s, reinterpret_cast<void*>(0x5eed5eed5eed5eedull));

    const uint64_t teb = reinterpret_cast<uint64_t>(NtCurrentTeb());
    run_regs(g_code + kSiteAt, r);

    uint64_t expect[30];
    std::memcpy(expect, before, sizeof expect);
    expect[15] = 1;  // the fall-through marker the site code sets
    for (const Reg& x : c.out) expect[x.idx] = resolve(x.val, mem);
    expect[29] = (c.flags_out == 0xffffffff ? c.flags_in : c.flags_out);
    for (int i = 0; i < 29; ++i) {
        if (i == 18) continue;
        if (r[i] != expect[i]) {
            char d[96];
            std::snprintf(d, sizeof d, "x%d = %llx, expected %llx", i, static_cast<unsigned long long>(r[i]),
                          static_cast<unsigned long long>(expect[i]));
            check(false, name, d);
        }
    }
    check((r[29] & 0xf0000000u) == (expect[29] & 0xf0000000u), name, "flags");
    const uint64_t v = reinterpret_cast<uint64_t>(TlsGetValue(g_v));
    if (v != resolve(c.v_out, mem)) {
        char d[96];
        std::snprintf(d, sizeof d, "virtual x18 = %llx, expected %llx", static_cast<unsigned long long>(v),
                      static_cast<unsigned long long>(resolve(c.v_out, mem)));
        check(false, name, d);
    }
    check(reinterpret_cast<uint64_t>(NtCurrentTeb()) == teb, name, "teb");
    if (c.check_mem) {
        for (int i = 0; i < 4; ++i) check(mem[i] == c.mem_out[i], name, "memory");
    }
}

// x15 is the marker for which way a compare-and-branch went: 1 falls through, 2 is taken, 3 is the target of br x18.
std::vector<Run> cases() {
    const uint64_t* m = kMem0;
    (void)m;
    constexpr uint32_t any = 0xffffffff;
    std::vector<Run> v;
    auto add = [&](Run r) { v.push_back(r); };
    add({"add x18, x18, #1", 0x91000652, 5, {}, 0, 6, {}, any, false, {}});
    add({"mov x18, x0", 0xaa0003f2, 9, {{0, 0x1234}}, 0, 0x1234, {}, any, false, {}});
    add({"mov x0, x18", 0xaa1203e0, 77, {}, 0, 77, {{0, 77}}, any, false, {}});
    add({"mov w18, #5", 0x528000b2, 0xffffffff00000000ull, {}, 0, 5, {}, any, false, {}});
    add({"add x0, x1, x18", 0x8b120020, 3, {{1, 10}}, 0, 3, {{0, 13}}, any, false, {}});
    add({"str x2, [x18], #8", 0xf8008642, kBuf + 0, {{2, 0xabcd}}, 0, kBuf + 8, {}, any, true,
         {0xabcd, 0x3333, 0x4444, 0x5555}});
    add({"ldr x2, [x18, #-8]!", 0xf85f8e42, kBuf + 8, {}, 0, kBuf + 0, {{2, 0x0000222200001111ull}}, any, false, {}});
    add({"ldp x0, x1, [x18]", 0xa9400640, kBuf + 0, {}, 0, kBuf + 0, {{0, 0x0000222200001111ull}, {1, 0x3333}}, any,
         false, {}});
    add({"stp x2, x18, [x1]", 0xa9004822, 0x99, {{1, kBuf + 0}, {2, 7}}, 0, 0x99, {{1, kBuf + 0}}, any, true,
         {7, 0x99, 0x4444, 0x5555}});
    add({"ldp x18, x19, [x1]", 0xa9404c32, 0xdead, {{1, kBuf + 0}}, 0, 0x0000222200001111ull,
         {{1, kBuf + 0}, {19, 0x3333}}, any, false, {}});
    add({"ldr x18, [x1, #8]", 0xf9400432, 0xdead, {{1, kBuf + 0}}, 0, 0x3333, {{1, kBuf + 0}}, any, false, {}});
    add({"ldr x18, [x18, #8]", 0xf9400652, kBuf + 0, {}, 0, 0x3333, {}, any, false, {}});
    add({"ldr w2, [x1, x18, lsl #2]", 0xb8727822, 1, {{1, kBuf + 0}}, 0, 1, {{1, kBuf + 0}, {2, 0x2222}}, any, false,
         {}});
    add({"mul x18, x0, x1", 0x9b017c12, 0, {{0, 6}, {1, 7}}, 0, 42, {}, any, false, {}});
    add({"madd x0, x1, x2, x18", 0x9b024820, 5, {{1, 3}, {2, 4}}, 0, 5, {{0, 17}}, any, false, {}});
    add({"csel x0, x1, x18, eq (equal)", 0x9a920020, 0x44, {{1, 0x11}}, kZ, 0x44, {{0, 0x11}}, any, false, {}});
    add({"csel x0, x1, x18, eq (not equal)", 0x9a920020, 0x44, {{1, 0x11}}, 0, 0x44, {{0, 0x44}}, any, false, {}});
    add({"cmp x18, x0", 0xeb00025f, 5, {{0, 5}}, 0, 5, {}, kZ | kC, false, {}});
    add({"ccmp x18, x0, #0, eq", 0xfa400240, 1, {{0, 2}}, kZ, 1, {}, kN, false, {}});
    add({"movk x18, #0xbeef, lsl #16", 0xf2b7ddf2, 0xaaaabbbb, {}, 0, 0xbeefbbbb, {}, any, false, {}});
    add({"bfi x18, x0, #8, #8", 0xb3781c12, 0xffffffff, {{0, 0}}, 0, 0xffff00ff, {{0, 0}}, any, false, {}});
    add({"ubfx x0, x18, #3, #4", 0xd3431a40, 0x78, {}, 0, 0x78, {{0, 0xf}}, any, false, {}});
    add({"lsl x18, x18, #3", 0xd37df252, 3, {}, 0, 24, {}, any, false, {}});
    add({"adds x18, x18, #1", 0xb1000652, ~0ull, {}, 0, 0, {}, kZ | kC, false, {}});
    add({"mrs x18, nzcv", 0xd53b4212, 0xffff, {}, 0xa0000000, 0xa0000000, {}, 0xa0000000, false, {}});
    add({"ldrb w18, [x1, #1]", 0x39400432, ~0ull, {{1, kBuf + 0}}, 0, 0x11, {{1, kBuf + 0}}, any, false, {}});
    add({"strb w18, [x1, #2]", 0x39000832, 0xab, {{1, kBuf + 0}}, 0, 0xab, {{1, kBuf + 0}}, any, true,
         {0x0000222200ab1111ull, 0x3333, 0x4444, 0x5555}});
    add({"stp x18, x19, [x1, #-16]!", 0xa9bf4c32, 5, {{1, kBuf + 16}, {19, 6}}, 0, 5, {{1, kBuf + 0}}, any, true,
         {5, 6, 0x4444, 0x5555}});
    add({"ldp x2, x3, [x18], #16", 0xa8c10e42, kBuf + 0, {}, 0, kBuf + 16,
         {{2, 0x0000222200001111ull}, {3, 0x3333}}, any, false, {}});
    // The instruction names the registers the thunk would borrow first, so it has to pick another.
    add({"add x9, x9, x18", 0x8b120129, 5, {{9, 100}}, 0, 5, {{9, 105}}, any, false, {}});
    add({"madd x9, x10, x11, x18", 0x9b0b4949, 5, {{10, 3}, {11, 4}}, 0, 5, {{9, 17}, {10, 3}, {11, 4}}, any, false,
         {}});
    add({"stp x9, x18, [x10]", 0xa9004949, 0x99, {{9, 7}, {10, kBuf + 0}}, 0, 0x99, {{9, 7}, {10, kBuf + 0}}, any, true,
         {7, 0x99, 0x4444, 0x5555}});
    add({"ldp x18, x9, [x10]", 0xa9402552, 0xdead, {{10, kBuf + 0}}, 0, 0x0000222200001111ull,
         {{9, 0x3333}, {10, kBuf + 0}}, any, false, {}});
    add({"ldr x9, [x10, x18]", 0xf8726949, 8, {{10, kBuf + 0}}, 0, 8, {{9, 0x3333}, {10, kBuf + 0}}, any, false, {}});
    add({"csel x17, x16, x18, eq", 0x9a920211, 0x44, {{16, 0x11}}, 0, 0x44, {{16, 0x11}, {17, 0x44}}, any, false, {}});
    add({"cbz x18, #12 (zero: taken)", 0xb4000072, 0, {}, 0, 0, {{15, 2}}, any, false, {}});
    add({"cbz x18, #12 (not zero)", 0xb4000072, 1, {}, 0, 1, {}, any, false, {}});
    add({"cbnz w18, #12 (low half zero: not taken)", 0x35000072, 0x100000000ull, {}, 0, 0x100000000ull, {}, any, false,
         {}});
    add({"cbnz w18, #12 (taken)", 0x35000072, 1, {}, 0, 1, {{15, 2}}, any, false, {}});
    add({"tbnz x18, #40, #12 (set: taken)", 0xb7400072, 1ull << 40, {}, 0, 1ull << 40, {{15, 2}}, any, false, {}});
    add({"tbnz x18, #40, #12 (clear)", 0xb7400072, 0, {}, 0, 0, {}, any, false, {}});
    add({"tbz w18, #3, #12 (clear: taken)", 0x36180072, 0, {}, 0, 0, {{15, 2}}, any, false, {}});
    add({"tbz w18, #3, #12 (set)", 0x36180072, 8, {}, 0, 8, {}, any, false, {}});
    add({"extr x0, x1, x18, #5", 0x93d21420, 0x20, {{1, 1}}, 0, 0x20, {{0, 0x0800000000000001ull}}, any, false, {}});
    add({"rbit x0, x18", 0xdac00240, 1, {}, 0, 1, {{0, 0x8000000000000000ull}}, any, false, {}});
    // br, blr and ret through x18: the target is the virtual x18; the borrowed register ends up holding it.
    add({"br x18", 0xd61f0240, kCode + kBranchTarget, {}, 0, kCode + kBranchTarget,
         {{15, 3}, {16, kCode + kBranchTarget}}, any, false, {}});
    add({"br x18 (through x17)", 0xd61f0240, kCode + kBranchTarget, {}, 0, kCode + kBranchTarget,
         {{15, 3}, {17, kCode + kBranchTarget}}, any, false, {}, 17});
    add({"br x18 (through the link register)", 0xd61f0240, kCode + kBranchTarget, {}, 0, kCode + kBranchTarget,
         {{15, 3}}, any, false, {}, 30});
    add({"ret x18", 0xd65f0240, kCode + kBranchTarget, {}, 0, kCode + kBranchTarget,
         {{15, 3}, {16, kCode + kBranchTarget}}, any, false, {}});
    add({"blr x18", 0xd63f0240, kCode + kCallee, {}, 0, kCode + kCallee,
         {{13, kCode + kSiteAt + 4}, {14, 7}, {16, kCode + kCallee}}, any, false, {}});
    // adr and adrp: the thunk is on another page than the site.
    add({"adr x18, #256", 0x10000812, 0x1234, {}, 0, kCode + kSiteAt + 256, {}, any, false, {}});
    add({"adr x18, #-64", 0x10fffe12, 0x1234, {}, 0, kCode + kSiteAt - 64, {}, any, false, {}});
    add({"adr x18, #1048572", 0x107ffff2, 0x1234, {}, 0, kCode + kSiteAt + 1048572, {}, any, false, {}});
    add({"adrp x18, #4096", 0xb0000012, 0x1234, {}, 0, kCode + 0x1000, {}, any, false, {}});
    add({"adrp x18, #1073741824", 0x90200012, 0x1234, {}, 0, kCode + 0x40000000, {}, any, false, {}});
    add({"adrp x18, #-8192", 0xd0fffff2, 0x1234, {}, 0, kCode - 0x2000, {}, any, false, {}});
    return v;
}

// ---- code or data ----------------------------------------------------------------------------------------

// A minimal ELF file: .text at 0x1000, .eh_frame at 0x2000 describing the given functions.
std::vector<uint8_t> make_elf(const std::vector<uint32_t>& text,
                              const std::vector<std::pair<uint32_t, uint32_t>>& fns) {
    std::vector<uint8_t> eh;
    auto put32 = [&](std::vector<uint8_t>& b, uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    // CIE: length, id 0, version 1, "zR", code align 4, data align -4, return register 30, augmentation length 1,
    // pcrel sdata4
    put32(eh, 16);
    put32(eh, 0);
    for (uint8_t b : {1, 0x7a, 0x52, 0, 4, 0x7c, 30, 1, 0x1b, 0, 0, 0}) eh.push_back(b);
    for (const auto& f : fns) {
        const size_t at = eh.size();
        put32(eh, 20);
        put32(eh, static_cast<uint32_t>(at + 4));  // back to the CIE at 0
        const size_t loc_at = eh.size();
        put32(eh, static_cast<uint32_t>(0x1000 + f.first * 4 - (0x2000 + loc_at)));
        put32(eh, f.second * 4);
        for (int i = 0; i < 8; ++i) eh.push_back(0);
        eh.resize(at + 24);
    }
    put32(eh, 0);
    const char names[] = "\0.text\0.eh_frame\0.shstrtab";
    std::vector<uint8_t> f(sizeof(winapk::elf::Ehdr));
    const size_t text_off = f.size();
    for (uint32_t w : text) put32(f, w);
    const size_t eh_off = f.size();
    f.insert(f.end(), eh.begin(), eh.end());
    const size_t str_off = f.size();
    f.insert(f.end(), names, names + sizeof names);
    while (f.size() % 8) f.push_back(0);
    const size_t sh_off = f.size();
    winapk::elf::Shdr sh[4] = {};
    sh[1] = {1, winapk::elf::kShtProgbits, winapk::elf::kShfExecinstr, 0x1000, text_off, text.size() * 4, 0, 0, 4, 0};
    sh[2] = {7, winapk::elf::kShtProgbits, 2, 0x2000, eh_off, eh.size(), 0, 0, 4, 0};
    sh[3] = {17, 3, 0, 0, str_off, sizeof names, 0, 0, 1, 0};
    f.resize(sh_off + sizeof sh);
    std::memcpy(f.data() + sh_off, sh, sizeof sh);
    winapk::elf::Ehdr eh_hdr = {};
    eh_hdr.e_ident[0] = winapk::elf::kMag0;
    eh_hdr.e_ident[1] = winapk::elf::kMag1;
    eh_hdr.e_ident[2] = winapk::elf::kMag2;
    eh_hdr.e_ident[3] = winapk::elf::kMag3;
    eh_hdr.e_machine = winapk::elf::kEmAarch64;
    eh_hdr.e_shoff = sh_off;
    eh_hdr.e_shentsize = sizeof(winapk::elf::Shdr);
    eh_hdr.e_shnum = 4;
    eh_hdr.e_shstrndx = 3;
    std::memcpy(f.data(), &eh_hdr, sizeof eh_hdr);
    return f;
}

constexpr uint32_t kRet = 0xd65f03c0, kNop = 0xd503201f, kX18Mov = 0xaa0003f2 /* mov x18, x0 */, kBrX0 = 0xd61f0000;

// The plan holds exactly the x18 instruction at word `index` of .text, and nothing makes it risky.
void check_one_site(const X18Plan& p, size_t index, const char* what) {
    const bool found = p.ok && p.sites.size() == 1 && !p.risky();
    check(found, what);
    if (!found) return;
    const X18Site& s = p.sites[0];
    check(s.vaddr == 0x1000 + index * 4, what, "address");
    check(s.offset == sizeof(winapk::elf::Ehdr) + index * 4, what, "file offset");
    check(s.word == kX18Mov, what, "word");
}

void test_plan() {
    {
        auto f = make_elf({kNop, kX18Mov, kRet}, {{0, 3}});
        check_one_site(plan_x18_sites(f.data(), f.size()), 1, "plan: a plain function");
    }
    // A word after a ret that nothing enters, in a function with no indirect branch: data.
    {
        auto f = make_elf({kNop, kRet, kX18Mov, kNop, kRet}, {{0, 5}});
        X18Plan p = plan_x18_sites(f.data(), f.size());
        check(p.ok && p.unreachable == 1 && p.risky(), "plan: unreachable word after ret");
    }
    // The same word, entered by a branch: code.
    {
        auto f = make_elf({0x54000040 /* b.eq .+8 */, kRet, kX18Mov, kRet}, {{0, 4}});
        check_one_site(plan_x18_sites(f.data(), f.size()), 2, "plan: a branch target after ret");
    }
    // A function with an indirect branch: a block after a ret is a block start.
    {
        auto f = make_elf({kBrX0, kRet, kX18Mov, kRet}, {{0, 4}});
        check_one_site(plan_x18_sites(f.data(), f.size()), 2, "plan: a block start in a function with br");
    }
    // A word a literal load reads: data. `ldr x1, .+12` at word 0 reads word 3.
    {
        auto f = make_elf({0x58000061 /* ldr x1, #12 */, kRet, kNop, kX18Mov, kNop, kRet}, {{0, 6}});
        X18Plan p = plan_x18_sites(f.data(), f.size());
        check(p.ok && p.literal_data == 1 && p.risky(), "plan: literal pool word");
    }
    // A word in reserved space in a block of a function with br: not code.
    {
        auto f = make_elf({kBrX0, kRet, 0x00000000, kX18Mov, kRet}, {{0, 5}});
        X18Plan p = plan_x18_sites(f.data(), f.size());
        check(p.ok && p.implausible == 1 && p.risky(), "plan: implausible block");
    }
    // Outside every function, and not text.
    {
        auto f = make_elf({kNop, kRet, kX18Mov}, {{0, 2}});
        X18Plan p = plan_x18_sites(f.data(), f.size());
        check(p.ok && p.outside_functions == 1 && p.risky(), "plan: x18 outside the unwind tables");
    }
    // Code outside the tables, a run of valid words ended by zero padding: read as a function of its own.
    {
        std::vector<uint32_t> text = {kNop, kRet};
        for (int i = 0; i < 4; ++i) text.push_back(kNop);
        text.push_back(kX18Mov);
        for (int i = 0; i < 4; ++i) text.push_back(kNop);
        text.push_back(kRet);
        text.push_back(0);
        auto f = make_elf(text, {{0, 2}});
        check_one_site(plan_x18_sites(f.data(), f.size()), 6, "plan: code outside the unwind tables");
        // The same word in a run cut short by a reserved word on each side stays refused.
        std::vector<uint32_t> cut = {kNop, kRet, 0, kNop, kX18Mov, kNop, 0, kRet};
        auto g = make_elf(cut, {{0, 2}});
        X18Plan p = plan_x18_sites(g.data(), g.size());
        check(p.ok && p.outside_functions == 1 && p.risky(), "plan: a short run outside the tables stays refused");
    }
    // x18 only in a table after the functions: nothing to rewrite, when the functions cover the code.
    {
        std::vector<uint32_t> text(8, kNop);
        text.push_back(kRet);
        text.push_back(kX18Mov);
        auto f = make_elf(text, {{0, 9}});
        check(x18_data_only(plan_x18_sites(f.data(), f.size())), "plan: x18 in a table outside the functions");
        auto g = make_elf({kNop, kRet, kX18Mov}, {{0, 2}});
        check(!x18_data_only(plan_x18_sites(g.data(), g.size())), "plan: too little code covered");
        auto h = make_elf({kNop, kX18Mov, kRet}, {{0, 3}});
        check(!x18_data_only(plan_x18_sites(h.data(), h.size())), "plan: a site in a function");
        check(!x18_data_only(plan_x18_sites(f.data(), 16)), "plan: not an ELF file");
        // High coverage does not make a library data-only when code outside the tables names x18: it is patched.
        std::vector<uint32_t> tail(40, kNop);
        tail.push_back(kRet);
        tail[5] = 0x94000000 | 36;  // bl to the first word after the function: the code outside is called
        for (uint32_t w : {kNop, kNop, kX18Mov, kNop, kRet, uint32_t{0}}) tail.push_back(w);
        auto t = make_elf(tail, {{0, 41}});
        const X18Plan tp = plan_x18_sites(t.data(), t.size());
        check(!x18_data_only(tp), "plan: code outside the tables at high coverage is not data");
        check_one_site(tp, 43, "plan: code outside the tables at high coverage is patched");
        // The same words that nothing calls are a table of constants kept in .text.
        tail[5] = kNop;
        auto u = make_elf(tail, {{0, 41}});
        check(x18_data_only(plan_x18_sites(u.data(), u.size())), "plan: uncalled code outside the tables is data");
    }
    // The register an indirect branch through x18 clobbers: one that nothing can still read where the branch may land.
    {
        constexpr uint32_t kBrX18 = 0xd61f0240, kReadX16 = 0xaa1003e0 /* mov x0, x16 */,
                           kWriteX16 = 0xaa0003f0 /* mov x16, x0 */, kB8 = 0x14000002, kBl8 = 0x94000002;
        auto site_borrow = [&](const std::vector<uint32_t>& text, const std::vector<std::pair<uint32_t, uint32_t>>& fns,
                               size_t index) {
            auto f = make_elf(text, fns);
            const X18Plan p = plan_x18_sites(f.data(), f.size());
            for (const X18Site& s : p.sites) {
                if (s.vaddr == 0x1000 + index * 4) return int(s.borrow);
            }
            return -1;
        };
        check(site_borrow({kNop, kBrX18, kRet}, {{0, 3}}, 1) == 16, "borrow: x16 when nothing uses it");
        check(site_borrow({kReadX16, kBrX18, kRet}, {{0, 3}}, 1) == 17, "borrow: x17 when x16 is read at the entry");
        check(site_borrow({kWriteX16, kBrX18, kRet}, {{0, 3}}, 1) == 16,
              "borrow: x16 when it is written before it is read");
        // After the branch, a block that reads x16 first (a case of a switch).
        check(site_borrow({kBrX18, kRet, kReadX16, kRet}, {{0, 4}}, 0) == 17, "borrow: x16 read in a later block");
        check(site_borrow({kBrX18, kRet, kWriteX16, kReadX16, kRet}, {{0, 5}}, 0) == 16,
              "borrow: x16 written first in the later block");
        check(site_borrow({kBrX18, kRet, kBl8, kRet}, {{0, 4}}, 0) == 16, "borrow: a call clobbers x16");
        check(site_borrow({kBrX18, kRet, kBl8, kReadX16, kRet}, {{0, 5}}, 0) == 17,
              "borrow: the word after a call is a landing point (a case label after a noreturn call)");
        std::vector<uint32_t> all = {kBrX18, kRet};
        for (uint32_t r = 9; r <= 17; ++r) all.push_back(0xaa0003e0 | r << 16);  // mov x0, xr
        all.push_back(kRet);
        check(site_borrow(all, {{0, uint32_t(all.size())}}, 0) == 0xff, "borrow: none when x9..x17 are all read");
        // A function joined to another by a direct branch shares what that one reads; a plain neighbour does not.
        check(site_borrow({kNop, kBrX18, kReadX16, kRet}, {{0, 2}, {2, 2}}, 1) == 16, "borrow: separate functions");
        check(site_borrow({kB8, kBrX18, kReadX16, kRet}, {{0, 2}, {2, 2}}, 1) == 17, "borrow: functions joined by b");
        check(site_borrow({kBl8, kBrX18, kReadX16, kRet}, {{0, 2}, {2, 2}}, 1) == 16,
              "borrow: a call does not join functions");
        check(x18_pick_borrow(0) == 16 && x18_pick_borrow(1u << 16 | 1u << 17) == 15, "borrow: pick order");
        // A table dispatch: an indexed load, an add of the base, other work, the branch.
        constexpr uint32_t kLdrsw = 0xb8ab798b /* ldrsw x11, [x12, x11, lsl #2] */,
                           kAdd = 0x8b0c0172 /* add x18, x11, x12 */, kLdp = 0xa8c17bfd /* ldp x29, x30, [sp], #16 */;
        const uint32_t dispatch[] = {kLdrsw, kAdd, kNop, kBrX18};
        check(x18_table_dispatch(dispatch, 3), "dispatch: load, add, br");
        const uint32_t pointer[] = {0xf86b798b /* ldr x11, [x12, x11, lsl #3] */, kNop, 0xd61f0160 /* br x11 */};
        check(!x18_table_dispatch(pointer, 2), "dispatch: a call through a loaded pointer is not one");
        const uint32_t no_load[] = {kNop, kAdd, kBrX18};
        check(!x18_table_dispatch(no_load, 2), "dispatch: an add without a table load");
        // Every x0..x17 is read in the block after the switch, the link register is reloaded: x30 is the one left.
        std::vector<uint32_t> sw = {kLdrsw, kAdd, kBrX18};
        for (uint32_t r = 0; r <= 17; ++r) sw.push_back(0xf100001f | r << 5);  // cmp xr, #0
        sw.push_back(kLdp);
        sw.push_back(kRet);
        check(site_borrow(sw, {{0, uint32_t(sw.size())}}, 2) == 30, "borrow: the link register in a non-leaf switch");
        // The same, but the function returns without reloading it: a leaf, whose link register is live.
        sw.pop_back();
        sw.pop_back();
        sw.push_back(kRet);
        check(site_borrow(sw, {{0, uint32_t(sw.size())}}, 2) == 0xff,
              "borrow: none in a leaf whose registers are read");
    }
    // Files that cannot be planned: the ELF comes from an APK, so none of these may be read out of bounds.
    const auto good = make_elf({kNop, kX18Mov, kRet}, {{0, 3}});
    winapk::elf::Ehdr hdr;
    std::memcpy(&hdr, good.data(), sizeof hdr);
    check(!plan_x18_sites(good.data(), sizeof hdr - 1).ok, "plan: shorter than an ELF header");
    check(!plan_x18_sites(good.data(), good.size() - 1).ok, "plan: section table cut short");
    {
        auto f = make_elf({kNop, kX18Mov, kRet}, {});
        check(!plan_x18_sites(f.data(), f.size()).ok, "plan: no function ranges");
    }
    {
        auto f = good;
        winapk::elf::Ehdr h = hdr;
        h.e_machine = 62;  // EM_X86_64
        std::memcpy(f.data(), &h, sizeof h);
        check(!plan_x18_sites(f.data(), f.size()).ok, "plan: not an arm64 file");
    }
    {
        auto f = good;
        winapk::elf::Shdr text;
        const size_t at = static_cast<size_t>(hdr.e_shoff) + sizeof text;
        std::memcpy(&text, f.data() + at, sizeof text);
        text.sh_offset = f.size();
        std::memcpy(f.data() + at, &text, sizeof text);
        X18Plan p = plan_x18_sites(f.data(), f.size());
        check(p.sites.empty(), "plan: .text past the end of the file");
    }
    {
        auto f = make_elf({kNop, kX18Mov, kRet}, {{0, 100}});
        check(plan_x18_sites(f.data(), f.size()).sites.empty(), "plan: a function range past the code");
    }
}

}  // namespace

int main() {
    g_v = TlsAlloc();
    g_s = TlsAlloc();
    if (g_v >= 64 || g_s >= 64) {
        std::printf("not run: the TLS indexes %lu, %lu are not in the TEB's inline array\n", g_v, g_s);
        return 2;
    }
    g_voff = 0x1480 + 8 * g_v;
    g_soff = 0x1480 + 8 * g_s;
    TlsSetValue(g_v, reinterpret_cast<void*>(0x1122334455667788ull));
    uint64_t seen;
    std::memcpy(&seen, reinterpret_cast<uint8_t*>(NtCurrentTeb()) + g_voff, 8);
    check(seen == 0x1122334455667788ull, "the TLS slot is at TEB + 0x1480 + 8 * index");

    for (const Decode& d : kDecode) {
        const X18Operands o = x18_operands(d.word);
        char detail[160];
        std::snprintf(detail, sizeof detail, "(kind %d fields %d reads %d writes %d)", o.kind, o.fields, o.reads,
                      o.writes);
        check(o.kind == d.kind && o.fields == d.fields &&
                  (d.kind == kNo || (o.reads == d.reads && o.writes == d.writes)),
              d.text, detail);
    }
    // Forms the rewrite refuses: exclusives, authenticated branches, ldr literal, an undefined prefetch form.
    for (uint32_t w : {0xc85f7e40u /* ldxr x0, [x18] */, 0xd61f0a40u /* braaz x18 */, 0x58000012u /* ldr x18, .+0 */,
                       0xd73f0a40u /* blraaz x18 */,
                       0xc8127c41u /* stxr w18, x1, [x2] */, 0xf88aea52u /* undefined: size 3, opc 2, op4 2 */}) {
        char t[32];
        std::snprintf(t, sizeof t, "unsupported %08x", w);
        check(x18_operands(w).kind == kUn, t);
        std::vector<uint32_t> out;
        std::string why;
        uint32_t patch;
        check(!make_x18_thunk(w, 0x1000, 0x2000, {0x1490, 0x1498}, 16, &out, &patch, &why) && out.empty(), t,
              "made a thunk");
    }
    {  // Forms that cannot be thunked are refused, with nothing appended.
        std::vector<uint32_t> out;
        std::string why;
        uint32_t patch;
        check(!make_x18_thunk(0xd61f0240, 0x1000, 0x2000, {0x1490, 0x1498}, 0xff, &out, &patch, &why) && out.empty(),
              "br x18 with no free register");
        const uint64_t distant = 0x1000 + (uint64_t{1} << 28);
        check(!make_x18_thunk(0xd63f0240, 0x1000, distant, {0x1490, 0x1498}, 16, &out, &patch, &why) && out.empty(),
              "blr x18 with the thunk out of branch range");
        // adrp x18, #-4 GB from a thunk 100 MB above the site: the page does not fit the thunk's adrp.
        check(!make_x18_thunk(0x90800012, 0x1000, 0x1000 + 0x6400000, {0x1490, 0x1498}, 16, &out, &patch, &why) &&
                  out.empty() && why.find("out of reach") != std::string::npos,
              "adrp out of reach");
        check(!make_x18_thunk(0xd63f0240, 0x1000, 0x2000, {0x1490, 0x1498}, 30, &out, &patch, &why) && out.empty(),
              "blr x18 may not borrow the link register");
        check(make_x18_thunk(0xd63f0240, 0x1000, 0x2000, {0x1490, 0x1498}, 16, &out, &patch, &why) &&
                  patch == 0x94000400 && out.size() == 2,
              "blr x18 patches the site with bl", "");
    }

    // Every word that names x18 as a general register is found by uses_x18 and the other way round; each form this
    // decoder accepts yields a thunk with no x18 left in it but the two slot accesses.
    uint64_t seed = 0x9e3779b97f4a7c15ull;
    size_t mismatch = 0, rewritable = 0, unsupported = 0, left = 0, refused = 0;
    for (size_t n = 0; n < 4000000; ++n) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        uint32_t w = static_cast<uint32_t>(seed >> 32);
        if (n & 1) {  // bias towards x18 in Rd or Rn
            const unsigned shift = n % 3 == 0 ? 5 : 0;
            w = (w & ~(31u << shift)) | (18u << shift);
        }
        const X18Operands o = x18_operands(w);
        const bool uses = uses_x18(w);
        if ((o.kind == kNo) == uses) ++mismatch;
        if (o.kind != kNo && o.kind != kUn) {
            ++rewritable;
            std::vector<uint32_t> out;
            std::string why;
            uint32_t patch;
            if (!make_x18_thunk(w, 0x10000000, 0x10100000, {0x1490, 0x1498}, 16, &out, &patch, &why)) {
                // An adrp whose target is 4 GB from the thunk is out of reach by design.
                if (o.kind != kPc || why.find("out of reach") == std::string::npos) ++refused;
                continue;
            }
            for (uint32_t t : out) {
                const bool slot = (t & 0xffc003e0) == 0xf9400240 || (t & 0xffc003e0) == 0xf9000240;
                if (uses_x18(t) && !slot) ++left;
            }
        } else if (o.kind == kUn) {
            ++unsupported;
        }
    }
    std::printf("random words: %zu accepted (%zu with no thunk), %zu unsupported, %zu where the decoders disagree, "
                "%zu thunks with x18 left\n",
                rewritable, refused, unsupported, mismatch, left);
    check(mismatch == 0, "x18_operands agrees with uses_x18");
    check(refused == 0, "every accepted random word gets a thunk");
    check(left == 0, "thunks of random rewritable words");

    g_code = static_cast<uint8_t*>(VirtualAlloc(nullptr, kCodeSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    check(g_code != nullptr, "executable memory");
    if (g_code) {
        const auto all = cases();
        for (const Run& c : all) run_case(c);
        std::printf("%zu thunks executed\n", all.size());

        // 16 threads, each with its own virtual x18, in one process.
        std::atomic<int> bad{0};
        std::vector<std::thread> th;
        // The code page is shared, so each thread has a copy of the site and thunk of `add x18, x18, #1`.
        for (int t = 0; t < 16; ++t) {
            th.emplace_back([&, t] {
                uint8_t* page = static_cast<uint8_t*>(
                    VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
                std::vector<uint32_t> mine;
                std::string w;
                const uint64_t site_at = reinterpret_cast<uint64_t>(page), thunk_at = site_at + 0x100;
                uint32_t jump = 0;
                if (!page || !make_x18_thunk(0x91000652, site_at, thunk_at, {g_voff, g_soff}, 16, &mine, &jump, &w)) {
                    ++bad;
                    return;
                }
                const uint32_t site[] = {jump, 0xd65f03c0};
                std::memcpy(page, site, sizeof site);
                std::memcpy(page + 0x100, mine.data(), mine.size() * 4);
                FlushInstructionCache(GetCurrentProcess(), page, 0x200);
                const uint64_t teb = reinterpret_cast<uint64_t>(NtCurrentTeb());
                const uint64_t start = static_cast<uint64_t>(t + 1) << 40;
                TlsSetValue(g_v, reinterpret_cast<void*>(start));
                uint64_t r[30] = {};
                for (int i = 0; i < 200000; ++i) {
                    for (int k = 0; k < 29; ++k) r[k] = 0x7000 + k + i;
                    run_regs(page, r);
                    for (int k = 0; k < 29; ++k) {
                        if (k != 18 && r[k] != uint64_t(0x7000 + k + i)) ++bad;
                    }
                    if (reinterpret_cast<uint64_t>(NtCurrentTeb()) != teb) ++bad;
                }
                if (reinterpret_cast<uint64_t>(TlsGetValue(g_v)) != start + 200000) ++bad;
            });
        }
        for (auto& x : th) x.join();
        check(bad == 0, "16 threads, each with its own virtual x18");
    }

    test_plan();
    std::printf(g_fail ? "x18 rewrite test: %d failure(s)\n" : "x18 rewrite test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
