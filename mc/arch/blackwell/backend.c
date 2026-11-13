/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * arch/blackwell/backend.c — the Blackwell GB20x architecture descriptor,
 * claimed for GB206.
 *
 * What this backend covers
 * ------------------------
 * The host-authored path: VA spaces, carriers, a CE channel, and copies.
 * Compute is deliberately absent -- see "No compute" below -- which the
 * descriptor expresses as a NULL compute pointer and empty kernel blobs.
 * hal/arch.h calls that a normal state rather than an error: the features
 * that need compute report unavailable through mc_feature_supported(), and
 * everything else works.
 *
 * Established on hardware, not inferred
 * -------------------------------------
 * An RTX PRO 2000 Blackwell Generation Laptop GPU (GB206) running driver
 * 620.70.06 reported architecture=0x1b0 implementation=0x6 and the class
 * list below.  The engine classes it offers, read back through the
 * MC_ARCH_FORCE bring-up path that hal/detect.c provides for exactly this
 * purpose:
 *
 *     0xc761  BLACKWELL_USERMODE_A
 *     0xc96f  BLACKWELL_CHANNEL_GPFIFO_A
 *     0xca6f  BLACKWELL_CHANNEL_GPFIFO_B
 *     0xcab5  BLACKWELL_DMA_COPY_B
 *     0xcec0  BLACKWELL_COMPUTE_B
 *     0xa06c  KEPLER_CHANNEL_GROUP_A
 *     0x90f1  FERMI_VASPACE_A
 *     0x50a0  NV50_MEMORY_VIRTUAL
 *
 * Note what is NOT there: BLACKWELL_DMA_COPY_A (0xc9b5) and
 * BLACKWELL_COMPUTE_A (0xcdc0).  GB206 carries the _B forms only, which is
 * the same split arch/ampere_ga10x/backend.c documents for GA10x against
 * GA100 -- and the same reason that backend exists.  A descriptor built from
 * the _A class names would compile, then fail the class-list check at init.
 *
 * Which implementations are claimed, and why not all of them
 * ----------------------------------------------------------
 * ARCHITECTURE_GB200 (0x1b0) is the family id for all of GB20x, not just the
 * GB200 part: ctrl2080mc.h lists GB200, GB202, GB203, GB205, GB206, GB207,
 * GB20B and GB20C under "valid ARCHITECTURE_GB20x implementation values".
 * Two of those have been run, and the mask names exactly those two:
 *
 *   GB206 (6)  RTX PRO 2000 Blackwell Generation Laptop GPU, driver
 *              620.70.06.  Full matrix: 35/35 passed, 33 skipped for the
 *              absent compute; mc_demo 7.24 GB/s D2H peak.
 *   GB202 (2)  RTX PRO 5000 Blackwell (48 GiB), driver 595.71.05.  Full
 *              matrix: 35/35 passed, the same 33 skipped for the absent
 *              compute; mc_demo 28.23 GB/s D2H peak.  Selected by
 *              detection on its own merits, not forced.
 *
 *              Its feature set is identical to GB206's, including the two
 *              that depend on the BAR1 usermode aperture -- carrier_fb
 *              (mc_carrier_host_demo --fb, 28.16 GB/s) and
 *              ce_doorbell_ring (mc_dbell_demo and every
 *              mc_dbell_chain_demo cell).  Those are the ones worth naming:
 *              a copy benchmark alone would not have exercised the
 *              doorbell PTE.
 *
 * Two dies at opposite ends of GB20x -- a 48 GiB workstation card and a
 * laptop part, on drivers five releases apart -- agreeing on the whole class
 * set is good evidence that this descriptor describes GB20x and not one
 * chip.  It is not evidence about the parts nobody has run, so they stay
 * out: this keeps the precedent arch/ampere_ga10x sets by declining GA10B,
 * and it is deliberately stricter than the "mask == 0 means any" default
 * hal/arch.h recommends.
 *
 * GB20B and GB20C are the ones to be most careful with.  They bind to a
 * different USERD-address HAL than GB206 and GB202 do (see the cap note
 * below -- the reach works out the same, but the binding differs), and the
 * GA10B precedent is that the SoC parts of a family are where the topology
 * stops matching.  Each remaining part is a one-bit change that belongs with
 * a run on it, not ahead of one.
 *
 * Compute: launches yes, SM-authored submission not yet
 * ------------------------------------------------------
 * `compute` is arch/blackwell/qmd_v05.c, a QMD V05 builder written from Mesa
 * NVK the way the V03 and V04 builders were, under the QMD exception
 * CLAUDE.md grants (no QMD header past Kepler's ships in the driver tree).
 * `kernels.store_dword` is this tree's own sm_120 cubin.  Together those are
 * what MC_FEAT_COMPUTE_LAUNCH needs, and it is offered.
 *
 * `kernels.sm_owner` is this tree's sm_120 build of the same kernel, so
 * MC_FEAT_XFER_SM is offered too and the whole matrix runs: 68/68 passed,
 * nothing skipped.  Its SASS was reviewed rather than merely counted --
 * sm_90 and sm_120 lower it to the identical store/fence sequence, 16
 * pushbuffer dwords, then a fence before each of the GPFIFO entry, the
 * USERD GPPut and the doorbell.
 *
 * The one assumption left, stated plainly
 * ---------------------------------------
 * libmc allocates BLACKWELL_COMPUTE_B (0xcec0) and drives it with the C7C0
 * method dialect the shared emitters in hal/methods.c write.  class/clcec0.h
 * is a stub -- 30 lines, a class id, no method addresses -- so 0xcec0's
 * dialect cannot be asserted directly.  Three things make this the same kind
 * of claim Hopper already rests on rather than a guess:
 *
 *   - Hopper does exactly this: it allocates HOPPER_COMPUTE_A (0xcbc0) and
 *     is driven with 0xC7C0.  Class id and method dialect are independent
 *     axes, which hal/method_abi.h exists to say.
 *   - BLACKWELL_COMPUTE_A (0xcdc0) ships a full header and agrees with C7C0
 *     on every method libmc emits; method_abi.h asserts each one, so a
 *     divergence is a compile error.
 *   - Within a generation, the _A and _B compute classes agree: method_abi.h
 *     asserts NVC6C0 (AMPERE_COMPUTE_A) against NVC7C0 (AMPERE_COMPUTE_B)
 *     for the same method set.  Mesa reaches the same conclusion for
 *     Blackwell from the QMD side -- clcdc0qmd.h and clcec0qmd.h are
 *     byte-identical apart from their prefix.
 *
 * And then it was run: see the GB206 line below.  A wrong method address
 * here does not corrupt anything silently, it faults the channel.
 */

#include "class/cl50a0.h"        /* NV50_MEMORY_VIRTUAL */
#include "class/cl90f1.h"        /* FERMI_VASPACE_A */
#include "class/cla06c.h"        /* KEPLER_CHANNEL_GROUP_A */
#include "class/clc761.h"        /* BLACKWELL_USERMODE_A */
#include "class/clc96f.h"        /* BLACKWELL_CHANNEL_GPFIFO_A */
#include "class/clcab5.h"        /* BLACKWELL_DMA_COPY_B */
#include "class/clcec0.h"        /* BLACKWELL_COMPUTE_B */

#include "ctrl/ctrl2080/ctrl2080mc.h"  /* ARCHITECTURE_GB200, IMPLEMENTATION_GB206 */

#include "hal/arch.h"

/* Generated from the sources under mc/kernels; see KERNEL_RULE in the Makefile. */
#include "arch/blackwell/sm_owner_sass.h"
#include "arch/blackwell/store_dword_sass.h"

extern const mc_arch_ops_t    mc_arch_blackwell_ops;
extern const mc_compute_ops_t mc_arch_blackwell_compute;
#include "internal/types.h"

const mc_arch_desc_t mc_arch_blackwell = {
    .name   = "blackwell",
    .family = MC_ARCH_BLACKWELL,

    .rm_architecture = NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GB200,  /* 0x1b0 */
    /* The GB20x parts actually run; see the header.  Each addition is one
     * bit and one hardware run. */
    .rm_impl_mask    = (1u << NV2080_CTRL_MC_ARCH_INFO_IMPLEMENTATION_GB202)
                     | (1u << NV2080_CTRL_MC_ARCH_INFO_IMPLEMENTATION_GB206),

    .cls = {
        .usermode       = BLACKWELL_USERMODE_A,        /* 0xC761 */
        .channel_gpfifo = BLACKWELL_CHANNEL_GPFIFO_A,  /* 0xC96F */
        /* The _B copy class, because GB206 exposes no _A.  Same situation as
         * GA10x, and the reason that is a separate backend. */
        .dma_copy       = BLACKWELL_DMA_COPY_B,        /* 0xCAB5 */
        /* Recorded for the class-list check only -- `compute` below is NULL,
         * so nothing allocates this. */
        .compute        = BLACKWELL_COMPUTE_B,         /* 0xCEC0 */
        .channel_group  = KEPLER_CHANNEL_GROUP_A,      /* 0xA06C */
        .vaspace        = FERMI_VASPACE_A,             /* 0x90F1 */
        .virtual_memory = NV50_MEMORY_VIRTUAL,         /* 0x50A0 */
    },

    /* Identical to Hopper's, from the same macros in internal/types.h, so the
     * derivations stay attached to the constants they explain.  The VF
     * doorbell in particular is invariant at +0x90 from Ampere through
     * Blackwell; stating it per backend rather than sharing a constant is
     * what tests/unit/mc_arch_desc_test.c checks. */
    .lay = {
        .userd_slot_bytes      = 512,
        .userd_offset          = MC_USERD_OFFSET,
        .vf_doorbell_offset    = MC_VF_DOORBELL_OFFSET,
        .usermode_window_bytes = MC_USERMODE_SIZE,
        .big_page_bytes        = MC_VA_POOL_ALIGN_BYTES,
        .vidmem_align_bytes    = MC_VIDMEM_ALIGN_BYTES,
        .gpfifo_entries        = MC_GPFIFO_ENTRIES,
    },

    .ops     = &mc_arch_blackwell_ops,
    .compute = &mc_arch_blackwell_compute,   /* QMD V05, arch/blackwell/qmd_v05.c */

    .kernels = {
        .sm_owner = { mc_sm_owner_kernel_sass, MC_SM_OWNER_SASS_LEN,
                      MC_SM_OWNER_REGS },
        .store_dword = { mc_store_dword_kernel_sass, MC_STORE_DWORD_SASS_LEN,
                         MC_STORE_DWORD_REGS },
    },

    .cap = {
        /* BLACKWELL_USERMODE_A is RS_OPTIONAL(NV_HOPPER_USERMODE_A_PARAMS) --
         * the same parameter struct Hopper uses, not merely a similar one:
         * src/nvidia/src/kernel/rmapi/resource_list.h:960-964 against
         * :950-954.  That is why ops.c can fill in one struct for both. */
        .usermode_takes_params = true,
        .usermode_bar1_capable = true,

        /* NVC96F has GP_ENTRY0_PB_EXTENDED_BASE_OPERAND, so a pushbuffer at
         * or above 2^40 is addressable. */
        .gpfifo_has_ext_base   = true,

        /* Nvc96fControl publishes GPPut at 0x8c and NO GPGet at all -- the
         * divergence hal/arch.h cites when explaining why the getter is a
         * try_ form.  Confirmed in class/clc96f.h; Nvca6fControl (_B) is the
         * same.  Progress polling must use the semaphore fallback. */
        .userd_exposes_gpget   = false,

        /* GB206 is not in the GB10B branch of the USERD-address HAL, so it
         * takes the GH100 default: NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW is 20
         * bits, reaching 2^52, and USERD may live in sysmem.
         *
         * From the driver's own dispatch at
         * src/nvidia/generated/g_kernel_channel_nvoc.c:1126-1134 -- the
         * GB10B arm names "GB10B | GB10C | GB20B | GB20C" and GB206 is in
         * none of them, so the else arm binds
         * kchannelIsUserdAddrSizeValid_GH100.  (The published swref headers
         * stop at blackwell/gb10b/dev_ram.h; there is no gb20x dev_ram.h to
         * read the field width from directly, which is why the HAL binding is
         * the evidence here rather than the register header.)
         *
         * tests/unit/mc_userd_addr_test.c already encodes this for the
         * family and checks this flag against it. */
        .userd_requires_vidmem = false,
    },
};
