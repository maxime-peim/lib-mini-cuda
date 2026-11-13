/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * hal/methods.c — every GPU method word libmc emits.
 *
 * Pure encoding: no fds, no RM handles, no fences.  The submission mechanics
 * that consume these streams live in hal/submit.c.
 *
 * hal/method_abi.h is included below purely for its side effect: it holds the
 * _Static_assert bank proving that the NVC8B5 / NVC86F / NVC7C0 encodings used
 * here are shared by the Ampere, Ada and Blackwell classes rather than being
 * Hopper-specific.  If a class ever moves one of these fields, this file
 * stops compiling and names what moved.
 */

#define _GNU_SOURCE
#include <emmintrin.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nvtypes.h"
#include "nvmisc.h"
#include "class/clc8b5.h"        /* NVC8B5_* copy methods (class id
                                  * itself comes from the descriptor) */
#include "class/clc86f.h"        /* NVC86F_SET_OBJECT + GP entry */
#include "class/clc36f.h"        /* NVC36F_DMA_INCR_* method headers */
#include "class/clc7c0.h"        /* compute method dialect (0xC7C0),
                                  * including INVALIDATE_*_CACHE_NO_WFI */
#include "class/clb1c0.h"        /* INVALIDATE_SKED_CACHES */
#include "class/cla06fsubch.h"   /* NVA06F_SUBCHANNEL_COPY_ENGINE / _COMPUTE */

#include "hal/arch.h"
#include "hal/method_abi.h"      /* compile-time cross-class ABI proof */
#include "hal/methods.h"
#include "internal/types.h"
#include "internal/log.h"

/*
 * Kepler-style method header.  One 32-bit word that describes the payload
 * dwords that follow; PBDMA parses it to figure out which method addresses
 * to program and on which subchannel.
 *
 *   NVC36F_DMA_INCR_OPCODE      [31:29] = NVC36F_DMA_INCR_OPCODE_VALUE (1)
 *   NVC36F_DMA_INCR_COUNT       [28:16] = number of data dwords following
 *   NVC36F_DMA_INCR_SUBCHANNEL  [15:13] = subchannel (CE uses 4)
 *   NVC36F_DMA_INCR_ADDRESS     [11:0]  = method_address >> 2
 *
 * Subchannel NVA06F_SUBCHANNEL_COPY_ENGINE (= 4) is not arbitrary: PBDMA
 * routes methods to the engine bound to each subchannel, and our first
 * emitted method is a SET_OBJECT that binds HOPPER_DMA_COPY_A to
 * subchannel 4.  Subchannel 0 has no engine bound and would trigger
 * Xid 32 "invalid pushbuffer stream".
 */
#define INCR_HEADER_SUB(method, count, subch)       \
  (DRF_DEF(C36F, _DMA_INCR, _OPCODE, _VALUE)        \
   | DRF_NUM(C36F, _DMA_INCR, _COUNT, (count))      \
   | DRF_NUM(C36F, _DMA_INCR, _SUBCHANNEL, (subch)) \
   | DRF_NUM(C36F, _DMA_INCR, _ADDRESS, (method) >> 2))
#define INCR_HEADER(method, count) \
  INCR_HEADER_SUB((method), (count), NVA06F_SUBCHANNEL_COPY_ENGINE)
/*
 * Build the NVC8B5 method stream for a 1D CE copy.
 *
 * Direction-agnostic: src_va and dst_va are arbitrary GPU VAs — swap
 * them to get H2D vs D2H with identical machinery.  Both H2D and D2H go
 * through the same CE (COPY0/COPY1 etc. per pick_non_grce_lce) because
 * a CE doesn't care about the direction of the transfer, only about
 * reading pages from src_va and writing them to dst_va.
 *
 * The stream is 18 dwords = 72 bytes and mirrors what libcuda emits on
 * H100 PCIe: the copy and its completion signal are two separate
 * LAUNCH_DMAs rather than one.  Launch 1 (0x182) moves the data and
 * carries neither a flush nor a semaphore; SET_SEMAPHORE_A/B/PAYLOAD then
 * program the release cell; launch 2 moves no data
 * (DATA_TRANSFER_TYPE=NONE) and exists only to flush and release.
 *
 * A single fused launch also works — 0x18e = NON_PIPELINED | FLUSH_ENABLE
 * | RELEASE_ONE_WORD_SEMAPHORE | SRC_PITCH | DST_PITCH — and that is the
 * value Yan et al. report from an A40 trace.  H100 libcuda does not use
 * it, so neither do we; the SM-authored path in kernels/sm_owner.cu still
 * does, for the reason given there.
 *
 * sema_va + sema_payload program a 4-byte release semaphore — the CE
 * writes sema_payload to sema_va once the transfer has committed (the
 * release launch carries FLUSH_ENABLE=TRUE).  The CPU polls this word to
 * detect completion.  libcuda releases four words (payload plus a
 * timestamp) here; the one-word form keeps that poll a single 32-bit
 * compare.
 *
 * Publishing this method stream to PBDMA is the channel-submit
 * primitives' job (mc_channel_submit / mc_channel_arm), which emit
 * the produce-fence-publish ordering internally; callers do not
 * sfence between mc_write_transfer_methods() and the publish step.
 */
uint32_t *mc_write_transfer_methods(const mc_arch_desc_t *arch,
                                    uint32_t *pb,
                                    uint64_t src_va, uint64_t dst_va,
                                    uint32_t nbytes,
                                    uint64_t sema_va, uint32_t sema_payload)
{
  *pb++ = INCR_HEADER(NVC86F_SET_OBJECT, 1);
  *pb++ = arch->cls.dma_copy;

  *pb++ = INCR_HEADER(NVC8B5_OFFSET_IN_UPPER, 2);
  *pb++ = DRF_NUM(C8B5, _OFFSET_IN_UPPER, _UPPER, (NvU32)(src_va >> 32));
  *pb++ = (NvU32)src_va;

  *pb++ = INCR_HEADER(NVC8B5_OFFSET_OUT_UPPER, 2);
  *pb++ = DRF_NUM(C8B5, _OFFSET_OUT_UPPER, _UPPER, (NvU32)(dst_va >> 32));
  *pb++ = (NvU32)dst_va;

  *pb++ = INCR_HEADER(NVC8B5_LINE_LENGTH_IN, 1);
  *pb++ = nbytes;

  /* EXPERIMENT: libcuda's split form.  Launch 1 is the copy alone — no
   * flush, no semaphore (SEMAPHORE_TYPE is field 4:3 and 0 means none,
   * so it is simply omitted).  This is bit-for-bit libcuda's 0x182. */
  *pb++ = INCR_HEADER(NVC8B5_LAUNCH_DMA, 1);
  *pb++ =
      DRF_DEF(C8B5, _LAUNCH_DMA, _DATA_TRANSFER_TYPE, _NON_PIPELINED)
      | DRF_DEF(C8B5, _LAUNCH_DMA, _FLUSH_ENABLE, _FALSE)
      | DRF_DEF(C8B5, _LAUNCH_DMA, _SRC_MEMORY_LAYOUT, _PITCH)
      | DRF_DEF(C8B5, _LAUNCH_DMA, _DST_MEMORY_LAYOUT, _PITCH);

  /* Then the semaphore, then launch 2: release only, moves no data.
   * libcuda uses the four-word (timestamped) release here; we keep the
   * one-word form so the host's existing 4-byte poll still works, which
   * isolates the split itself as the only variable. */
  *pb++ = INCR_HEADER(NVC8B5_SET_SEMAPHORE_A, 3);
  *pb++ = DRF_NUM(C8B5, _SET_SEMAPHORE_A, _UPPER, (NvU32)(sema_va >> 32));
  *pb++ = (NvU32)sema_va;
  *pb++ = sema_payload;

  *pb++ = INCR_HEADER(NVC8B5_LAUNCH_DMA, 1);
  *pb++ =
      DRF_DEF(C8B5, _LAUNCH_DMA, _DATA_TRANSFER_TYPE, _NONE)
      | DRF_DEF(C8B5, _LAUNCH_DMA, _FLUSH_ENABLE, _TRUE)
      | DRF_DEF(C8B5, _LAUNCH_DMA, _SEMAPHORE_TYPE, _RELEASE_ONE_WORD_SEMAPHORE);

  return pb;
}

/*
 * Write one or two 8-byte GPFIFO entries pointing PBDMA at a method stream
 * in the pushbuffer.
 *
 * Normal GP entry format (NVC86F):
 *   ENTRY0[31:2]  NVC86F_GP_ENTRY0_GET      pb_va[31:2] (stored in-place)
 *   ENTRY0[1:0]   NVC86F_GP_ENTRY0_FETCH    0 = UNCONDITIONAL
 *   ENTRY1[7:0]   NVC86F_GP_ENTRY1_GET_HI   pb_va[39:32]
 *   ENTRY1[30:10] NVC86F_GP_ENTRY1_LENGTH   length in dwords
 *   ENTRY1[31]    NVC86F_GP_ENTRY1_SYNC     0 = PROCEED
 *
 * A single normal entry encodes 40 bits of pb_va (pb_va[39:0] — 1 TiB).
 * UVM under Paper F1 routinely places user buffers above bit 40
 * (typical VAs look like 0x7xxx_xxxx_xxxx), so the extra high bits
 * come via a separate SET_PB_SEGMENT_EXTENDED_BASE entry written
 * BEFORE the normal entry.  That control entry is identified by
 * OPCODE = 0x4 in ENTRY1[7:0], with operand pb_va[56:40] in
 * ENTRY0[24:8].  Returns 1 if no ext-base needed, else 2.
 *
 * NVC86F_GP_ENTRY0_GET takes the *semantic* value (pb_va >> 2), not
 * the pre-shifted bit pattern — DRF_NUM masks to 30 bits then shifts
 * left by 2 to place pb_va[31:2] at bit positions [31:2].  Passing
 * pb_va directly would put pb_va[29:0] at ENTRY0[29:0] and trigger
 * Xid 31 when PBDMA reads 4× the wrong address.
 *
 * An sfence is emitted at the end so the GPFIFO ring BAR1 writes are
 * flushed before the caller advances GPPut.
 */
uint32_t write_gp_entry(const mc_arch_desc_t *arch,
                               volatile uint32_t *gpfifo_ring,
                               uint32_t gp_put_index, uint32_t ring_entries,
                               uintptr_t pb_cpu_va, uint32_t pb_offset,
                               uint32_t method_bytes)
{
  uintptr_t pb_va    = pb_cpu_va + pb_offset;
  /* NVC86F_GP_ENTRY1_LENGTH is a count of 32-bit dwords. */
  uint32_t  length   = method_bytes / (uint32_t)sizeof(uint32_t);
  uint32_t  consumed = 0;
  NvU32     ext_base = (NvU32)(pb_va >> 40);
  NvU32     idx, entry0, entry1;

  if (ext_base != 0)
  {
    /* A pushbuffer at or above 2^40 needs an extended-base opcode ahead of
     * the real entry.  AMPERE_CHANNEL_GPFIFO_A has no such opcode, so on
     * that class such a pushbuffer is simply unreachable.  That is a
     * bring-up bug rather than a runtime condition -- the VA pool and every
     * channel allocation sit far below 2^40 -- so fail loudly instead of
     * emitting an entry the class cannot parse. */
    if (!arch->cap.gpfifo_has_ext_base)
    {
      ERROR_LOG("pushbuffer VA 0x%" NvU64_fmtx " needs an extended-base GPFIFO "
                "entry, but %s has no PB extended-base opcode",
                (NvU64)pb_va, arch->name);
      return 0;
    }
    idx = (gp_put_index + consumed) & (ring_entries - 1);
    gpfifo_ring[idx * 2 + 0] =
        DRF_NUM(C86F, _GP_ENTRY0, _PB_EXTENDED_BASE_OPERAND, ext_base);
    gpfifo_ring[idx * 2 + 1] =
        DRF_DEF(C86F, _GP_ENTRY1, _OPCODE, _SET_PB_SEGMENT_EXTENDED_BASE);
    consumed++;
  }

  idx    = (gp_put_index + consumed) & (ring_entries - 1);
  entry0 = DRF_NUM(C86F, _GP_ENTRY0, _GET, (NvU32)(pb_va >> 2))
           | DRF_DEF(C86F, _GP_ENTRY0, _FETCH, _UNCONDITIONAL);
  entry1 = DRF_NUM(C86F, _GP_ENTRY1, _GET_HI, (NvU32)(pb_va >> 32))
           | DRF_NUM(C86F, _GP_ENTRY1, _LENGTH, length)
           | DRF_DEF(C86F, _GP_ENTRY1, _LEVEL, _MAIN)
           | DRF_DEF(C86F, _GP_ENTRY1, _SYNC, _PROCEED);
  gpfifo_ring[idx * 2 + 0] = entry0;
  gpfifo_ring[idx * 2 + 1] = entry1;
  consumed++;

  _mm_sfence();
  return consumed;
}

/* ── Compute method stream ────────────────────────────────────────────────
 *
 * Same header encoding as the CE side, pinned to the compute subchannel.
 *
 * The methods below are named NVC7C0_* (AMPERE_COMPUTE_B) even though the
 * object bound to the subchannel is HOPPER_COMPUTE_A (0xCBC0): the compute
 * class id and the method dialect it is driven with are independent axes, and
 * Hopper inherits the Ampere-B dialect.  hal/method_abi.h asserts that Ada and
 * Blackwell agree on these addresses too.
 */
#define INCR_HEADER_COMPUTE(method, count) \
  INCR_HEADER_SUB((method), (count), NVA06F_SUBCHANNEL_COMPUTE)

/* Per-channel one-time setup, modelled on Mesa NVK's
 * `nvk_push_dispatch_state_init` (vulkan/nvk_cmd_dispatch.c:31-89) +
 * `nvk_cmd_buffer_begin_compute` (same file:99-115).  NVK proves that
 * launching a Hopper compute kernel does NOT require the libcuda-only
 * MME bytecode burst, the SET_CWD_REF_COUNTER ramp, the SET_SPA_VERSION
 * / SET_QMD_VERSION / SET_RESERVED_SW_METHOD07 / SET_VALID_SPAN_OVERFLOW
 * methods, or the texture-pool methods.
 *
 * The minimal NVK-style setup is:
 *   - SET_OBJECT(HOPPER_COMPUTE_A)
 *   - INVALIDATE_SKED_CACHES                        (clear stale QMD cache)
 *   - INVALIDATE_SAMPLER_CACHE_NO_WFI(LINES_ALL=0)
 *   - INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI(LINES_ALL=0)
 *   - SET_SHADER_SHARED_MEMORY_WINDOW_A/B = 0x400000000 (16 GiB)
 *       Hopper requires this NON-ZERO and 4 GiB-aligned.  The window
 *       is 4 GiB WIDE on Hopper: every GPU VA in [base, base + 4 GiB)
 *       is intercepted as SMEM by the SM front-end and never reaches
 *       the GMMU as a global store.  NVK uses 1<<32 because its Vulkan
 *       heap avoids that range, but mc's dma_ch carrier
 *       (NV50_MEMORY_VIRTUAL) lands at GPU VA ~0x120000000 and the
 *       va_pool at 0x200000000 — both inside a 4 GiB window starting
 *       at 1<<32.  16 GiB is well above any mc VA.
 *   - SET_SHADER_LOCAL_MEMORY_WINDOW_A/B = 0xffULL << 24 (NVK constant)
 */
uint32_t *mc_compute_write_setup(const mc_arch_desc_t *arch, uint32_t *pb)
{
  uint64_t smem_window = 0x400000000ULL;     /* 16 GiB; above all mc VAs */
  uint64_t lmem_window = 0xffULL << 24;      /* NVK constant */

  *pb++ = INCR_HEADER_COMPUTE(NVC86F_SET_OBJECT, 1);
  *pb++ = arch->cls.compute;

  /* INVALIDATE_SKED_CACHES (no payload bits matter — V=0 means
   * "invalidate now"; NVK passes 0). */
  *pb++ = INCR_HEADER_COMPUTE(NVB1C0_INVALIDATE_SKED_CACHES, 1);
  *pb++ = 0;

  *pb++ = INCR_HEADER_COMPUTE(NVC7C0_INVALIDATE_SAMPLER_CACHE_NO_WFI, 1);
  *pb++ = NVC7C0_INVALIDATE_SAMPLER_CACHE_NO_WFI_LINES_ALL;

  *pb++ = INCR_HEADER_COMPUTE(NVC7C0_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 1);
  *pb++ = NVC7C0_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI_LINES_ALL;

  *pb++ = INCR_HEADER_COMPUTE(NVC7C0_SET_SHADER_SHARED_MEMORY_WINDOW_A, 2);
  *pb++ = (NvU32)(smem_window >> 32);
  *pb++ = (NvU32)(smem_window & 0xffffffffu);

  *pb++ = INCR_HEADER_COMPUTE(NVC7C0_SET_SHADER_LOCAL_MEMORY_WINDOW_A, 2);
  *pb++ = (NvU32)(lmem_window >> 32);
  *pb++ = (NvU32)(lmem_window & 0xffffffffu);

  return pb;
}

/* Submit one QMD via SEND_PCAS_A.  The setup methods above have already been
 * emitted on the channel (gated by compute_ch.setup_done), so this builder
 * does not repeat SET_OBJECT.  NVK's launch path also does not re-emit it
 * per-dispatch (vulkan/nvk_cmd_dispatch.c:328-340 / 387-401).
 *
 * See the header for why `invalidate_caches` is a caller decision rather than
 * something this builder does unconditionally.
 */
uint32_t *mc_compute_write_launch(const mc_arch_desc_t *arch, uint32_t *pb,
                                  uint64_t qmd_gpu_va,
                                  uint64_t sema_va,
                                  uint32_t sema_payload,
                                  bool     invalidate_caches)
{
  if (invalidate_caches)
  {
    /* The SM front-end's constant cache holds prior CB0 reads keyed by GPU
     * VA.  Reusing one CB0 GPU VA across launches and patching its contents
     * per-call returns stale bytes unless explicitly invalidated.
     *
     * NO_WFI variant: don't stall the engine waiting for inflight compute to
     * drain.  We're at the start of a new launch; nothing is inflight on this
     * channel because the prior launch already fired its release sema before
     * the caller polled it. */
    *pb++ = INCR_HEADER_COMPUTE(NVC7C0_INVALIDATE_SHADER_CACHES_NO_WFI, 1);
    *pb++ = DRF_DEF(C7C0, _INVALIDATE_SHADER_CACHES_NO_WFI, _CONSTANT,    _TRUE)
          | DRF_DEF(C7C0, _INVALIDATE_SHADER_CACHES_NO_WFI, _GLOBAL_DATA, _TRUE);
  }

  *pb++ = INCR_HEADER_COMPUTE(NVC7C0_SEND_PCAS_A, 1);
  *pb++ = (NvU32)(qmd_gpu_va >> 8);

  *pb++ = INCR_HEADER_COMPUTE(NVC7C0_SEND_SIGNALING_PCAS2_B, 1);
  *pb++ = 0x0000000a;                /* SCHEDULE | <bit 3>; verified working
                                      * on H100.  NVK uses
                                      * PCAS_ACTION_INVALIDATE_COPY_SCHEDULE
                                      * (= 0x3) which also works on this
                                      * driver. */

  /* Release semaphore after the kernel completes.  4-method incr write;
   * A=offset_upper, B=offset_lower, C=payload, D=control word. */
  *pb++ = INCR_HEADER_COMPUTE(NVC7C0_SET_REPORT_SEMAPHORE_A, 4);
  *pb++ = DRF_NUM(C7C0, _SET_REPORT_SEMAPHORE_A, _OFFSET_UPPER,
                  (NvU32)(sema_va >> 32));
  *pb++ = (NvU32)sema_va;
  *pb++ = sema_payload;
  *pb++ = DRF_DEF(C7C0, _SET_REPORT_SEMAPHORE_D, _OPERATION, _RELEASE)
        | DRF_DEF(C7C0, _SET_REPORT_SEMAPHORE_D, _STRUCTURE_SIZE, _ONE_WORD)
        | DRF_DEF(C7C0, _SET_REPORT_SEMAPHORE_D, _FLUSH_DISABLE, _FALSE);

  return pb;
}
