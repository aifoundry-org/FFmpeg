/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Exact MPEG-2 half-pel motion compensation. No edge emulation or padding.
 *
 * Contract: size is 8 or 16; hx, hy, average are 0/1. Every source row has
 * size+hx readable bytes, for size+hy rows. Destination has size writable
 * bytes per row (also readable for average). Source and destination do not
 * overlap. stride >= size+hx, destination and stride are 8-byte aligned.
 * The decoder must validate these conditions BEFORE calling this header.
 * Only the proven rectangle is accessed, including at an allocation edge.
 *
 * ET_MC_IMPL: 0 scalar, 1 RV64 SWAR (default), 2 ET paired-byte SIMD4,
 *             3 ET eight-lane SIMD with packed L1 stores,
 *             4 SIMD8 with proven-window FG32 and packed L1 FSC32W stores.
 * ET_MC_LOAD: 0 exact byte loads, 1 largest naturally aligned chunks (default).
 * ET_MC_XY:   0 separate even/odd bytes, 1 quotient/remainder (default).
 * ET_MC_V8_STORE: mode 3 only: 0 packed L1 fsw.ps (default), 1 byte scatter.
 * SIMD implementations require silicon gating, not just native models.
 */
#ifndef ET_MPEG2_MOTION_H
#define ET_MPEG2_MOTION_H

#include <stddef.h>
#include <stdint.h>

#ifndef ET_MC_IMPL
#define ET_MC_IMPL 1
#endif
#ifndef ET_MC_LOAD
#define ET_MC_LOAD 1
#endif
#ifndef ET_MC_XY
#define ET_MC_XY 1
#endif
#ifndef ET_MC_V8_STORE
#define ET_MC_V8_STORE 0
#endif
#if ET_MC_IMPL < 0 || ET_MC_IMPL > 4 || ET_MC_LOAD < 0 || ET_MC_LOAD > 1 || ET_MC_XY < 0 || ET_MC_XY > 1 || ET_MC_V8_STORE < 0 || ET_MC_V8_STORE > 1
#error "Invalid motion compensation control"
#endif
#if ET_MC_IMPL >= 2 && (!defined(__riscv) || !defined(ET_DEVICE))
#error "ET_MC_IMPL>=2 needs the ET device compiler; native tests use an explicit model"
#endif

#define ET_MC_INLINE static inline __attribute__((always_inline))
typedef uint64_t et_mc_u64_alias __attribute__((may_alias));
typedef uint32_t et_mc_u32_alias __attribute__((may_alias));
typedef uint16_t et_mc_u16_alias __attribute__((may_alias));

/* Volatile byte accesses forbid a compiler's speculative/widened reads.
 * Wide accesses below are always naturally aligned and wholly in [p,p+8). */
ET_MC_INLINE uint64_t et_mc_load8_bytes(const uint8_t *p)
{
    const volatile uint8_t *v = p;
    return (uint64_t)v[0]       | (uint64_t)v[1] << 8  |
           (uint64_t)v[2] << 16 | (uint64_t)v[3] << 24 |
           (uint64_t)v[4] << 32 | (uint64_t)v[5] << 40 |
           (uint64_t)v[6] << 48 | (uint64_t)v[7] << 56;
}

ET_MC_INLINE uint64_t et_mc_load8(const uint8_t *p)
{
#if ET_MC_LOAD == 1
    uintptr_t alignment = (uintptr_t)p & 7;
    if (!alignment)
        return *(const et_mc_u64_alias *)p;
    if (!(alignment & 3))
        return (uint64_t)*(const et_mc_u32_alias *)p |
               (uint64_t)*(const et_mc_u32_alias *)(p + 4) << 32;
    if (!(alignment & 1))
        return (uint64_t)*(const et_mc_u16_alias *)p |
               (uint64_t)*(const et_mc_u16_alias *)(p + 2) << 16 |
               (uint64_t)*(const et_mc_u16_alias *)(p + 4) << 32 |
               (uint64_t)*(const et_mc_u16_alias *)(p + 6) << 48;
#endif
    return et_mc_load8_bytes(p);
}

/* Per-byte ceil((a+b)/2). The subtraction cannot borrow across bytes:
 * (a|b) >= ((a^b)>>1) in EACH byte. Mask after shifting, not before. */
ET_MC_INLINE uint64_t et_mc_avg8(uint64_t a, uint64_t b)
{
    return (a | b) - (((a ^ b) >> 1) & UINT64_C(0x7f7f7f7f7f7f7f7f));
}

ET_MC_INLINE uint64_t et_mc_xy8_evenodd(uint64_t a, uint64_t b,
                                       uint64_t c, uint64_t d)
{
    const uint64_t m = UINT64_C(0x00ff00ff00ff00ff);
    const uint64_t r = UINT64_C(0x0002000200020002);
    /* Each 16-bit slot sums to at most 1022: no cross-slot carry. */
    uint64_t lo = (a & m) + (b & m) + (c & m) + (d & m) + r;
    uint64_t hi = ((a >> 8) & m) + ((b >> 8) & m) +
                  ((c >> 8) & m) + ((d >> 8) & m) + r;
    return ((lo >> 2) & m) | (((hi >> 2) & m) << 8);
}

ET_MC_INLINE uint64_t et_mc_xy8_parts(uint64_t a, uint64_t b,
                                     uint64_t c, uint64_t d)
{
    const uint64_t q = UINT64_C(0x3f3f3f3f3f3f3f3f);
    const uint64_t r = UINT64_C(0x0303030303030303);
    /* Quotients total <=252. Remainders+rounding total <=14. Both
     * additions stay inside bytes; final quotient+carry is <=255. */
    uint64_t hi = ((a >> 2) & q) + ((b >> 2) & q) +
                  ((c >> 2) & q) + ((d >> 2) & q);
    uint64_t lo = (a & r) + (b & r) + (c & r) + (d & r) +
                  UINT64_C(0x0202020202020202);
    return hi + ((lo >> 2) & r);
}

ET_MC_INLINE uint64_t et_mc_xy8(uint64_t a, uint64_t b,
                               uint64_t c, uint64_t d)
{
#if ET_MC_XY == 0
    return et_mc_xy8_evenodd(a, b, c, d);
#else
    return et_mc_xy8_parts(a, b, c, d);
#endif
}

ET_MC_INLINE void et_mc_scalar(uint8_t *dst, const uint8_t *src,
                               size_t stride, unsigned size,
                               unsigned hx, unsigned hy, unsigned average)
{
    for (unsigned y = 0; y < size; ++y) {
        for (unsigned x = 0; x < size; ++x) {
            unsigned a = src[x], val;
            if (hx && hy)
                val = (a + src[x+1] + src[x+stride] + src[x+stride+1] + 2) >> 2;
            else if (hx) val = (a + src[x+1] + 1) >> 1;
            else if (hy) val = (a + src[x+stride] + 1) >> 1;
            else val = a;
            dst[x] = (uint8_t)(average ? (dst[x] + val + 1) >> 1 : val);
        }
        /* Avoid even forming a pointer past the final validated row. */
        if (y + 1 < size) { src += stride; dst += stride; }
    }
}

/* hx/hy/average are constants after the outer dispatch, so there are no
 * per-pixel mode branches. Horizontal neighbor needs only ONE halo byte. */
ET_MC_INLINE void et_mc_swar_body(uint8_t *dst, const uint8_t *src,
                                  size_t stride, unsigned size,
                                  unsigned hx, unsigned hy, unsigned average)
{
    for (unsigned y = 0; y < size; ++y) {
        for (unsigned x = 0; x < size; x += 8) {
            uint64_t a = et_mc_load8(src+x), value = a;
            if (hx) {
                uint64_t b = (a >> 8) | (uint64_t)((const volatile uint8_t *)src)[x+8] << 56;
                if (hy) {
                    uint64_t c = et_mc_load8(src+x+stride);
                    uint64_t d = (c >> 8) | (uint64_t)((const volatile uint8_t *)src)[x+stride+8] << 56;
                    value = et_mc_xy8(a, b, c, d);
                } else value = et_mc_avg8(a, b);
            } else if (hy) value = et_mc_avg8(a, et_mc_load8(src+x+stride));
            if (average) value = et_mc_avg8(value, *(const et_mc_u64_alias *)(dst+x));
            *(et_mc_u64_alias *)(dst+x) = value;
        }
        if (y + 1 < size) { src += stride; dst += stride; }
    }
}

#define ET_MC_DISPATCH(fn) \
    switch (hx | (hy << 1) | (average << 2)) { \
    case 0: fn(dst, src, stride, size, 0, 0, 0); break; \
    case 1: fn(dst, src, stride, size, 1, 0, 0); break; \
    case 2: fn(dst, src, stride, size, 0, 1, 0); break; \
    case 3: fn(dst, src, stride, size, 1, 1, 0); break; \
    case 4: fn(dst, src, stride, size, 0, 0, 1); break; \
    case 5: fn(dst, src, stride, size, 1, 0, 1); break; \
    case 6: fn(dst, src, stride, size, 0, 1, 1); break; \
    case 7: fn(dst, src, stride, size, 1, 1, 1); break; \
    }

ET_MC_INLINE void et_mc_swar(uint8_t *dst, const uint8_t *src,
                            size_t stride, unsigned size,
                            unsigned hx, unsigned hy, unsigned average)
{
    ET_MC_DISPATCH(et_mc_swar_body)
}

#if ET_MC_IMPL == 2
/* See tests/MOTION.md for ISA sources and audit. Deliberately use only the
 * low FOUR lanes: setting all eight then doing a 16-byte load/store is wrong.
 * Each lane predicts TWO pixels, independently in f1 (even) and f3 (odd).
 * Only after rounding/blending do we pack them for aligned halfword scatter.
 * Thus one group writes exactly eight bytes, never the inactive half-lanes.
 * Byte gathers never assume source alignment or round a source pointer down.
 * Signed byte gather must be zero-extended BEFORE summation. Entire vector
 * lifetime is in one asm; GCC must not spill vectors as scalar floats.
 * Save/restore all masks; only caller-saved FP registers are clobbered.
 */
#define ET_MC_VLOAD(reg, addr) \
    "fgb.ps " reg ", f0(" addr ")\n" \
    "fandi.pi " reg ", " reg ", 255\n"
#define ET_MC_VX \
    "addi t0, %[s], 2\n" ET_MC_VLOAD("f2", "t0") \
    "fadd.pi f1, f1, f3\n" \
    "fadd.pi f3, f3, f2\n"
#define ET_MC_VY \
    "add t0, %[s], %[stride]\n" ET_MC_VLOAD("f4", "t0") \
    "addi t0, t0, 1\n" ET_MC_VLOAD("f5", "t0") \
    "fadd.pi f1, f1, f4\n" \
    "fadd.pi f3, f3, f5\n"
#define ET_MC_VXY \
    ET_MC_VX \
    "add t0, %[s], %[stride]\n" ET_MC_VLOAD("f4", "t0") \
    "addi t0, t0, 1\n" ET_MC_VLOAD("f5", "t0") \
    "addi t0, t0, 1\n" ET_MC_VLOAD("f6", "t0") \
    "fadd.pi f4, f4, f5\n" \
    "fadd.pi f5, f5, f6\n" \
    "fadd.pi f1, f1, f4\n" \
    "fadd.pi f3, f3, f5\n" \
    "faddi.pi f1, f1, 2\n" "fsrli.pi f1, f1, 2\n" \
    "faddi.pi f3, f3, 2\n" "fsrli.pi f3, f3, 2\n"
#define ET_MC_VHALF \
    "faddi.pi f1, f1, 1\n" "fsrli.pi f1, f1, 1\n" \
    "faddi.pi f3, f3, 1\n" "fsrli.pi f3, f3, 1\n"
#define ET_MC_VAVG \
    ET_MC_VLOAD("f4", "%[d]") \
    "addi t0, %[d], 1\n" ET_MC_VLOAD("f5", "t0") \
    "fadd.pi f1, f1, f4\n" \
    "fadd.pi f3, f3, f5\n" ET_MC_VHALF
#define ET_MC_VBODY(name, prediction, blend) \
ET_MC_INLINE void name(uint8_t *dst, const uint8_t *src, size_t stride, unsigned size) \
{ \
    _Alignas(16) static const uint32_t offsets[4] = {0, 2, 4, 6}; \
    uintptr_t saved_masks, cols, rows = size; \
    __asm__ volatile( \
        "mova.x.m %[masks]\n" \
        "mov.m.x m0, zero, 15\n" \
        "flw.ps f0, 0(%[offsets])\n" \
        "1:\n" \
        "mv %[cols], %[size]\n" \
        "2:\n" \
        ET_MC_VLOAD("f1", "%[s]") \
        "addi t0, %[s], 1\n" ET_MC_VLOAD("f3", "t0") prediction blend \
        "fslli.pi f3, f3, 8\n" \
        "for.pi f1, f1, f3\n" \
        "fsch.ps f1, f0(%[d])\n" \
        "addi %[cols], %[cols], -8\n" \
        "beqz %[cols], 3f\n" \
        "addi %[s], %[s], 8\n" \
        "addi %[d], %[d], 8\n" \
        "j 2b\n" \
        "3:\n" \
        "addi %[rows], %[rows], -1\n" \
        "beqz %[rows], 4f\n" \
        "add %[s], %[s], %[step]\n" \
        "add %[d], %[d], %[step]\n" \
        "j 1b\n" \
        "4:\n" \
        "mova.m.x %[masks]\n" \
        : [s] "+&r"(src), [d] "+&r"(dst), [rows] "+&r"(rows), \
          [masks] "=&r"(saved_masks), [cols] "=&r"(cols) \
        : [stride] "r"(stride), [size] "r"((uintptr_t)size), \
          [step] "r"(stride-size+8), [offsets] "r"(offsets) \
        : "t0", "f0", "f1", "f2", "f3", "f4", "f5", "f6", "memory"); \
}
ET_MC_VBODY(et_mc_v_put, "", "")
ET_MC_VBODY(et_mc_v_x, ET_MC_VX ET_MC_VHALF, "")
ET_MC_VBODY(et_mc_v_y, ET_MC_VY ET_MC_VHALF, "")
ET_MC_VBODY(et_mc_v_xy, ET_MC_VXY, "")
ET_MC_VBODY(et_mc_v_avg, "", ET_MC_VAVG)
ET_MC_VBODY(et_mc_v_ax, ET_MC_VX ET_MC_VHALF, ET_MC_VAVG)
ET_MC_VBODY(et_mc_v_ay, ET_MC_VY ET_MC_VHALF, ET_MC_VAVG)
ET_MC_VBODY(et_mc_v_axy, ET_MC_VXY, ET_MC_VAVG)

ET_MC_INLINE void et_mc_vector(uint8_t *dst, const uint8_t *src,
                              size_t stride, unsigned size,
                              unsigned hx, unsigned hy, unsigned average)
{
    switch (hx | (hy << 1) | (average << 2)) {
    case 0: et_mc_v_put(dst, src, stride, size); break;
    case 1: et_mc_v_x(dst, src, stride, size); break;
    case 2: et_mc_v_y(dst, src, stride, size); break;
    case 3: et_mc_v_xy(dst, src, stride, size); break;
    case 4: et_mc_v_avg(dst, src, stride, size); break;
    case 5: et_mc_v_ax(dst, src, stride, size); break;
    case 6: et_mc_v_ay(dst, src, stride, size); break;
    case 7: et_mc_v_axy(dst, src, stride, size); break;
    }
}
#undef ET_MC_VBODY
#undef ET_MC_VLOAD
#undef ET_MC_VX
#undef ET_MC_VY
#undef ET_MC_VXY
#undef ET_MC_VHALF
#undef ET_MC_VAVG
#endif

#if ET_MC_IMPL == 3
/* Eight adjacent bytes -> eight independent u32 lanes. Local ISA evidence:
 * fgb.ps SIGN-extends; fandi.pi 255 (NOT fsatu8.pi) converts to unsigned.
 * fpackrepb.pi puts pixel bytes 0..3 in lane 0, 4..7 in lane 1, repeated in
 * lanes 2..7. Only lanes 0,1 may store. MUST use L1 fsw.ps, never fsw[lg].ps:
 * A0 coherent-vector stores can ignore masks and overwrite the next 24 bytes.
 * No saturation needed: exact prediction/blend already lies in [0,255].
 * Full FF mask for gathers is safe: offsets 0..7 fit the proven 8/9-byte span.
 * Table and saved masks live across all rows in one asm. No per-row calls,
 * scalar-float spills, or mask saves. Each iteration covers EIGHT pixels,
 * just like mode 2 (which computes two pixels in each of its four lanes).
 */
#define ET_MC_WLOAD(reg, addr) \
    "fgb.ps " reg ", f0(" addr ")\n" \
    "fandi.pi " reg ", " reg ", 255\n"
#define ET_MC_WX \
    "addi t0, %[s], 1\n" ET_MC_WLOAD("f2", "t0") \
    "fadd.pi f1, f1, f2\n"
#define ET_MC_WY \
    "add t0, %[s], %[stride]\n" ET_MC_WLOAD("f2", "t0") \
    "fadd.pi f1, f1, f2\n"
#define ET_MC_WXY \
    ET_MC_WX ET_MC_WY \
    "addi t0, t0, 1\n" ET_MC_WLOAD("f2", "t0") \
    "fadd.pi f1, f1, f2\n" \
    "faddi.pi f1, f1, 2\n" "fsrli.pi f1, f1, 2\n"
#define ET_MC_WHALF "faddi.pi f1, f1, 1\n" "fsrli.pi f1, f1, 1\n"
#define ET_MC_WAVG \
    ET_MC_WLOAD("f2", "%[d]") \
    "fadd.pi f1, f1, f2\n" ET_MC_WHALF
#if ET_MC_V8_STORE == 0
#define ET_MC_WSTORE \
    "fpackrepb.pi f1, f1\n" \
    "mov.m.x m0, zero, 3\n" \
    "fsw.ps f1, 0(%[d])\n" \
    "mov.m.x m0, zero, 255\n"
#else
#define ET_MC_WSTORE "fscb.ps f1, f0(%[d])\n"
#endif
#define ET_MC_WBODY(name, prediction, blend) \
ET_MC_INLINE void name(uint8_t *dst, const uint8_t *src, size_t stride, unsigned size) \
{ \
    _Alignas(32) static const uint32_t offsets[8] = {0, 1, 2, 3, 4, 5, 6, 7}; \
    uintptr_t saved_masks, cols, rows = size; \
    __asm__ volatile( \
        "mova.x.m %[masks]\n" \
        "mov.m.x m0, zero, 255\n" \
        "flw.ps f0, 0(%[offsets])\n" \
        "1:\n" \
        "mv %[cols], %[size]\n" \
        "2:\n" \
        ET_MC_WLOAD("f1", "%[s]") prediction blend \
        ET_MC_WSTORE \
        "addi %[cols], %[cols], -8\n" \
        "beqz %[cols], 3f\n" \
        "addi %[s], %[s], 8\n" \
        "addi %[d], %[d], 8\n" \
        "j 2b\n" \
        "3:\n" \
        "addi %[rows], %[rows], -1\n" \
        "beqz %[rows], 4f\n" \
        "add %[s], %[s], %[step]\n" \
        "add %[d], %[d], %[step]\n" \
        "j 1b\n" \
        "4:\n" \
        "mova.m.x %[masks]\n" \
        : [s] "+&r"(src), [d] "+&r"(dst), [rows] "+&r"(rows), \
          [masks] "=&r"(saved_masks), [cols] "=&r"(cols) \
        : [stride] "r"(stride), [size] "r"((uintptr_t)size), \
          [step] "r"(stride-size+8), [offsets] "r"(offsets) \
        : "t0", "f0", "f1", "f2", "memory"); \
}
ET_MC_WBODY(et_mc_w_put, "", "")
ET_MC_WBODY(et_mc_w_x, ET_MC_WX ET_MC_WHALF, "")
ET_MC_WBODY(et_mc_w_y, ET_MC_WY ET_MC_WHALF, "")
ET_MC_WBODY(et_mc_w_xy, ET_MC_WXY, "")
ET_MC_WBODY(et_mc_w_avg, "", ET_MC_WAVG)
ET_MC_WBODY(et_mc_w_ax, ET_MC_WX ET_MC_WHALF, ET_MC_WAVG)
ET_MC_WBODY(et_mc_w_ay, ET_MC_WY ET_MC_WHALF, ET_MC_WAVG)
ET_MC_WBODY(et_mc_w_axy, ET_MC_WXY, ET_MC_WAVG)

ET_MC_INLINE void et_mc_vector8(uint8_t *dst, const uint8_t *src,
                               size_t stride, unsigned size,
                               unsigned hx, unsigned hy, unsigned average)
{
    switch (hx | (hy << 1) | (average << 2)) {
    case 0: et_mc_w_put(dst, src, stride, size); break;
    case 1: et_mc_w_x(dst, src, stride, size); break;
    case 2: et_mc_w_y(dst, src, stride, size); break;
    case 3: et_mc_w_xy(dst, src, stride, size); break;
    case 4: et_mc_w_avg(dst, src, stride, size); break;
    case 5: et_mc_w_ax(dst, src, stride, size); break;
    case 6: et_mc_w_ay(dst, src, stride, size); break;
    case 7: et_mc_w_axy(dst, src, stride, size); break;
    }
}
#undef ET_MC_WBODY
#undef ET_MC_WLOAD
#undef ET_MC_WX
#undef ET_MC_WY
#undef ET_MC_WXY
#undef ET_MC_WHALF
#undef ET_MC_WAVG
#undef ET_MC_WSTORE
#endif

#if ET_MC_IMPL == 4
/* Column-major groups: prove the source window ONCE per eight-column stripe,
 * outside its row loop. If stride%32==0 and (src%32)<=24-hx, every 8/9-byte
 * source span in that stripe fits a single aligned 32-byte window. Otherwise
 * take an all-general-byte-gather loop (also handles valid stride%32!=0).
 * For a 16x16 plane, either/both stripes may independently use the fast loop.
 * Only byte gathers read pixels; FG32's address rounding never fetches extra
 * bytes. Destination is always 8-byte aligned, so its FG32B avg read and two
 * FSC32W packed stores cannot wrap. Mask=3 enables exactly words at dst,dst+4;
 * config 8 encodes word offsets {0,1} in the first two 3-bit fields.
 * Ordinary L1 stores ONLY: no G/L suffix, new cache operations or ownership.
 */
#define ET_MC_RGEN(reg, addr) \
    "fgb.ps " reg ", f0(" addr ")\n" \
    "fandi.pi " reg ", " reg ", 255\n"
#define ET_MC_RFAST(reg, addr) \
    "fg32b.ps " reg ", %[gather](" addr ")\n" \
    "fandi.pi " reg ", " reg ", 255\n"
#define ET_MC_RXADD(load) \
    "addi t0, %[s], 1\n" load("f2", "t0") \
    "fadd.pi f1, f1, f2\n"
#define ET_MC_RYADD(load) \
    "add t0, %[s], %[stride]\n" load("f2", "t0") \
    "fadd.pi f1, f1, f2\n"
#define ET_MC_RHALF "faddi.pi f1, f1, 1\n" "fsrli.pi f1, f1, 1\n"
#define ET_MC_RNONE(load)
#define ET_MC_RX(load) ET_MC_RXADD(load) ET_MC_RHALF
#define ET_MC_RY(load) ET_MC_RYADD(load) ET_MC_RHALF
#define ET_MC_RXY(load) \
    ET_MC_RXADD(load) ET_MC_RYADD(load) \
    "addi t0, t0, 1\n" load("f2", "t0") \
    "fadd.pi f1, f1, f2\n" \
    "faddi.pi f1, f1, 2\n" "fsrli.pi f1, f1, 2\n"
#define ET_MC_RAVG \
    ET_MC_RFAST("f2", "%[d]") \
    "fadd.pi f1, f1, f2\n" ET_MC_RHALF
#define ET_MC_RSTORE \
    "fpackrepb.pi f1, f1\n" \
    "mov.m.x m0, zero, 3\n" \
    "fsc32w.ps f1, %[wconf](%[d])\n" \
    "mov.m.x m0, zero, 255\n"
#define ET_MC_RBODY(name, hx, prediction, blend) \
ET_MC_INLINE void name(uint8_t *dst, const uint8_t *src, size_t stride, unsigned size) \
{ \
    _Alignas(32) static const uint32_t offsets[8] = {0, 1, 2, 3, 4, 5, 6, 7}; \
    const uintptr_t gather = (UINT64_C(1)<<5) | (UINT64_C(2)<<10) | \
        (UINT64_C(3)<<15) | (UINT64_C(4)<<20) | (UINT64_C(5)<<25) | \
        (UINT64_C(6)<<30) | (UINT64_C(7)<<35); \
    uintptr_t saved_masks, rows, cols = size; \
    __asm__ volatile( \
        "mova.x.m %[masks]\n" \
        "mov.m.x m0, zero, 255\n" \
        "flw.ps f0, 0(%[offsets])\n" \
        "1:\n" \
        "mv %[rows], %[size]\n" \
        "andi t0, %[s], 31\n" \
        "or t0, t0, %[badstride]\n" \
        "sltiu t0, t0, %[limit]\n" \
        "beqz t0, 3f\n" \
        "2:\n" \
        ET_MC_RFAST("f1", "%[s]") prediction(ET_MC_RFAST) blend \
        ET_MC_RSTORE \
        "addi %[rows], %[rows], -1\n" \
        "beqz %[rows], 4f\n" \
        "add %[s], %[s], %[stride]\n" \
        "add %[d], %[d], %[stride]\n" \
        "j 2b\n" \
        "3:\n" \
        ET_MC_RGEN("f1", "%[s]") prediction(ET_MC_RGEN) blend \
        ET_MC_RSTORE \
        "addi %[rows], %[rows], -1\n" \
        "beqz %[rows], 4f\n" \
        "add %[s], %[s], %[stride]\n" \
        "add %[d], %[d], %[stride]\n" \
        "j 3b\n" \
        "4:\n" \
        "addi %[cols], %[cols], -8\n" \
        "beqz %[cols], 5f\n" \
        "add %[s], %[s], %[colstep]\n" \
        "add %[d], %[d], %[colstep]\n" \
        "j 1b\n" \
        "5:\n" \
        "mova.m.x %[masks]\n" \
        : [s] "+&r"(src), [d] "+&r"(dst), [cols] "+&r"(cols), \
          [masks] "=&r"(saved_masks), [rows] "=&r"(rows) \
        : [stride] "r"(stride), [size] "r"((uintptr_t)size), \
          [colstep] "r"((uintptr_t)8-(size-1)*stride), \
          [badstride] "r"((uintptr_t)(stride & 31 ? 32 : 0)), \
          [limit] "i"(25-(hx)), [gather] "r"(gather), \
          [wconf] "r"((uintptr_t)8), [offsets] "r"(offsets) \
        : "t0", "f0", "f1", "f2", "memory"); \
}
ET_MC_RBODY(et_mc_r_put, 0, ET_MC_RNONE, "")
ET_MC_RBODY(et_mc_r_x, 1, ET_MC_RX, "")
ET_MC_RBODY(et_mc_r_y, 0, ET_MC_RY, "")
ET_MC_RBODY(et_mc_r_xy, 1, ET_MC_RXY, "")
ET_MC_RBODY(et_mc_r_avg, 0, ET_MC_RNONE, ET_MC_RAVG)
ET_MC_RBODY(et_mc_r_ax, 1, ET_MC_RX, ET_MC_RAVG)
ET_MC_RBODY(et_mc_r_ay, 0, ET_MC_RY, ET_MC_RAVG)
ET_MC_RBODY(et_mc_r_axy, 1, ET_MC_RXY, ET_MC_RAVG)

ET_MC_INLINE void et_mc_vector32(uint8_t *dst, const uint8_t *src,
                                size_t stride, unsigned size,
                                unsigned hx, unsigned hy, unsigned average)
{
    switch (hx | (hy << 1) | (average << 2)) {
    case 0: et_mc_r_put(dst, src, stride, size); break;
    case 1: et_mc_r_x(dst, src, stride, size); break;
    case 2: et_mc_r_y(dst, src, stride, size); break;
    case 3: et_mc_r_xy(dst, src, stride, size); break;
    case 4: et_mc_r_avg(dst, src, stride, size); break;
    case 5: et_mc_r_ax(dst, src, stride, size); break;
    case 6: et_mc_r_ay(dst, src, stride, size); break;
    case 7: et_mc_r_axy(dst, src, stride, size); break;
    }
}
#undef ET_MC_RBODY
#undef ET_MC_RGEN
#undef ET_MC_RFAST
#undef ET_MC_RXADD
#undef ET_MC_RYADD
#undef ET_MC_RHALF
#undef ET_MC_RNONE
#undef ET_MC_RX
#undef ET_MC_RY
#undef ET_MC_RXY
#undef ET_MC_RAVG
#undef ET_MC_RSTORE
#endif

ET_MC_INLINE void et_mc_predict(uint8_t *dst, const uint8_t *src,
                               size_t stride, unsigned size,
                               unsigned hx, unsigned hy, unsigned average)
{
#if ET_MC_IMPL == 0 || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
    et_mc_scalar(dst, src, stride, size, hx, hy, average);
#elif ET_MC_IMPL == 1
    et_mc_swar(dst, src, stride, size, hx, hy, average);
#elif ET_MC_IMPL == 2
    et_mc_vector(dst, src, stride, size, hx, hy, average);
#elif ET_MC_IMPL == 3
    et_mc_vector8(dst, src, stride, size, hx, hy, average);
#else
    et_mc_vector32(dst, src, stride, size, hx, hy, average);
#endif
}
#undef ET_MC_DISPATCH
#undef ET_MC_INLINE
#endif
