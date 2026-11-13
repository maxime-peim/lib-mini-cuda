/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * mc_qmd_test — QMD layout, per version, without a GPU.
 *
 * A QMD is dereferenced by SKED.  A field in the wrong place does not return
 * an error; it produces an Xid, or a launch that reads the wrong memory.
 * That makes the QMD builders the code here least forgiving of a silent
 * mistake, and the most worth pinning down on the host.
 *
 * Two kinds of check:
 *
 *   1. Golden dwords.  Each builder runs with distinctive inputs and its
 *      complete set of non-zero dwords is compared against a frozen table.
 *      This is the check with teeth: it does not use the builders' own
 *      bit-range macros, so a moved field, a mis-indexed dword, and one
 *      field clobbering another all fail here.
 *
 *   2. Structural claims separating V03 from V04, taken from Mesa NVK's
 *      Qmd3_0 / Qmd4_0.  "V03 is V04 with fields moved" is false, and these
 *      stop it creeping back in through a copy-paste.
 *
 * The golden tables were produced with MC_QMD_DUMP=1 and then checked by hand
 * against the MW(hi:lo) ranges quoted in each builder.  A diff means a real
 * regression or a deliberate layout change; in the second case re-derive from
 * the header rather than pasting the new output.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal/arch.h"

static int failures;

#define EXPECT_MSG(cond, ...)                                           \
  do {                                                                  \
    if (!(cond)) {                                                      \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
      printf(__VA_ARGS__);                                              \
      printf("\n");                                                     \
      failures++;                                                       \
    }                                                                   \
  } while (0)

/* A check must not announce PASS if it added a failure.  Without this a
 * failing run prints both FAIL and PASS lines for the same check, and the
 * matrix greps for PASS.  Same idiom as tests/unit/mc_methods_test.c. */
#define BEGIN_CHECK() int failures_at_entry = failures
#define END_CHECK(name)                                                 \
  do {                                                                  \
    if (failures == failures_at_entry) printf("PASS: %s\n", (name));    \
  } while (0)


extern const mc_compute_ops_t mc_arch_ampere_compute;   /* QMD V03 */
extern const mc_compute_ops_t mc_arch_hopper_compute;   /* QMD V04 */
extern const mc_compute_ops_t mc_arch_blackwell_compute;/* QMD V05 */

/* Distinctive inputs: every nibble different, so a misplaced field shows as a
 * wrong value rather than an accidental match.  Both addresses are 256-byte
 * aligned, as every real allocation is, and fit V03's 49-bit address fields. */
#define TEST_PROG_VA   0x0000123456789A00ULL
#define TEST_CB0_VA    0x00001BCDEF012300ULL
#define TEST_REGS      40u
#define TEST_CB0_BYTES 0x200u

struct golden_dw { unsigned idx; uint32_t val; };

/* V03 (Ampere, Ada).  Hand-checked:
 *   dw 4  bit 134            SM_GLOBAL_CACHING_ENABLE
 *   dw 11 bit 378            API_VISIBLE_CALL_LIMIT = NO_CHECK
 *   dw 12/13/14              CTA_RASTER_WIDTH/HEIGHT/DEPTH = 1
 *   dw 18 = 0x00010030       MAJOR_VERSION 3 at bits 4-7, THREAD_DIM0 = 1
 *   dw 19 = 0x00010001       CTA_THREAD_DIMENSION1 and 2
 *   dw 20 = 0x00002801       CONSTANT_BUFFER_VALID(0) bit 640,
 *                            REGISTER_COUNT_V 40 at bit 648
 *   dw 32/33                 cbuf addr UNSHIFTED, plus size>>4 at bit 1075
 *   dw 48/49                 PROGRAM_ADDRESS lower / upper
 */
static const struct golden_dw v03_golden[] = {
    {   4, 0x00000040u },
    {  11, 0x04000000u },
    {  12, 0x00000001u },
    {  13, 0x00000001u },
    {  14, 0x00000001u },
    {  18, 0x00010030u },
    {  19, 0x00010001u },
    {  20, 0x00002801u },
    {  32, 0xef012300u },
    {  33, 0x01001bcdu },
    {  48, 0x56789a00u },
    {  49, 0x00001234u },
};

/* V04 (Hopper).  Hand-checked:
 *   dw 0  = 0x01000000       QMD_TYPE = GRID_CTA (2) at bit 23
 *   dw 13 bit 416            CONSTANT_BUFFER_VALID(0)
 *   dw 17 bit 568            API_VISIBLE_CALL_LIMIT = NO_CHECK
 *   dw 18 = 0x00000040       MAJOR_VERSION 4 at bits 4-7
 *   dw 32/33/34              GRID_WIDTH/HEIGHT/DEPTH = 1
 *   dw 36/37                 CTA_THREAD_DIMENSION0..2, REGISTER_COUNT 40
 *   dw 38/39                 PROGRAM_ADDRESS lower / upper
 *   dw 48/49                 cbuf addr SHIFTED6, plus size>>4 at bit 1587
 * Note dw 0 and the absence of any caching bit: the two places V04 differs
 * from V03 in what `new()` writes at all.
 */
static const struct golden_dw v04_golden[] = {
    {   0, 0x01000000u },
    {  13, 0x00000001u },
    {  17, 0x01000000u },
    {  18, 0x00000040u },
    {  32, 0x00000001u },
    {  33, 0x00000001u },
    {  34, 0x00000001u },
    {  36, 0x00010001u },
    {  37, 0x00280001u },
    {  38, 0x56789a00u },
    {  39, 0x00001234u },
    {  48, 0x37bc048cu },
    {  49, 0x0100006fu },
};

/* V05 (Blackwell).  Hand-checked against Mesa's clcec0qmd.h, field by field:
 *   dw 4  = 0x011f0000   QMD_GROUP_ID 0x1f at bits 144-149, and
 *                        QMD_TYPE = GRID_CTA (2) at bits 151-153
 *   dw 14 = 0x00500100   MAJOR_VERSION 5 at bits 468-471,
 *                        API_VISIBLE_CALL_LIMIT = NO_CHECK at bit 456
 *   dw 32/33             PROGRAM_ADDRESS SHIFTED4: 0x123456789a00 >> 4
 *                        = 0x123456789a0, split 32/21
 *   dw 34 = 0x00010001   CTA_THREAD_DIMENSION0 and 1
 *   dw 35 = 0x00002801   DIMENSION2 (8 bits) = 1, REGISTER_COUNT 40 at bit 1128
 *   dw 39/40/41          GRID_WIDTH/HEIGHT/DEPTH = 1
 *   dw 42/43             cbuf addr SHIFTED6 + size>>4 at bit 1395
 *   dw 58 bit 1856       CONSTANT_BUFFER_VALID(0)
 *
 * Two things worth seeing side by side with v04_golden above.  dw 42/43 hold
 * exactly the values V04 puts in dw 48/49 -- the cbuf encoding carried over
 * unchanged while the position moved.  And V05 writes nothing at all in dw 0,
 * 13, 17 or 18, where V04 keeps QMD_TYPE, CONSTANT_BUFFER_VALID,
 * API_VISIBLE_CALL_LIMIT and its version nibbles: same size, same encodings,
 * almost no overlap in placement.
 */
static const struct golden_dw v05_golden[] = {
    {   4, 0x011f0000u },
    {  14, 0x00500100u },
    {  32, 0x456789a0u },
    {  33, 0x00000123u },
    {  34, 0x00010001u },
    {  35, 0x00002801u },
    {  39, 0x00000001u },
    {  40, 0x00000001u },
    {  41, 0x00000001u },
    {  42, 0x37bc048cu },
    {  43, 0x0100006fu },
    {  58, 0x00000001u },
};

/* MC_QMD_DUMP=1 prints each builder's non-zero dwords in table form. */
static void dump_nonzero(const char *what, const uint8_t *qmd, uint32_t bytes)
{
    const uint32_t *dw = (const uint32_t *)qmd;
    unsigned i;
    if (getenv("MC_QMD_DUMP") == NULL) return;
    printf("  %s non-zero dwords:\n", what);
    for (i = 0; i < bytes / 4u; i++)
        if (dw[i] != 0)
            printf("    { %3u, 0x%08xu },\n", i, dw[i]);
}

static void check_golden(const char *what, const uint8_t *qmd, uint32_t bytes,
                         const struct golden_dw *want, size_t n_want)
{
    const uint32_t *dw = (const uint32_t *)qmd;
    unsigned n_dw = bytes / 4u;
    size_t   w = 0;
    unsigned i, n_nonzero = 0;

    for (i = 0; i < n_dw; i++)
        if (dw[i] != 0) n_nonzero++;

    EXPECT_MSG(n_nonzero == n_want, "%s has %u non-zero dwords, expected %zu",
               what, n_nonzero, n_want);

    for (i = 0; i < n_dw; i++) {
        if (dw[i] == 0) continue;
        if (w >= n_want) {
            EXPECT_MSG(0, "%s dword %u = 0x%08x is past the golden table",
                       what, i, dw[i]);
            return;
        }
        EXPECT_MSG(want[w].idx == i && want[w].val == dw[i],
                   "%s dword %u = 0x%08x, expected dword %u = 0x%08x",
                   what, i, dw[i], want[w].idx, want[w].val);
        w++;
    }
}

/* Read bits [hi:lo] back out, written independently of the builders' helper
 * so a bug in one cannot mask itself in the other. */
static uint64_t get_bits(const uint8_t *qmd, unsigned hi, unsigned lo)
{
    uint64_t out = 0;
    unsigned b;
    for (b = lo; b <= hi; b++) {
        unsigned byte = b / 8u, bit = b % 8u;
        if (qmd[byte] & (1u << bit))
            out |= 1ULL << (b - lo);
    }
    return out;
}

static void build_all(uint8_t *v03, uint8_t *v04, uint8_t *v05)
{
    mc_arch_ampere_compute.build_qmd(v03, TEST_PROG_VA, TEST_REGS,
                                     TEST_CB0_VA, TEST_CB0_BYTES);
    mc_arch_hopper_compute.build_qmd(v04, TEST_PROG_VA, TEST_REGS,
                                     TEST_CB0_VA, TEST_CB0_BYTES);
    mc_arch_blackwell_compute.build_qmd(v05, TEST_PROG_VA, TEST_REGS,
                                        TEST_CB0_VA, TEST_CB0_BYTES);
}

static void test_golden_dwords(void)
{
    BEGIN_CHECK();
    uint8_t v03[256], v04[384], v05[384];

    build_all(v03, v04, v05);
    dump_nonzero("V03", v03, sizeof(v03));
    dump_nonzero("V04", v04, sizeof(v04));
    dump_nonzero("V05", v05, sizeof(v05));

    check_golden("V03", v03, sizeof(v03), v03_golden,
                 sizeof(v03_golden) / sizeof(v03_golden[0]));
    check_golden("V04", v04, sizeof(v04), v04_golden,
                 sizeof(v04_golden) / sizeof(v04_golden[0]));
    check_golden("V05", v05, sizeof(v05), v05_golden,
                 sizeof(v05_golden) / sizeof(v05_golden[0]));
    END_CHECK("golden QMD dwords for all three versions");
}

static void test_sizes_differ(void)
{
    BEGIN_CHECK();
    /* NVK: Qmd3_0 is [u32; 64], Qmd4_0 is [u32; 96].  The most load-bearing
     * difference, since everything downstream allocates by qmd_bytes. */
    EXPECT_MSG(mc_arch_ampere_compute.qmd_bytes == 256,
               "V03 qmd_bytes = %u, expected 256",
               mc_arch_ampere_compute.qmd_bytes);
    EXPECT_MSG(mc_arch_hopper_compute.qmd_bytes == 384,
               "V04 qmd_bytes = %u, expected 384",
               mc_arch_hopper_compute.qmd_bytes);
    EXPECT_MSG(mc_arch_ampere_compute.version_major == 3,
               "V03 version_major = %u", mc_arch_ampere_compute.version_major);
    EXPECT_MSG(mc_arch_hopper_compute.version_major == 4,
               "V04 version_major = %u", mc_arch_hopper_compute.version_major);

    /* V05 is [u32; 96] too (NVK Qmd5_0), so it is the counter-example to the
     * test's own name: same size as V04, almost entirely different layout.
     * Size alone never identifies a QMD version. */
    EXPECT_MSG(mc_arch_blackwell_compute.qmd_bytes == 384,
               "V05 qmd_bytes = %u, expected 384",
               mc_arch_blackwell_compute.qmd_bytes);
    EXPECT_MSG(mc_arch_blackwell_compute.version_major == 5,
               "V05 version_major = %u", mc_arch_blackwell_compute.version_major);
    END_CHECK("QMD versions differ in size, not just field placement");
}

static void test_cbuf_addr_shift_differs(void)
{
    BEGIN_CHECK();
    uint8_t v03[256], v04[384], v05[384];
    uint64_t lo, hi;

    build_all(v03, v04, v05);

    /* NVK gives V03 qmd_impl_set_cbuf!(..., NONE, SHIFTED4): the address is
     * stored whole.  V04 uses SHIFTED6.  Getting this wrong points CB0 64x
     * too low and the kernel reads garbage arguments rather than failing. */
    lo = get_bits(v03, 1055, 1024);
    hi = get_bits(v03, 1072, 1056);
    EXPECT_MSG(((hi << 32) | lo) == TEST_CB0_VA,
               "V03 cbuf addr = %#llx, expected %#llx unshifted",
               (unsigned long long)((hi << 32) | lo),
               (unsigned long long)TEST_CB0_VA);

    lo = get_bits(v04, 1567, 1536);
    hi = get_bits(v04, 1586, 1568);
    EXPECT_MSG(((hi << 32) | lo) == (TEST_CB0_VA >> 6),
               "V04 cbuf addr = %#llx, expected %#llx (>>6)",
               (unsigned long long)((hi << 32) | lo),
               (unsigned long long)(TEST_CB0_VA >> 6));

    /* Size IS shifted right by 4 on both. */
    EXPECT_MSG(get_bits(v03, 1087, 1075) == (TEST_CB0_BYTES >> 4),
               "V03 cbuf size field");
    EXPECT_MSG(get_bits(v04, 1599, 1587) == (TEST_CB0_BYTES >> 4),
               "V04 cbuf size field");

    /* V05 shifts by 6 like V04, but from entirely different bit positions --
     * the encoding carried over, the layout did not. */
    lo = get_bits(v05, 1375, 1344);
    hi = get_bits(v05, 1394, 1376);
    EXPECT_MSG(((hi << 32) | lo) == (TEST_CB0_VA >> 6),
               "V05 cbuf addr = %#llx, expected %#llx (>>6)",
               (unsigned long long)((hi << 32) | lo),
               (unsigned long long)(TEST_CB0_VA >> 6));
    EXPECT_MSG(get_bits(v05, 1407, 1395) == (TEST_CB0_BYTES >> 4),
               "V05 cbuf size field");
    END_CHECK("V03 stores cbuf addresses unshifted, V04 and V05 shift by 6");
}

static void test_program_address(void)
{
    BEGIN_CHECK();
    uint8_t v03[256], v04[384], v05[384];
    uint64_t lo, hi;

    build_all(v03, v04, v05);

    lo = get_bits(v03, 1567, 1536);
    hi = get_bits(v03, 1584, 1568);
    EXPECT_MSG(((hi << 32) | lo) == TEST_PROG_VA, "V03 program address");

    lo = get_bits(v04, 1247, 1216);
    hi = get_bits(v04, 1272, 1248);
    EXPECT_MSG(((hi << 32) | lo) == TEST_PROG_VA, "V04 program address");

    /* V05 is the odd one out: PROGRAM_ADDRESS_*_SHIFTED4, so the stored
     * value is the VA >>4.  Writing the raw VA here would send SKED to an
     * address 16x too high. */
    lo = get_bits(v05, 1055, 1024);
    hi = get_bits(v05, 1076, 1056);
    EXPECT_MSG(((hi << 32) | lo) == (TEST_PROG_VA >> 4),
               "V05 program address = %#llx, expected %#llx (>>4)",
               (unsigned long long)((hi << 32) | lo),
               (unsigned long long)(TEST_PROG_VA >> 4));
    END_CHECK("program address round-trips in all three versions");
}

static void test_version_and_caching(void)
{
    BEGIN_CHECK();
    uint8_t v03[256], v04[384], v05[384];

    build_all(v03, v04, v05);

    /* Both put the version nibbles in the same place, which is exactly why
     * the rest being different is easy to miss. */
    EXPECT_MSG(get_bits(v03, 583, 580) == 3, "V03 major version");
    EXPECT_MSG(get_bits(v03, 579, 576) == 0, "V03 minor version");
    EXPECT_MSG(get_bits(v04, 583, 580) == 4, "V04 major version");
    EXPECT_MSG(get_bits(v04, 579, 576) == 0, "V04 minor version");

    /* Qmd3_0::new sets SM_GLOBAL_CACHING_ENABLE; Qmd4_0::new does not. */
    EXPECT_MSG(get_bits(v03, 134, 134) == 1,
               "V03 must set SM_GLOBAL_CACHING_ENABLE (NVK Qmd3_0::new)");

    /* Register count: REGISTER_COUNT_V MW(656:648) on V03,
     * REGISTER_COUNT MW(1208:1200) on V04 -- different name, different place. */
    EXPECT_MSG(get_bits(v03, 656, 648) == TEST_REGS, "V03 REGISTER_COUNT_V");
    EXPECT_MSG(get_bits(v04, 1208, 1200) == TEST_REGS, "V04 REGISTER_COUNT");

    /* CONSTANT_BUFFER_VALID(0): bit 640 on V03, bit 416 on V04. */
    EXPECT_MSG(get_bits(v03, 640, 640) == 1, "V03 CONSTANT_BUFFER_VALID(0)");
    EXPECT_MSG(get_bits(v04, 416, 416) == 1, "V04 CONSTANT_BUFFER_VALID(0)");

    /* V05 moves even the version nibbles, which V03 and V04 share: major is
     * MW(471:468) and minor MW(467:464). */
    EXPECT_MSG(get_bits(v05, 471, 468) == 5, "V05 major version");
    EXPECT_MSG(get_bits(v05, 467, 464) == 0, "V05 minor version");
    EXPECT_MSG(get_bits(v05, 1136, 1128) == TEST_REGS, "V05 REGISTER_COUNT");
    EXPECT_MSG(get_bits(v05, 1856, 1856) == 1, "V05 CONSTANT_BUFFER_VALID(0)");

    /* QMD_GROUP_ID = 0x1f is set by NVK's Qmd5_0::new and has no V04
     * counterpart; leaving it zero is not the same as not setting it. */
    EXPECT_MSG(get_bits(v05, 149, 144) == 0x1f, "V05 QMD_GROUP_ID");
    EXPECT_MSG(get_bits(v05, 153, 151) == 2, "V05 QMD_TYPE = GRID_CTA");
    END_CHECK("version, caching, register-count and cbuf-valid fields");
}

static void test_grid_dims(void)
{
    BEGIN_CHECK();
    uint8_t v03[256], v04[384], v05[384];

    build_all(v03, v04, v05);

    /* Both launch a single CTA of a single thread; the fields are named
     * CTA_RASTER_* on V03 and GRID_* on V04, at different offsets. */
    EXPECT_MSG(get_bits(v03, 415, 384) == 1, "V03 CTA_RASTER_WIDTH");
    EXPECT_MSG(get_bits(v03, 431, 416) == 1, "V03 CTA_RASTER_HEIGHT");
    EXPECT_MSG(get_bits(v03, 463, 448) == 1, "V03 CTA_RASTER_DEPTH");
    EXPECT_MSG(get_bits(v03, 607, 592) == 1, "V03 CTA_THREAD_DIMENSION0");

    EXPECT_MSG(get_bits(v04, 1055, 1024) == 1, "V04 GRID_WIDTH");
    EXPECT_MSG(get_bits(v04, 1071, 1056) == 1, "V04 GRID_HEIGHT");
    EXPECT_MSG(get_bits(v04, 1103, 1088) == 1, "V04 GRID_DEPTH");
    EXPECT_MSG(get_bits(v04, 1167, 1152) == 1, "V04 CTA_THREAD_DIMENSION0");

    EXPECT_MSG(get_bits(v05, 1279, 1248) == 1, "V05 GRID_WIDTH");
    EXPECT_MSG(get_bits(v05, 1295, 1280) == 1, "V05 GRID_HEIGHT");
    EXPECT_MSG(get_bits(v05, 1327, 1312) == 1, "V05 GRID_DEPTH");
    EXPECT_MSG(get_bits(v05, 1103, 1088) == 1, "V05 CTA_THREAD_DIMENSION0");

    /* V05's GRID_WIDTH sits where V04 keeps PROGRAM_ADDRESS_LOWER.  A V04
     * builder pointed at a V05 QMD would write a grid dimension over the
     * program address, so pin that the two really are different. */
    EXPECT_MSG(get_bits(v05, 1055, 1024) != 1 || TEST_PROG_VA == (1ULL << 4),
               "V05 bits 1055:1024 are PROGRAM_ADDRESS_LOWER_SHIFTED4, "
               "not GRID_WIDTH");
    END_CHECK("single-CTA single-thread grid dimensions");
}

static void test_no_stray_writes(void)
{
    BEGIN_CHECK();
    /* A V03 builder that used a V04 bit position would run off the end of a
     * 256-byte buffer, so this catches a pasted range most directly. */
    static uint8_t buf[512];
    unsigned i;

    memset(buf, 0xA5, sizeof(buf));
    mc_arch_ampere_compute.build_qmd(buf, TEST_PROG_VA, TEST_REGS,
                                     TEST_CB0_VA, TEST_CB0_BYTES);
    for (i = 256; i < sizeof(buf); i++)
        if (buf[i] != 0xA5) {
            EXPECT_MSG(0, "V03 builder wrote past 256 bytes, at offset %u", i);
            break;
        }

    memset(buf, 0xA5, sizeof(buf));
    mc_arch_hopper_compute.build_qmd(buf, TEST_PROG_VA, TEST_REGS,
                                     TEST_CB0_VA, TEST_CB0_BYTES);
    for (i = 384; i < sizeof(buf); i++)
        if (buf[i] != 0xA5) {
            EXPECT_MSG(0, "V04 builder wrote past 384 bytes, at offset %u", i);
            break;
        }

    memset(buf, 0xA5, sizeof(buf));
    mc_arch_blackwell_compute.build_qmd(buf, TEST_PROG_VA, TEST_REGS,
                                        TEST_CB0_VA, TEST_CB0_BYTES);
    for (i = 384; i < sizeof(buf); i++)
        if (buf[i] != 0xA5) {
            EXPECT_MSG(0, "V05 builder wrote past 384 bytes, at offset %u", i);
            break;
        }
    END_CHECK("no builder writes outside its QMD");
}

static void test_cb0_offsets_are_per_arch(void)
{
    BEGIN_CHECK();
    /* nvcc places kernel parameters at a different constant-bank offset per
     * SM version.  Read off cuobjdump; if a future edit makes the two equal,
     * one was far more likely copied than NVIDIA changing the layout. */
    EXPECT_MSG(mc_arch_ampere_compute.cb0_off_dst_lo == 0x160u,
               "sm_80 dst_lo = %#x, expected 0x160",
               mc_arch_ampere_compute.cb0_off_dst_lo);
    EXPECT_MSG(mc_arch_ampere_compute.cb0_off_token == 0x168u,
               "sm_80 token = %#x, expected 0x168",
               mc_arch_ampere_compute.cb0_off_token);
    EXPECT_MSG(mc_arch_hopper_compute.cb0_off_dst_lo == 0x210u,
               "sm_90 dst_lo = %#x, expected 0x210",
               mc_arch_hopper_compute.cb0_off_dst_lo);
    EXPECT_MSG(mc_arch_blackwell_compute.cb0_off_dst_lo == 0x380u,
               "sm_120 dst_lo = %#x, expected 0x380",
               mc_arch_blackwell_compute.cb0_off_dst_lo);
    EXPECT_MSG(mc_arch_blackwell_compute.cb0_off_token == 0x388u,
               "sm_120 token = %#x, expected 0x388",
               mc_arch_blackwell_compute.cb0_off_token);
    EXPECT_MSG(mc_arch_hopper_compute.cb0_off_token == 0x218u,
               "sm_90 token = %#x, expected 0x218",
               mc_arch_hopper_compute.cb0_off_token);
    EXPECT_MSG(mc_arch_ampere_compute.cb0_off_dst_lo
               != mc_arch_hopper_compute.cb0_off_dst_lo,
               "sm_80 and sm_90 CB0 argument offsets are equal; one was "
               "probably copied from the other");

    /* The allocation must cover the last argument dword. */
    EXPECT_MSG(mc_cb0_alloc_bytes(&mc_arch_ampere_compute)
               >= mc_arch_ampere_compute.cb0_off_token + 4u,
               "sm_80 CB0 allocation does not cover its token offset");
    EXPECT_MSG(mc_cb0_alloc_bytes(&mc_arch_hopper_compute)
               >= mc_arch_hopper_compute.cb0_off_token + 4u,
               "sm_90 CB0 allocation does not cover its token offset");
    END_CHECK("CB0 argument offsets differ per SM version and are covered");
}

int main(void)
{
    test_golden_dwords();
    test_sizes_differ();
    test_cbuf_addr_shift_differs();
    test_program_address();
    test_version_and_caching();
    test_grid_dims();
    test_no_stray_writes();
    test_cb0_offsets_are_per_arch();

    if (failures) {
        printf("%d QMD check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
