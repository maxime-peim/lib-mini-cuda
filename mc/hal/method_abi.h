/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * hal/method_abi.h — compile-time proof that the method encodings libmc
 * emits are the same across the channel, copy and compute classes of every
 * architecture it targets.
 *
 * Why this file exists
 * --------------------
 * hal/methods.c emits ONE method stream and parameterises only the class id
 * in SET_OBJECT.  That is a claim: that NVC8B5's method addresses and
 * LAUNCH_DMA bit fields, NVC86F's GP-entry layout, and NVC7C0's compute
 * methods are shared by the Ampere, Ada and Blackwell classes rather than
 * being Hopper-specific.
 *
 * CLAUDE.md requires that a GPU command bit field be established from the
 * pinned driver headers rather than assumed.  So instead of asserting the
 * claim in a comment, this header derives it from those headers with the
 * SDK's own DRF_* macros.  If a future class moves a field or a method
 * address, `make libmc` fails with the name of what moved -- rather than the
 * GPU faulting at runtime, which is what a wrong method word actually costs.
 *
 * These are all compile-time constant expressions; the file emits no code.
 *
 * Adding an architecture: add its class headers below and a block of
 * asserts.  If a class genuinely diverges, that is a real finding -- do not
 * delete the assert, give that class its own emitter and record why.
 *
 * Coverage is bounded by what the pinned tree actually ships.  Two classes
 * we target have header stubs containing only a class id and no method
 * defines at all -- BLACKWELL_DMA_COPY_A (class/clc9b5.h) and
 * ADA_COMPUTE_A (class/clc9c0.h) -- so there is nothing to compare for
 * them; they carry no distinct encoding of their own.  BLACKWELL_DMA_COPY_B
 * (class/clcab5.h) ships only LAUNCH_DMA and DATA_TRANSFER_TYPE, so those
 * are the only Blackwell copy-engine fields asserted here.  Absence of an
 * assert below means the header is silent, NOT that the field was checked
 * and found to differ.
 */
#ifndef MC_HAL_METHOD_ABI_H
#define MC_HAL_METHOD_ABI_H

#include "nvmisc.h"              /* DRF_SHIFT / DRF_MASK */

/* Channel (GPFIFO) classes: Volta (method headers), Ampere, Hopper, Blackwell. */
#include "class/clc36f.h"
#include "class/clc56f.h"
#include "class/clc86f.h"
#include "class/clc96f.h"

/* Copy-engine classes: Ampere A/B, Hopper, Blackwell A/B. */
#include "class/clc6b5.h"
#include "class/clc7b5.h"
#include "class/clc8b5.h"
#include "class/clcab5.h"

/* Compute classes: Ampere B (the dialect Hopper inherits), Ada, Blackwell. */
#include "class/clc6c0.h"        /* AMPERE_COMPUTE_A dialect (GA100) */
#include "class/clc7c0.h"
#include "class/clcdc0.h"

/* Compare a whole DRF field -- both its position and its width. */
#define MC_ASSERT_FIELD_SAME(a, b, what)                                      \
  _Static_assert(DRF_SHIFT(a) == DRF_SHIFT(b) && DRF_MASK(a) == DRF_MASK(b),  \
                 "method ABI divergence: " what)

/* Compare a method address. */
#define MC_ASSERT_METHOD_SAME(a, b, what)                                     \
  _Static_assert((a) == (b), "method ABI divergence: " what)

/* ── Copy-engine method addresses ────────────────────────────────────────
 * hal/methods.c writes the CE copy stream using the NVC8B5_* names. */
MC_ASSERT_METHOD_SAME(NVC6B5_OFFSET_IN_UPPER,  NVC8B5_OFFSET_IN_UPPER,  "C6B5 OFFSET_IN_UPPER");
MC_ASSERT_METHOD_SAME(NVC6B5_OFFSET_OUT_UPPER, NVC8B5_OFFSET_OUT_UPPER, "C6B5 OFFSET_OUT_UPPER");
MC_ASSERT_METHOD_SAME(NVC6B5_LINE_LENGTH_IN,   NVC8B5_LINE_LENGTH_IN,   "C6B5 LINE_LENGTH_IN");
MC_ASSERT_METHOD_SAME(NVC6B5_SET_SEMAPHORE_A,  NVC8B5_SET_SEMAPHORE_A,  "C6B5 SET_SEMAPHORE_A");
MC_ASSERT_METHOD_SAME(NVC6B5_LAUNCH_DMA,       NVC8B5_LAUNCH_DMA,       "C6B5 LAUNCH_DMA");
MC_ASSERT_METHOD_SAME(NVC7B5_LAUNCH_DMA,       NVC8B5_LAUNCH_DMA,       "C7B5 LAUNCH_DMA");
MC_ASSERT_METHOD_SAME(NVCAB5_LAUNCH_DMA,       NVC8B5_LAUNCH_DMA,       "CAB5 LAUNCH_DMA");

/* ── LAUNCH_DMA bit fields ───────────────────────────────────────────────
 * These are the flags the copy stream composes with DRF_DEF. */
MC_ASSERT_FIELD_SAME(NVC6B5_LAUNCH_DMA_DATA_TRANSFER_TYPE,
                     NVC8B5_LAUNCH_DMA_DATA_TRANSFER_TYPE, "C6B5 DATA_TRANSFER_TYPE");
MC_ASSERT_FIELD_SAME(NVC6B5_LAUNCH_DMA_FLUSH_ENABLE,
                     NVC8B5_LAUNCH_DMA_FLUSH_ENABLE, "C6B5 FLUSH_ENABLE");
MC_ASSERT_FIELD_SAME(NVC6B5_LAUNCH_DMA_SEMAPHORE_TYPE,
                     NVC8B5_LAUNCH_DMA_SEMAPHORE_TYPE, "C6B5 SEMAPHORE_TYPE");
MC_ASSERT_FIELD_SAME(NVC6B5_LAUNCH_DMA_SRC_MEMORY_LAYOUT,
                     NVC8B5_LAUNCH_DMA_SRC_MEMORY_LAYOUT, "C6B5 SRC_MEMORY_LAYOUT");
MC_ASSERT_FIELD_SAME(NVC6B5_LAUNCH_DMA_DST_MEMORY_LAYOUT,
                     NVC8B5_LAUNCH_DMA_DST_MEMORY_LAYOUT, "C6B5 DST_MEMORY_LAYOUT");
MC_ASSERT_FIELD_SAME(NVCAB5_LAUNCH_DMA_DATA_TRANSFER_TYPE,
                     NVC8B5_LAUNCH_DMA_DATA_TRANSFER_TYPE, "CAB5 DATA_TRANSFER_TYPE");

/* ── Method-stream header (DMA_INCR) ─────────────────────────────────────
 * Every emitter builds its header with the NVC36F_DMA_INCR_* fields.  Only
 * Volta (C36F) and Ampere (C56F) actually define them: clc86f.h and
 * clc96f.h define zero DMA_INCR fields, because Hopper and Blackwell
 * inherit the Volta method-header encoding rather than restating it.  That
 * is why hal/submit.c includes clc36f.h for headers while using C86F for
 * GP entries.  Ampere restating them identically is the evidence that the
 * inherited encoding is stable. */
MC_ASSERT_FIELD_SAME(NVC56F_DMA_INCR_OPCODE, NVC36F_DMA_INCR_OPCODE, "C56F DMA_INCR_OPCODE");
MC_ASSERT_FIELD_SAME(NVC56F_DMA_INCR_COUNT,  NVC36F_DMA_INCR_COUNT,  "C56F DMA_INCR_COUNT");
MC_ASSERT_FIELD_SAME(NVC56F_DMA_INCR_SUBCHANNEL, NVC36F_DMA_INCR_SUBCHANNEL, "C56F DMA_INCR_SUBCHANNEL");
MC_ASSERT_FIELD_SAME(NVC56F_DMA_INCR_ADDRESS, NVC36F_DMA_INCR_ADDRESS, "C56F DMA_INCR_ADDRESS");

/* ── GPFIFO entry layout ─────────────────────────────────────────────────
 * write_gp_entry composes entry0/entry1 with the NVC86F_* names. */
MC_ASSERT_FIELD_SAME(NVC56F_GP_ENTRY0_GET,     NVC86F_GP_ENTRY0_GET,     "C56F GP_ENTRY0_GET");
MC_ASSERT_FIELD_SAME(NVC96F_GP_ENTRY0_GET,     NVC86F_GP_ENTRY0_GET,     "C96F GP_ENTRY0_GET");
MC_ASSERT_FIELD_SAME(NVC56F_GP_ENTRY1_GET_HI,  NVC86F_GP_ENTRY1_GET_HI,  "C56F GP_ENTRY1_GET_HI");
MC_ASSERT_FIELD_SAME(NVC96F_GP_ENTRY1_GET_HI,  NVC86F_GP_ENTRY1_GET_HI,  "C96F GP_ENTRY1_GET_HI");
MC_ASSERT_FIELD_SAME(NVC56F_GP_ENTRY1_LENGTH,  NVC86F_GP_ENTRY1_LENGTH,  "C56F GP_ENTRY1_LENGTH");
MC_ASSERT_FIELD_SAME(NVC96F_GP_ENTRY1_LENGTH,  NVC86F_GP_ENTRY1_LENGTH,  "C96F GP_ENTRY1_LENGTH");
MC_ASSERT_FIELD_SAME(NVC56F_GP_ENTRY1_SYNC,    NVC86F_GP_ENTRY1_SYNC,    "C56F GP_ENTRY1_SYNC");
MC_ASSERT_FIELD_SAME(NVC96F_GP_ENTRY1_SYNC,    NVC86F_GP_ENTRY1_SYNC,    "C96F GP_ENTRY1_SYNC");

/* SET_OBJECT is method 0 on every channel class we target. */
MC_ASSERT_METHOD_SAME(NVC56F_SET_OBJECT, NVC86F_SET_OBJECT, "C56F SET_OBJECT");
MC_ASSERT_METHOD_SAME(NVC96F_SET_OBJECT, NVC86F_SET_OBJECT, "C96F SET_OBJECT");

/* ── Compute method addresses ────────────────────────────────────────────
 * hal/methods.c writes the compute stream with the NVC7C0_* names.  Note
 * that Hopper allocates HOPPER_COMPUTE_A (0xCBC0) but is driven with the
 * 0xC7C0 (AMPERE_COMPUTE_B) dialect -- the class id and the method dialect
 * are independent axes, which is exactly why these asserts matter. */
/* GA100 allocates AMPERE_COMPUTE_A (0xC6C0), a different class id from the
 * 0xC7C0 dialect the emitter writes -- so its agreement has to be checked,
 * not assumed from the two being "both Ampere". */
MC_ASSERT_METHOD_SAME(NVC6C0_SEND_PCAS_A, NVC7C0_SEND_PCAS_A, "C6C0 SEND_PCAS_A");
MC_ASSERT_METHOD_SAME(NVC6C0_SEND_SIGNALING_PCAS2_B, NVC7C0_SEND_SIGNALING_PCAS2_B, "C6C0 SEND_SIGNALING_PCAS2_B");
MC_ASSERT_METHOD_SAME(NVC6C0_SET_REPORT_SEMAPHORE_A, NVC7C0_SET_REPORT_SEMAPHORE_A, "C6C0 SET_REPORT_SEMAPHORE_A");
MC_ASSERT_METHOD_SAME(NVC6C0_SET_SHADER_SHARED_MEMORY_WINDOW_A, NVC7C0_SET_SHADER_SHARED_MEMORY_WINDOW_A, "C6C0 SHARED_MEMORY_WINDOW_A");
MC_ASSERT_METHOD_SAME(NVC6C0_SET_SHADER_LOCAL_MEMORY_WINDOW_A, NVC7C0_SET_SHADER_LOCAL_MEMORY_WINDOW_A, "C6C0 LOCAL_MEMORY_WINDOW_A");
MC_ASSERT_METHOD_SAME(NVC6C0_INVALIDATE_SHADER_CACHES_NO_WFI, NVC7C0_INVALIDATE_SHADER_CACHES_NO_WFI, "C6C0 INVALIDATE_SHADER_CACHES_NO_WFI");
MC_ASSERT_METHOD_SAME(NVC6C0_INVALIDATE_SAMPLER_CACHE_NO_WFI, NVC7C0_INVALIDATE_SAMPLER_CACHE_NO_WFI, "C6C0 INVALIDATE_SAMPLER_CACHE_NO_WFI");
MC_ASSERT_METHOD_SAME(NVC6C0_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, NVC7C0_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, "C6C0 INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI");
MC_ASSERT_FIELD_SAME(NVC6C0_SET_REPORT_SEMAPHORE_D_OPERATION,
                     NVC7C0_SET_REPORT_SEMAPHORE_D_OPERATION, "C6C0 REPORT_SEMAPHORE_D_OPERATION");
MC_ASSERT_FIELD_SAME(NVC6C0_SET_REPORT_SEMAPHORE_A_OFFSET_UPPER,
                     NVC7C0_SET_REPORT_SEMAPHORE_A_OFFSET_UPPER, "C6C0 REPORT_SEMAPHORE_A_OFFSET_UPPER");

MC_ASSERT_METHOD_SAME(NVCDC0_SEND_PCAS_A, NVC7C0_SEND_PCAS_A, "CDC0 SEND_PCAS_A");
MC_ASSERT_METHOD_SAME(NVCDC0_SEND_SIGNALING_PCAS2_B, NVC7C0_SEND_SIGNALING_PCAS2_B, "CDC0 SEND_SIGNALING_PCAS2_B");
MC_ASSERT_METHOD_SAME(NVCDC0_SET_REPORT_SEMAPHORE_A, NVC7C0_SET_REPORT_SEMAPHORE_A, "CDC0 SET_REPORT_SEMAPHORE_A");
MC_ASSERT_METHOD_SAME(NVCDC0_SET_SHADER_SHARED_MEMORY_WINDOW_A, NVC7C0_SET_SHADER_SHARED_MEMORY_WINDOW_A, "CDC0 SHARED_MEMORY_WINDOW_A");
MC_ASSERT_METHOD_SAME(NVCDC0_SET_SHADER_LOCAL_MEMORY_WINDOW_A, NVC7C0_SET_SHADER_LOCAL_MEMORY_WINDOW_A, "CDC0 LOCAL_MEMORY_WINDOW_A");
MC_ASSERT_METHOD_SAME(NVCDC0_INVALIDATE_SHADER_CACHES_NO_WFI, NVC7C0_INVALIDATE_SHADER_CACHES_NO_WFI, "CDC0 INVALIDATE_SHADER_CACHES_NO_WFI");
MC_ASSERT_METHOD_SAME(NVCDC0_INVALIDATE_SAMPLER_CACHE_NO_WFI, NVC7C0_INVALIDATE_SAMPLER_CACHE_NO_WFI, "CDC0 INVALIDATE_SAMPLER_CACHE_NO_WFI");
MC_ASSERT_METHOD_SAME(NVCDC0_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, NVC7C0_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, "CDC0 INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI");

/* ── Compute report-semaphore control word ───────────────────────────────
 *
 * NOT asserted same: SET_REPORT_SEMAPHORE_A_OFFSET_UPPER is 7:0 on C6C0 and
 * C7C0 but 24:0 on CDC0 -- Blackwell widened the field from 8 to 25 bits.
 *
 * Writing it with the narrow C7C0 mask stays correct there (the extra bits
 * are simply zero), so the shared emitter is safe; what it costs is reach.
 * An 8-bit upper limits a compute report semaphore to a GPU VA below 2^40,
 * which every address libmc uses today satisfies.  A Blackwell backend that
 * ever places one higher must widen the mask, and this is the note saying
 * why it compiles either way.
 */
MC_ASSERT_FIELD_SAME(NVCDC0_SET_REPORT_SEMAPHORE_D_OPERATION,
                     NVC7C0_SET_REPORT_SEMAPHORE_D_OPERATION, "CDC0 REPORT_SEMAPHORE_D_OPERATION");

#endif /* MC_HAL_METHOD_ABI_H */
