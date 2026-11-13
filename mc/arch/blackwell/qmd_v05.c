/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * arch/blackwell/qmd_v05.c — NVK-style Blackwell QMD V05 builder.
 *
 * Mirrors Mesa NVK's `Qmd5_0` (qmd.rs:807-839) + `fill_qmd` (qmd.rs:842+).
 * See arch/blackwell/qmd_v05.h for provenance and for what changed from V04.
 *
 * Bit-range references are the NVCEC0_QMDV05_00_* macros from Mesa's
 * clcec0qmd.h -- the QMD header for BLACKWELL_COMPUTE_B, the class this
 * backend allocates.  All MW(hi:lo) ranges are flat bit positions over the
 * 384-byte QMD (LSB = bit 0 of byte 0).
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "hal/arch.h"
#include "qmd_v05.h"

/* ── Bit-range write helper ─────────────────────────────────────────
 * Writes `value` into the QMD at bits [hi:lo] inclusive (MW(hi:lo)
 * semantics), handling a range that straddles two 32-bit dwords, as NVK's
 * `BitMutView::set_field` does.  The QMD is an array of little-endian
 * dwords; the SDK macros and NVK both view it that way.
 *
 * This is a deliberate copy of the V04 helper rather than a shared one.
 * The two builders have no field constants in common (see the table in
 * qmd_v05.h) and hoisting the writer would be the one piece of shared
 * surface through which a change to one QMD version could reach the other.
 * Twenty lines is a cheap price for that isolation.
 */
static void qmd_set_bits(uint8_t *qmd, unsigned hi, unsigned lo, uint64_t value)
{
    unsigned width = hi - lo + 1;
    assert(width <= 32);

    uint64_t mask = (width == 64) ? ~0ULL : ((1ULL << width) - 1ULL);
    value &= mask;

    uint32_t *qw = (uint32_t *)qmd;

    unsigned dw_lo  = lo / 32;
    unsigned bit_lo = lo % 32;
    unsigned bit_hi = hi - dw_lo * 32;        /* relative to dw_lo */

    if (bit_hi < 32) {
        uint32_t dw_mask = (uint32_t)(mask << bit_lo);
        qw[dw_lo] = (qw[dw_lo] & ~dw_mask) | (uint32_t)((value & mask) << bit_lo);
    } else {
        unsigned lo_bits = 32 - bit_lo;
        uint32_t lo_mask = (uint32_t)(((1ULL << lo_bits) - 1ULL) << bit_lo);
        uint32_t hi_mask = (uint32_t)((1ULL << (width - lo_bits)) - 1ULL);

        qw[dw_lo] = (qw[dw_lo] & ~lo_mask)
                    | (uint32_t)((value & ((1ULL << lo_bits) - 1ULL)) << bit_lo);
        qw[dw_lo + 1] = (qw[dw_lo + 1] & ~hi_mask)
                    | (uint32_t)((value >> lo_bits) & hi_mask);
    }
}

/* ── QMD V05 field bit positions (clcec0qmd.h, NVCEC0_QMDV05_00_*) ────── */
/* QMD_GROUP_ID                          MW(149:144)                      */
#define QMD_GROUP_ID_HI           149u
#define QMD_GROUP_ID_LO           144u
#define QMD_GROUP_ID_VALUE        0x1Fu   /* NVK Qmd5_0::new, qmd.rs:817  */
/* QMD_TYPE                              MW(153:151)                      */
#define QMD_TYPE_HI               153u
#define QMD_TYPE_LO               151u
#define QMD_TYPE_GRID_CTA         0x00000002u
/* API_VISIBLE_CALL_LIMIT                MW(456:456)                      */
#define API_VIS_CALL_LIMIT_BIT    456u
#define API_VIS_CALL_LIMIT_NO_CHECK 1u
/* SAMPLER_INDEX                         MW(457:457)                      */
#define SAMPLER_INDEX_BIT         457u
#define SAMPLER_INDEX_INDEPENDENTLY 0u
/* QMD_MINOR_VERSION                     MW(467:464)                      */
#define QMD_MINOR_VERSION_HI      467u
#define QMD_MINOR_VERSION_LO      464u
/* QMD_MAJOR_VERSION                     MW(471:468)                      */
#define QMD_MAJOR_VERSION_HI      471u
#define QMD_MAJOR_VERSION_LO      468u
/* PROGRAM_ADDRESS_LOWER_SHIFTED4        MW(1055:1024)                    */
#define PROG_ADDR_LO_HI           1055u
#define PROG_ADDR_LO_LO           1024u
/* PROGRAM_ADDRESS_UPPER_SHIFTED4        MW(1076:1056)                    */
#define PROG_ADDR_HI_HI           1076u
#define PROG_ADDR_HI_LO           1056u
/* CTA_THREAD_DIMENSION0/1/2  MW(1103:1088) / MW(1119:1104) / MW(1127:1120)
 * Note DIMENSION2 is 8 bits in V05 against 16 in V04. */
#define CTA_DIM0_HI               1103u
#define CTA_DIM0_LO               1088u
#define CTA_DIM1_HI               1119u
#define CTA_DIM1_LO               1104u
#define CTA_DIM2_HI               1127u
#define CTA_DIM2_LO               1120u
/* REGISTER_COUNT                        MW(1136:1128)                    */
#define REGISTER_COUNT_HI         1136u
#define REGISTER_COUNT_LO         1128u
/* BARRIER_COUNT                         MW(1141:1137)                    */
#define BARRIER_COUNT_HI          1141u
#define BARRIER_COUNT_LO          1137u
/* SHARED_MEMORY_SIZE_SHIFTED7           MW(1162:1152)                    */
#define SMEM_SIZE_HI              1162u
#define SMEM_SIZE_LO              1152u
/* SHADER_LOCAL_MEMORY_LOW_SIZE_SHIFTED4 MW(1199:1184)                    */
#define SLM_LOW_SIZE_HI           1199u
#define SLM_LOW_SIZE_LO           1184u
/* SHADER_LOCAL_MEMORY_HIGH_SIZE_SHIFTED4 MW(1215:1200)                   */
#define SLM_HIGH_SIZE_HI          1215u
#define SLM_HIGH_SIZE_LO          1200u
/* GRID_WIDTH                            MW(1279:1248)                    */
#define GRID_W_HI                 1279u
#define GRID_W_LO                 1248u
/* GRID_HEIGHT                           MW(1295:1280)                    */
#define GRID_H_HI                 1295u
#define GRID_H_LO                 1280u
/* GRID_DEPTH                            MW(1327:1312)                    */
#define GRID_D_HI                 1327u
#define GRID_D_LO                 1312u
/* CONSTANT_BUFFER_ADDR_LOWER_SHIFTED6(i) MW((1375+i*64):(1344+i*64))     */
#define CB_ADDR_LO_HI(i)          (1375u + (i) * 64u)
#define CB_ADDR_LO_LO(i)          (1344u + (i) * 64u)
/* CONSTANT_BUFFER_ADDR_UPPER_SHIFTED6(i) MW((1394+i*64):(1376+i*64))     */
#define CB_ADDR_HI_HI(i)          (1394u + (i) * 64u)
#define CB_ADDR_HI_LO(i)          (1376u + (i) * 64u)
/* CONSTANT_BUFFER_SIZE_SHIFTED4(i)      MW((1407+i*64):(1395+i*64))      */
#define CB_SIZE_HI(i)             (1407u + (i) * 64u)
#define CB_SIZE_LO(i)             (1395u + (i) * 64u)
/* CONSTANT_BUFFER_VALID(i)              MW((1856+i*4):(1856+i*4))        */
#define CB_VALID_BIT(i)           (1856u + (i) * 4u)

/* ── Field setters ─────────────────────────────────────────────────── */

static void mc_qmd_init(uint8_t *qmd)
{
    memset(qmd, 0, MC_QMD_V05_BYTES);

    /* NVK qmd_init!(bv, clcdc0, QMDV05_00, 5, 0) -- major 5, minor 0, plus
     * the two enum defaults it always sets. */
    qmd_set_bits(qmd, QMD_MAJOR_VERSION_HI, QMD_MAJOR_VERSION_LO, 5);
    qmd_set_bits(qmd, QMD_MINOR_VERSION_HI, QMD_MINOR_VERSION_LO, 0);
    qmd_set_bits(qmd, API_VIS_CALL_LIMIT_BIT, API_VIS_CALL_LIMIT_BIT,
                 API_VIS_CALL_LIMIT_NO_CHECK);
    qmd_set_bits(qmd, SAMPLER_INDEX_BIT, SAMPLER_INDEX_BIT,
                 SAMPLER_INDEX_INDEPENDENTLY);

    /* Both set explicitly by Qmd5_0::new (qmd.rs:816-817).  QMD_GROUP_ID has
     * no V04 counterpart in NVK's builder -- it is new work for V05, and
     * leaving it zero is not the same as not setting it. */
    qmd_set_bits(qmd, QMD_TYPE_HI, QMD_TYPE_LO, QMD_TYPE_GRID_CTA);
    qmd_set_bits(qmd, QMD_GROUP_ID_HI, QMD_GROUP_ID_LO, QMD_GROUP_ID_VALUE);
}

static void mc_qmd_set_barrier_count(uint8_t *qmd, uint8_t n)
{
    qmd_set_bits(qmd, BARRIER_COUNT_HI, BARRIER_COUNT_LO, n);
}

static void mc_qmd_set_global_size(uint8_t *qmd, uint32_t w, uint32_t h, uint32_t d)
{
    qmd_set_bits(qmd, GRID_W_HI, GRID_W_LO, w);
    qmd_set_bits(qmd, GRID_H_HI, GRID_H_LO, h);
    qmd_set_bits(qmd, GRID_D_HI, GRID_D_LO, d);
}

static void mc_qmd_set_local_size(uint8_t *qmd, uint16_t w, uint16_t h, uint16_t d)
{
    /* DIMENSION2 is 8 bits wide in V05; a depth that does not fit would be
     * silently truncated by the writer's mask, so catch it here. */
    assert(d <= 0xFFu);
    qmd_set_bits(qmd, CTA_DIM0_HI, CTA_DIM0_LO, w);
    qmd_set_bits(qmd, CTA_DIM1_HI, CTA_DIM1_LO, h);
    qmd_set_bits(qmd, CTA_DIM2_HI, CTA_DIM2_LO, d);
}

static void mc_qmd_set_prog_addr(uint8_t *qmd, uint64_t prog_gpu_va)
{
    /* NVK qmd_impl_set_prog_addr_64!(clcdc0, QMDV05_00, SHIFTED4): the
     * address is written >>4, and NVK asserts the shift is lossless.  V04
     * took the raw 64-bit VA with no shift at all. */
    uint64_t shifted = prog_gpu_va >> 4;
    assert((shifted << 4) == prog_gpu_va && "program address must be 16-byte aligned");

    qmd_set_bits(qmd, PROG_ADDR_LO_HI, PROG_ADDR_LO_LO, (uint32_t)shifted);
    qmd_set_bits(qmd, PROG_ADDR_HI_HI, PROG_ADDR_HI_LO, shifted >> 32);
}

static void mc_qmd_set_register_count(uint8_t *qmd, uint8_t n)
{
    qmd_set_bits(qmd, REGISTER_COUNT_HI, REGISTER_COUNT_LO, n);
}

static void mc_qmd_set_slm_size(uint8_t *qmd, uint32_t bytes)
{
    /* NVK qmd_impl_set_slm_size!(clcdc0, QMDV05_00, SHIFTED4): round up to a
     * 16-byte multiple, write LOW >>4 and HIGH = 0.  The shift is what V04
     * did not have. */
    uint32_t rounded = (bytes + 0xFu) & ~0xFu;
    qmd_set_bits(qmd, SLM_HIGH_SIZE_HI, SLM_HIGH_SIZE_LO, 0);
    qmd_set_bits(qmd, SLM_LOW_SIZE_HI,  SLM_LOW_SIZE_LO,  rounded >> 4);
}

static void mc_qmd_set_smem_size(uint8_t *qmd, uint32_t bytes)
{
    /* NVK qmd_impl_set_smem_size_bounded_gb! rounds up to 0x100, writes
     * SHARED_MEMORY_SIZE_SHIFTED7, then MIN/MAX/TARGET_SM_CONFIG_* from
     * gv100_get_hw_smem_sizes.
     *
     * Both embedded kernels use zero shared memory, so the shifted size is
     * zero and the MIN/MAX/TARGET fields are the only part not ported.  This
     * is the same simplification arch/hopper/qmd_v04.c makes and asserts,
     * kept deliberately identical so the two read alike -- port
     * gv100_get_hw_smem_sizes into both if a kernel ever needs SMEM. */
    uint32_t rounded = (bytes + 0xFFu) & ~0xFFu;
    assert(rounded == 0 && "non-zero SMEM not implemented; port gv100_get_hw_smem_sizes if needed");
    qmd_set_bits(qmd, SMEM_SIZE_HI, SMEM_SIZE_LO, rounded >> 7);
}

static void mc_qmd_set_cbuf(uint8_t *qmd, uint8_t idx, uint64_t addr, uint32_t size)
{
    /* NVK qmd_impl_set_cbuf!(clcdc0, QMDV05_00, SHIFTED6, SHIFTED4): address
     * >>6 (64-byte aligned), size >>4 (16-byte aligned), then mark the slot
     * valid.  Same encoding as V04 -- only the bit positions moved. */
    if (idx >= 8) return;
    assert((addr & 0x3F) == 0);
    assert((size & 0x0F) == 0);

    uint64_t addr_s6 = addr >> 6;
    uint32_t size_s4 = size >> 4;

    qmd_set_bits(qmd, CB_ADDR_LO_HI(idx), CB_ADDR_LO_LO(idx), (uint32_t)addr_s6);
    qmd_set_bits(qmd, CB_ADDR_HI_HI(idx), CB_ADDR_HI_LO(idx), addr_s6 >> 32);
    qmd_set_bits(qmd, CB_SIZE_HI(idx),    CB_SIZE_LO(idx),    size_s4);
    qmd_set_bits(qmd, CB_VALID_BIT(idx),  CB_VALID_BIT(idx),  1);
}

/* ── The operation the library actually needs ─────────────────────────────
 *
 * One QMD, single CTA of a single thread, running `prog_gpu_va` with constant
 * buffer 0 bound at `cb0_gpu_va` -- the shape of every launch libmc makes.
 * The setters above stay private: they are how a V05 QMD is assembled, not
 * what a caller asks for.
 */
static void blackwell_build_qmd(uint8_t *qmd,
                                uint64_t prog_gpu_va, uint32_t register_count,
                                uint64_t cb0_gpu_va,  uint32_t cb0_bytes)
{
    mc_qmd_init(qmd);
    mc_qmd_set_global_size   (qmd, 1, 1, 1);
    mc_qmd_set_local_size    (qmd, 1, 1, 1);
    mc_qmd_set_prog_addr     (qmd, prog_gpu_va);
    mc_qmd_set_register_count(qmd, (uint8_t)register_count);
    mc_qmd_set_barrier_count (qmd, 0);
    mc_qmd_set_slm_size      (qmd, 0);
    mc_qmd_set_smem_size     (qmd, 0);
    mc_qmd_set_cbuf          (qmd, 0, cb0_gpu_va, cb0_bytes);
}

const mc_compute_ops_t mc_arch_blackwell_compute = {
    .qmd_bytes     = MC_QMD_V05_BYTES,
    .qmd_align     = 256,
    .cbuf_align    = 0x100,
    .version_major = 5,
    .version_minor = 0,

    /* sm_120's, read off `cuobjdump --dump-sass` of this tree's own
     * blackwell store_dword cubin -- NOT carried from another architecture,
     * which hal/arch.h is explicit about:
     *
     *   LDC.64 R2,   c[0x0][0x380]     ; dst  (two dwords, 0x380/0x384)
     *   LDC   R5,    c[0x0][0x388]     ; token
     *   LDCU.64 UR4, c[0x0][0x358]     ; global memdesc, must read 0
     *
     * CONSTANT[0] is 908 bytes = 0x38C, which is exactly the end of the
     * token dword, so cb0_bytes is 0x38C.  The memdesc at 0x358 selects
     * raw-VA addressing when zero, and hal/cbuf.c zeroes the whole buffer
     * before writing the three argument dwords, so it is handled.
     *
     * For scale, sm_90 puts these at 0x210/0x214/0x218 and sm_80 at
     * 0x160/0x164/0x168. */
    .cb0_bytes      = 0x38Cu,
    .cb0_off_dst_lo = 0x380u,
    .cb0_off_dst_hi = 0x384u,
    .cb0_off_token  = 0x388u,

    .build_qmd     = blackwell_build_qmd,
};
