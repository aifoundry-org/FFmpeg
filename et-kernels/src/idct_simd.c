/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Eight-column ET-SoC-1 integer SIMD simple IDCT.
 *
 * Fixed-point algorithm from libavcodec/simple_idct_template.c,
 * Copyright (c) 2001 Michael Niedermayer <michaelni@gmx.at>.
 *
 * ET's .pi instructions operate on eight 32-bit INTEGER lanes in the custom
 * FP register file.  No floating-point arithmetic/conversions are used.
 * fmul.pi retains the low 32 bits; add/sub wrap; fsrai.pi is signed.  This is
 * exactly FFmpeg's SUINT arithmetic, including arbitrary int16_t inputs.
 *
 * Primary ISA references (sibling et-platform):
 *   sw-sysemu/insns/packed_arith.cpp: fmul_pi, fadd_pi, fsub_pi, fsrai_pi,
 *                                  fsatu8_pi, fandi_pi
 *   sw-sysemu/insns/packed_loadstore.cpp: fgh_ps, fgb_ps, fscb_ps
 *   sw-sysemu/insns/packed_mask.cpp: mova_x_m, mov_m_x
 *   dnn-library/include/internal/LoadStore.h: unaligned gather/scatter syntax
 *
 * The default path uses general gathers for merely 8-byte-aligned blocks.
 * ET_IDCT_PACKED enables FG32 gathers and packed two-word output stores only
 * after proving their modulo-32 addressing is safe at runtime. No unaligned
 * FLW/FSW.PS assumption is made. FLW.PS reads only our explicitly aligned
 * offset tables. Every access is within the block / eight destination bytes.
 * ET_IDCT_ROWS optionally adds the exact eight-rows-in-parallel row pass.
 */
#include "idct_simd.h"

/* A single instruction schedule is shared with the native exact lane model.
 * Native model success does not constitute device execution validation.
 * Only caller-saved FP registers are used: no scalar ABI save/restore traffic.
 * Registers: current row 0; a0..a3 1..4; b0..b3 5,6,7,10;
 * W1,W2,W3,W4,W5,W6,W7 11..17; temporaries 28,29; offsets 31.
 * Reading one input row at a time keeps all eight-column accumulators live. */
#define ET_IDCT_COL_OPS(LOAD) \
    BC(11, 22725) BC(12, 21407) BC(13, 19266) BC(14, 16383) \
    BC(15, 12873) BC(16, 8867) BC(17, 4520) \
    LOAD(0, 0) AI(0, 0, 32) \
    MUL(1, 14, 0) MOV(2, 1) MOV(3, 1) MOV(4, 1) \
    LOAD(0, 1) \
    MUL(5, 11, 0) MUL(6, 13, 0) MUL(7, 15, 0) MUL(10, 17, 0) \
    LOAD(0, 2) \
    MUL(28, 12, 0) MUL(29, 16, 0) \
    ADD(1, 1, 28) ADD(2, 2, 29) SUB(3, 3, 29) SUB(4, 4, 28) \
    LOAD(0, 3) \
    MUL(28, 13, 0) ADD(5, 5, 28) \
    MUL(28, 17, 0) SUB(6, 6, 28) \
    MUL(28, 11, 0) SUB(7, 7, 28) \
    MUL(28, 15, 0) SUB(10, 10, 28) \
    LOAD(0, 4) MUL(28, 14, 0) \
    ADD(1, 1, 28) SUB(2, 2, 28) SUB(3, 3, 28) ADD(4, 4, 28) \
    LOAD(0, 5) \
    MUL(28, 15, 0) ADD(5, 5, 28) \
    MUL(28, 11, 0) SUB(6, 6, 28) \
    MUL(28, 17, 0) ADD(7, 7, 28) \
    MUL(28, 13, 0) ADD(10, 10, 28) \
    LOAD(0, 6) MUL(28, 16, 0) MUL(29, 12, 0) \
    ADD(1, 1, 28) SUB(2, 2, 29) ADD(3, 3, 29) SUB(4, 4, 28) \
    LOAD(0, 7) \
    MUL(28, 17, 0) ADD(5, 5, 28) \
    MUL(28, 15, 0) SUB(6, 6, 28) \
    MUL(28, 13, 0) ADD(7, 7, 28) \
    MUL(28, 11, 0) SUB(10, 10, 28) \
    SUB(28, 1, 5) ADD(1, 1, 5) MOV(5, 28) \
    SUB(28, 2, 6) ADD(2, 2, 6) MOV(6, 28) \
    SUB(28, 3, 7) ADD(3, 3, 7) MOV(7, 28) \
    SUB(28, 4, 10) ADD(4, 4, 10) MOV(10, 28) \
    ASR(1, 1, 20) ASR(2, 2, 20) ASR(3, 3, 20) ASR(4, 4, 20) \
    ASR(10, 10, 20) ASR(7, 7, 20) ASR(6, 6, 20) ASR(5, 5, 20)

/* Row lanes are independent rows, with transposed halfword gather/scatter.
 * f29 ORs AC inputs; f30 holds the exact DC*8 shortcut.  The normal butterfly
 * still computes all lanes, then selects DC*8 only for all-AC-zero lanes.
 * Only low 16 bits are stored: this is wrapping, NOT saturating narrowing. */
#define SELECT_DC(d) AND(d, d, 29) OR(d, d, 30)
#define ET_IDCT_ROW_OPS \
    BC(11, 22725) BC(12, 21407) BC(13, 19266) BC(14, 16383) \
    BC(15, 12873) BC(16, 8867) BC(17, 4520) \
    BC(29, 0) RLD(0, 0) LSL(30, 0, 3) BC(28, 1024) \
    MUL(1, 14, 0) ADD(1, 1, 28) MOV(2, 1) MOV(3, 1) MOV(4, 1) \
    RLD(0, 1) \
    MUL(5, 11, 0) MUL(6, 13, 0) MUL(7, 15, 0) MUL(10, 17, 0) \
    RLD(0, 2) \
    MUL(28, 12, 0) ADD(1, 1, 28) SUB(4, 4, 28) \
    MUL(28, 16, 0) ADD(2, 2, 28) SUB(3, 3, 28) \
    RLD(0, 3) \
    MUL(28, 13, 0) ADD(5, 5, 28) \
    MUL(28, 17, 0) SUB(6, 6, 28) \
    MUL(28, 11, 0) SUB(7, 7, 28) \
    MUL(28, 15, 0) SUB(10, 10, 28) \
    RLD(0, 4) MUL(28, 14, 0) \
    ADD(1, 1, 28) SUB(2, 2, 28) SUB(3, 3, 28) ADD(4, 4, 28) \
    RLD(0, 5) \
    MUL(28, 15, 0) ADD(5, 5, 28) \
    MUL(28, 11, 0) SUB(6, 6, 28) \
    MUL(28, 17, 0) ADD(7, 7, 28) \
    MUL(28, 13, 0) ADD(10, 10, 28) \
    RLD(0, 6) MUL(28, 16, 0) ADD(1, 1, 28) SUB(4, 4, 28) \
    MUL(28, 12, 0) SUB(2, 2, 28) ADD(3, 3, 28) \
    RLD(0, 7) \
    MUL(28, 17, 0) ADD(5, 5, 28) \
    MUL(28, 15, 0) SUB(6, 6, 28) \
    MUL(28, 13, 0) ADD(7, 7, 28) \
    MUL(28, 11, 0) SUB(10, 10, 28) \
    SUB(28, 1, 5) ADD(1, 1, 5) MOV(5, 28) \
    SUB(28, 2, 6) ADD(2, 2, 6) MOV(6, 28) \
    SUB(28, 3, 7) ADD(3, 3, 7) MOV(7, 28) \
    SUB(28, 4, 10) ADD(4, 4, 10) MOV(10, 28) \
    ASR(1, 1, 11) ASR(2, 2, 11) ASR(3, 3, 11) ASR(4, 4, 11) \
    ASR(10, 10, 11) ASR(7, 7, 11) ASR(6, 6, 11) ASR(5, 5, 11) \
    BC(0, 0) EQ(29, 29, 0) AND(30, 30, 29) NOT(29, 29) \
    SELECT_DC(1) SELECT_DC(2) SELECT_DC(3) SELECT_DC(4) \
    SELECT_DC(10) SELECT_DC(7) SELECT_DC(6) SELECT_DC(5)

#if ET_IDCT_PACKED
#define COLUMN_LINK static
#define COLUMN_NAME et_idct_columns_generic
#else
#define COLUMN_LINK
#define COLUMN_NAME et_idct_simd_columns
#endif

#if defined(ET_IDCT_SIMD_MODEL) && ET_IDCT_SIMD_MODEL

/* Explicit unsigned modular arithmetic; ASR does not depend on the native
 * implementation's conversion of uint32_t to int32_t or signed right shift. */
#define LANES(d, expression) do { \
    for (unsigned lane = 0; lane < 8; ++lane) r[d][lane] = (expression); \
} while (0);
#define LD(d, row) LANES(d, (uint32_t)(int32_t)block[(row) * 8 + lane])
#define BC(d, imm) LANES(d, (uint32_t)(imm))
#define AI(d, a, imm) LANES(d, r[a][lane] + (uint32_t)(imm))
#define MOV(d, a) LANES(d, r[a][lane])
#define MUL(d, a, b) LANES(d, r[a][lane] * r[b][lane])
#define ADD(d, a, b) LANES(d, r[a][lane] + r[b][lane])
#define SUB(d, a, b) LANES(d, r[a][lane] - r[b][lane])
#define OR(d, a, b) LANES(d, r[a][lane] | r[b][lane])
#define AND(d, a, b) LANES(d, r[a][lane] & r[b][lane])
#define EQ(d, a, b) LANES(d, r[a][lane] == r[b][lane] ? UINT32_MAX : 0u)
#define NOT(d, a) LANES(d, ~r[a][lane])
#define LSL(d, a, n) LANES(d, r[a][lane] << (n))
#define RLD(d, col) LANES(d, (uint32_t)(int32_t)block[lane * 8 + (col)]) \
    if (col) { OR(29, 29, d) }
#define ASR(d, a, n) LANES(d, (r[a][lane] >> (n)) | \
    ((0u - (r[a][lane] >> 31)) << (32 - (n))))

#if ET_IDCT_ROWS
void et_idct_simd_rows(int16_t *block)
{
    static const unsigned result_regs[8] = {1, 2, 3, 4, 10, 7, 6, 5};
    uint32_t r[31][8];
    ET_IDCT_ROW_OPS
    for (unsigned col = 0; col < 8; ++col) {
        for (unsigned row = 0; row < 8; ++row) {
            uint32_t bits = r[result_regs[col]][row] & 65535u;
            block[row * 8 + col] = bits < 32768u ? (int)bits : (int)bits - 65536;
        }
    }
}
#endif

COLUMN_LINK void COLUMN_NAME(uint8_t *dest, ptrdiff_t stride,
                          const int16_t *block, int add)
{
    static const unsigned result_regs[8] = {1, 2, 3, 4, 10, 7, 6, 5};
    uint32_t r[30][8];
    ET_IDCT_COL_OPS(LD)
    for (unsigned y = 0; y < 8; ++y) {
        for (unsigned x = 0; x < 8; ++x) {
            uint32_t bits = r[result_regs[y]][x];
            int value = bits <= INT32_MAX ? (int)bits :
                -1 - (int)(UINT32_MAX - bits);
            if (add)
                value += dest[x];
            dest[x] = value < 0 ? 0 : value > 255 ? 255 : value;
        }
        if (y != 7)
            dest += stride;
    }
}

#if ET_IDCT_PACKED
/* Model the actual FG32 address wrapping, FPACKREPB lane mapping and masked
 * two-word stores, rather than silently using the generic memory helpers. */
static uint32_t model_fg32h(const int16_t *src, unsigned lane)
{
    uintptr_t base = (uintptr_t)src;
    uintptr_t addr = (base & ~(uintptr_t)31) + ((base + 2 * lane) & 30);
    return (uint32_t)(int32_t)*(const int16_t *)addr;
}
#define LDA(d, row) LANES(d, model_fg32h(block + (row) * 8, lane))
static void et_idct_columns_packed(uint8_t *dest, ptrdiff_t stride,
                                   const int16_t *block, int add)
{
    static const unsigned result_regs[8] = {1, 2, 3, 4, 10, 7, 6, 5};
    uint32_t r[30][8];
    uint8_t *pred = dest;
    ET_IDCT_COL_OPS(LDA)
    for (unsigned y = 0; y < 8; ++y) {
        unsigned reg = result_regs[y];
        uint32_t tmp[8];
        for (unsigned lane = 0; lane < 8; ++lane) {
            uint32_t bits = r[reg][lane];
            int value = bits <= INT32_MAX ? (int)bits : -1 - (int)(UINT32_MAX - bits);
            if (add) {
                uintptr_t base = (uintptr_t)pred;
                uintptr_t addr = (base & ~(uintptr_t)31) + ((base + lane) & 31);
                /* FGB sign extension followed by FANDI 255 is unsigned byte. */
                value += *(const uint8_t *)addr;
            }
            tmp[lane] = value < 0 ? 0 : value > 255 ? 255 : value;
        }
        /* Exact fpackrepb.pi, including repeated upper lanes and source aliasing. */
        for (unsigned lane = 0; lane < 8; ++lane) {
            unsigned first = 4 * (lane & 1);
            r[reg][lane] = tmp[first] | (tmp[first + 1] << 8) |
                (tmp[first + 2] << 16) | (tmp[first + 3] << 24);
        }
        if (y != 7) pred += stride;
    }
    /* m0=3: only packed lanes zero and one are stored, not 32 bytes. */
    for (unsigned y = 0; y < 8; ++y) {
        for (unsigned lane = 0; lane < 2; ++lane) {
            uintptr_t base = (uintptr_t)dest;
            uintptr_t addr = (base & ~(uintptr_t)31) + ((base + 4 * lane) & 28);
            uint32_t word = r[result_regs[y]][lane];
            for (unsigned byte = 0; byte < 4; ++byte)
                ((uint8_t *)addr)[byte] = (uint8_t)(word >> (8 * byte));
        }
        if (y != 7) dest += stride;
    }
}

#undef LDA
#endif

#else

#if !defined(ET_DEVICE) || !defined(__riscv)
#error "ET integer SIMD requires ET_DEVICE on ET RISC-V; use ET_IDCT_SIMD_MODEL=1 only for native tests"
#endif

#define LD(d, row) "fgh.ps f" #d ", f31(%[src])\n" "addi %[src], %[src], 16\n"
#define BC(d, imm) "fbci.pi f" #d ", " #imm "\n"
#define AI(d, a, imm) "faddi.pi f" #d ", f" #a ", " #imm "\n"
#define MOV(d, a) "for.pi f" #d ", f" #a ", f" #a "\n"
#define MUL(d, a, b) "fmul.pi f" #d ", f" #a ", f" #b "\n"
#define ADD(d, a, b) "fadd.pi f" #d ", f" #a ", f" #b "\n"
#define SUB(d, a, b) "fsub.pi f" #d ", f" #a ", f" #b "\n"
#define OR(d, a, b) "for.pi f" #d ", f" #a ", f" #b "\n"
#define AND(d, a, b) "fand.pi f" #d ", f" #a ", f" #b "\n"
#define EQ(d, a, b) "feq.pi f" #d ", f" #a ", f" #b "\n"
#define NOT(d, a) "fnot.pi f" #d ", f" #a "\n"
#define LSL(d, a, n) "fslli.pi f" #d ", f" #a ", " #n "\n"
#define RLD(d, col) "fgh.ps f" #d ", f31(%[src])\n" \
    "addi %[src], %[src], 2\n" \
    ".if " #col "\n" "for.pi f29, f29, f" #d "\n" ".endif\n"
#define RST(d) "fsch.ps f" #d ", f31(%[dst])\n" "addi %[dst], %[dst], 2\n"
#define ASR(d, a, n) "fsrai.pi f" #d ", f" #a ", " #n "\n"
#define PUT(d) \
    "fsatu8.pi f" #d ", f" #d "\n" \
    "fscb.ps f" #d ", f31(%[dst])\n" \
    "add %[dst], %[dst], %[stride]\n"
#define ACCUMULATE(d) \
    "fgb.ps f28, f31(%[dst])\n" \
    /* FGB sign-extends: restore the unsigned prediction byte before Add. */ \
    "fandi.pi f28, f28, 255\n" \
    "fadd.pi f" #d ", f" #d ", f28\n" \
    PUT(d)

COLUMN_LINK void COLUMN_NAME(uint8_t *dest, ptrdiff_t stride,
                          const int16_t *block, int add)
{
    static const _Alignas(32) uint32_t offsets[8] = {0, 2, 4, 6, 8, 10, 12, 14};
    unsigned long saved_masks;
    __asm__ volatile(
        "mova.x.m %[masks]\n"
        "mov.m.x m0, zero, 255\n"
        "flw.ps f31, 0(%[offsets])\n"
        ET_IDCT_COL_OPS(LD)
        "fsrli.pi f31, f31, 1\n"
        "bnez %[add], 1f\n"
        PUT(1) PUT(2) PUT(3) PUT(4) PUT(10) PUT(7) PUT(6) PUT(5)
        "j 2f\n"
        "1:\n"
        ACCUMULATE(1) ACCUMULATE(2) ACCUMULATE(3) ACCUMULATE(4)
        ACCUMULATE(10) ACCUMULATE(7) ACCUMULATE(6) ACCUMULATE(5)
        "2:\n"
        "mov.m.x m0, %[masks], 0\n"
        : [src] "+&r"(block), [dst] "+&r"(dest), [masks] "=&r"(saved_masks)
        : [stride] "r"(stride), [add] "r"(add), [offsets] "r"(offsets)
        : "memory", "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7",
          "f10", "f11", "f12", "f13", "f14", "f15", "f16", "f17",
          "f28", "f29", "f31");
}

#if ET_IDCT_PACKED
#define LDA(d, row) "fg32h.ps f" #d ", %[hconf](%[src])\n" \
    "addi %[src], %[src], 16\n"
#define PRED(d) \
    "fg32b.ps f28, %[bconf](%[pred])\n" \
    "fandi.pi f28, f28, 255\n" \
    "fadd.pi f" #d ", f" #d ", f28\n" \
    "add %[pred], %[pred], %[stride]\n"
#define PACK(d) "fsatu8.pi f" #d ", f" #d "\n" \
    "fpackrepb.pi f" #d ", f" #d "\n"
#define PST(d) "fsc32w.ps f" #d ", %[wconf](%[dst])\n" \
    "add %[dst], %[dst], %[stride]\n"
static void et_idct_columns_packed(uint8_t *dest, ptrdiff_t stride,
                                   const int16_t *block, int add)
{
    uint8_t *pred = dest;
    unsigned long saved_masks;
    __asm__ volatile(
        "mova.x.m %[masks]\n"
        "mov.m.x m0, zero, 255\n"
        ET_IDCT_COL_OPS(LDA)
        "beqz %[add], 1f\n"
        PRED(1) PRED(2) PRED(3) PRED(4) PRED(10) PRED(7) PRED(6) PRED(5)
        "1:\n"
        PACK(1) PACK(2) PACK(3) PACK(4) PACK(10) PACK(7) PACK(6) PACK(5)
        "mov.m.x m0, zero, 3\n"
        PST(1) PST(2) PST(3) PST(4) PST(10) PST(7) PST(6) PST(5)
        "mov.m.x m0, %[masks], 0\n"
        : [src] "+&r"(block), [dst] "+&r"(dest), [pred] "+&r"(pred),
          [masks] "=&r"(saved_masks)
        : [stride] "r"(stride), [add] "r"(add),
          [hconf] "r"(0x76543210UL), [bconf] "r"(0x398a418820UL), [wconf] "r"(8UL)
        : "memory", "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7",
          "f10", "f11", "f12", "f13", "f14", "f15", "f16", "f17",
          "f28", "f29");
}
#undef LDA
#undef PRED
#undef PACK
#undef PST
#endif

#if ET_IDCT_ROWS
void et_idct_simd_rows(int16_t *block)
{
    static const _Alignas(32) uint32_t offsets[8] = {0, 16, 32, 48, 64, 80, 96, 112};
    const int16_t *src = block;
    unsigned long saved_masks;
    __asm__ volatile(
        "mova.x.m %[masks]\n"
        "mov.m.x m0, zero, 255\n"
        "flw.ps f31, 0(%[offsets])\n"
        ET_IDCT_ROW_OPS
        RST(1) RST(2) RST(3) RST(4) RST(10) RST(7) RST(6) RST(5)
        "mov.m.x m0, %[masks], 0\n"
        : [src] "+&r"(src), [dst] "+&r"(block), [masks] "=&r"(saved_masks)
        : [offsets] "r"(offsets)
        : "memory", "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7",
          "f10", "f11", "f12", "f13", "f14", "f15", "f16", "f17",
          "f28", "f29", "f30", "f31");
}
#endif

#undef PUT
#undef ACCUMULATE
#endif

#if ET_IDCT_PACKED
void et_idct_simd_columns(uint8_t *dest, ptrdiff_t stride,
                          const int16_t *block, int add)
{
    /* A halfword row occupies [0,15] or [16,31] within its FG32 window.
     * Each destination occupies one aligned 8-byte interval within its window.
     * These runtime checks also prove each two-word packed store stays within
     * that window.  Nonzero stride guarantees disjoint destination rows.
     * No stronger public alignment contract is introduced. */
    if (stride && !((uintptr_t)block & 15) &&
        !(((uintptr_t)dest | (uintptr_t)stride) & 7))
        et_idct_columns_packed(dest, stride, block, add);
    else
        et_idct_columns_generic(dest, stride, block, add);
}
#endif

#undef COLUMN_LINK
#undef COLUMN_NAME

#undef LD
#undef BC
#undef AI
#undef MOV
#undef MUL
#undef ADD
#undef SUB
#undef ASR
#undef LANES
#undef ET_IDCT_COL_OPS

#undef RLD
#undef RST
#undef OR
#undef AND
#undef EQ
#undef NOT
#undef LSL
#undef ET_IDCT_ROW_OPS
#undef SELECT_DC
