/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * experimental/compute.c — demo-specific compute-kernel launch path.
 *
 * Hand-rolled SM-thread launch of mc_doorbell_kernel
 * (`*dst = token`) plus the chain-of-evidence variant that uses it
 * to ring primary's BAR1 doorbell.  These are bound tightly to one
 * specific embedded SASS (1×1×1 grid, hard-coded register count,
 * fixed CB0 layout) — they live here rather than in core/core.c so
 * the channel-management library stays general and the kernel-launch
 * code can grow independently.
 *
 * Library-internal: includes the internal headers.  Compiled into
 * libmc.so alongside the other library TUs (core/core.c, driver/rm.c,
 * driver/uvm.c, hal/submit.c, core/vaspace.c, arch/hopper/qmd_v04.c); not
 * exposed as a separate public surface.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "nvtypes.h"
#include "nvmisc.h"
#include "class/clc86f.h"        /* NVC86F_SET_OBJECT */
#include "class/cla06fsubch.h"   /* NVA06F_SUBCHANNEL_COMPUTE */

#include <mc/mc_experimental.h>

#include "internal/types.h"
#include "internal/log.h"
#include "internal/rm.h"
#include "internal/submit.h"
#include "internal/vaspace.h"
#include "internal/alloc.h"
#include "internal/compute.h"
#include "hal/methods.h"
#include "arch/hopper/qmd_v04.h"


/* mc_compute_doorbell_kernel — submit the embedded mc_doorbell_kernel
 * SASS on the compute channel.  See mc.h for the full contract. */
mc_status_t mc_compute_doorbell_kernel(mc_ctx_t *ctx, uint64_t dst_gpu_va,
                                       uint32_t token)
{
  mc_channel_t             *ch;
  struct mc_compute_extras *ex;
  struct mc_compute_module *mod;
  uint32_t                 *pb;
  uint32_t                 *pb_end;
  uint32_t                  copy_bytes;

  if (ctx == NULL) return MC_EINVAL;
  ch = mc_vas_find_channel(&ctx->vas[MC_VAS_SYSMEM_CARRIER], MC_ROLE_COMPUTE);
  if (ch == NULL || !ch->h_channel) return MC_EINTERNAL;

  ex  = &ch->x.compute;
  mod = &ex->module;

  /* Build the QMD from a zero-initialized buffer NVK-style: only
   * the fields `nak::qmd::Qmd4_0::fill_qmd` sets.  See
   * arch/hopper/qmd_v04.h for the setter list.  Single-thread (1,1,1)
   * grid; 8 GPRs (SASS uses up to R5); no SLM/SMEM/barriers.  CB0
   * binds the kernel argument tuple. */
  ch->arch->compute->build_qmd(mod->qmd_cpu,
                               mod->sass_gpu_va, /*register_count=*/8,
                               mod->cb0_gpu_va,
                               mc_cb0_alloc_bytes(ch->arch->compute));

  /* CB0: zero-init, then write only the kernel-argument dwords the SASS
   * actually reads.  Where those sit is per-architecture (sm_90 puts them
   * at 0x210, sm_80 at 0x160), so the offsets come from the compute ops. */
  mc_cb0_init(ch->arch->compute, mod->cb0_cpu);
  mc_cb0_set_args(ch->arch->compute, mod->cb0_cpu, dst_gpu_va, token);

  if (mc_debug())
  {
    /* Dump the patched QMD's pointer-bearing fields.  Field offsets
     * derived from clcbc0qmd.h V04 MW(hi:lo) ranges:
     *   PROGRAM_ADDRESS_LOWER  MW(1247:1216) → byte 0x98 (dword 38)
     *   PROGRAM_ADDRESS_UPPER  MW(1272:1248) → byte 0x9c (dword 39)
     *   CB0_VALID(0)           bit 416        → byte 0x34 bit 0
     *   CB0_ADDR_LOWER_S6(0)   MW(1567:1536) → byte 0xc0 (dword 48)
     *   CB0_ADDR_UPPER+SIZE(0) MW(1599:1568) → byte 0xc4 (dword 49) */
    uint32_t *q = (uint32_t *)mod->qmd_cpu;
    fprintf(stderr,
            "[mc] DEBUG: mc_compute_doorbell:"
            " qmd_va=0x%llx sass_va=0x%llx cb0_va=0x%llx"
            " dst_va=0x%llx token=0x%x\n"
            "[mc] DEBUG: qmd patched fields (NVK style):\n"
            "  off=0x034 (CB_VALID): 0x%08x\n"
            "  off=0x098 (PROG_LO):  0x%08x\n"
            "  off=0x09c (PROG_HI):  0x%08x\n"
            "  off=0x0c0 (CB0_LO):   0x%08x\n"
            "  off=0x0c4 (CB0_UP+SZ):0x%08x\n",
            (unsigned long long)mod->qmd_gpu_va,
            (unsigned long long)mod->sass_gpu_va,
            (unsigned long long)mod->cb0_gpu_va,
            (unsigned long long)dst_gpu_va, token,
            q[0x034/4], q[0x098/4], q[0x09c/4],
            q[0x0c0/4], q[0x0c4/4]);
  }

  /* Bump per-channel sema_payload (skip 0). */
  ch->sema_payload++;
  if (ch->sema_payload == 0) ch->sema_payload = 1;

  /* Build the pushbuffer.  First call: setup methods + launch.
   * Subsequent calls: launch only. */
  pb = ch->pb_cpu;
  if (!ex->setup_done)
  {
    pb = mc_compute_write_setup(ch->arch, pb);
    ex->setup_done = true;
  }
  /* invalidate_caches = false: every caller of this path passes identical
   * CB0 arguments on every launch (dst_va is the constant doorbell address
   * and token is the constant work-submit token), so the SM's constant cache
   * returning a prior launch's bytes is harmless here. */
  pb_end = mc_compute_write_launch(ch->arch, pb, mod->qmd_gpu_va,
                                   ch->sema_gpu_va,
                                   ch->sema_payload,
                                   false);
  copy_bytes = (uint32_t)((pb_end - ch->pb_cpu) * sizeof(uint32_t));

  return mc_channel_submit(ch, ctx->vf_doorbell, copy_bytes);
}

mc_status_t mc_compute_get_scratch(mc_ctx_t *ctx, volatile uint32_t **cpu_ptr,
                                   uint64_t *gpu_va)
{
  /* Trivial getter over the dedicated host-visible scratch dword that
   * mc_compute_module_init allocates and maps into the carrier VAS.
   * The dword has its own RM hMemory and GPU MMU PTE; the carrier
   * VAS owns both for the lifetime of the compute channel. */
  const struct mc_compute_module *mod;
  mc_channel_t *cmp;

  if (ctx == NULL || cpu_ptr == NULL || gpu_va == NULL) return MC_EINVAL;

  cmp = mc_vas_find_channel(&ctx->vas[MC_VAS_SYSMEM_CARRIER], MC_ROLE_COMPUTE);
  if (cmp == NULL || !cmp->h_channel) return MC_EINTERNAL;
  mod = &cmp->x.compute.module;

  if (mod->scratch_cpu == NULL || mod->scratch_gpu_va == 0)
    return MC_EINTERNAL;

  *cpu_ptr = mod->scratch_cpu;
  *gpu_va  = mod->scratch_gpu_va;
  return MC_OK;
}

/* ── SM-thread-rung copy ───────────────────────────────────────────
 * Same chain-of-evidence as mc_memcpy_gpu_doorbell_ce, but the
 * GPU-issued BAR1 doorbell write is performed by an SM thread of
 * mc_doorbell_kernel rather than by the DMA channel's CE LAUNCH_DMA.
 * No copy engine is involved in the doorbell ring — just a single
 * warp's STG.E.STRONG.SYS to a GPU-mapped BAR1 page.
 *
 * Direction is inferred from which user pointer was returned by
 * mc_malloc_host vs mc_malloc_device.  Both must live in MC_VAS_UVM
 * (the chain primary is the UVM CE channel).
 *
 * Both timeouts (compute sema then UVM-channel sema) are charged
 * against the same wall-clock budget so a wedged UVM channel can't
 * hide behind the compute kernel having succeeded.
 */
mc_status_t mc_memcpy_gpu_doorbell_sm(mc_ctx_t *ctx, void *dst,
                                      const void *src, size_t n)
{
  const mc_alloc_t     *src_slot;
  const mc_alloc_t     *dst_slot;
  mc_channel_t         *prim;
  mc_channel_t         *dma;
  mc_channel_t         *cmp;
  NvU64                 dbell_gpu_va;
  uint32_t             *prim_pb_end;
  uint32_t              prim_copy_bytes;
  struct timespec       t0;
  mc_status_t           rc;

  if (ctx == NULL || dst == NULL || src == NULL) return MC_EINVAL;
  if (n == 0 || n > MC_MAX_TRANSFER_SIZE)        return MC_EINVAL;

  /* Same alloc-table validation as mc_memcpy_gpu_doorbell_ce / mc_memcpy:
   * both pointers must come from mc_malloc_* on this ctx, share a VAS,
   * and the chain primary is the UVM channel — so MC_VAS_UVM only. */
  src_slot = alloc_table_lookup(ctx, src);
  dst_slot = alloc_table_lookup(ctx, dst);
  if (src_slot == NULL || dst_slot == NULL)
  {
    WARN_LOG("mc_memcpy_gpu_doorbell_sm: src=%p dst=%p — at least one "
             "pointer not in alloc table; both must come from mc_malloc_* "
             "on this ctx", src, dst);
    return MC_EINVAL;
  }
  if (src_slot->vas != MC_VAS_UVM || dst_slot->vas != MC_VAS_UVM)
  {
    WARN_LOG("mc_memcpy_gpu_doorbell_sm: src VAS=%d dst VAS=%d; both "
             "buffers must live in MC_VAS_UVM (the chain primary is the "
             "UVM CE channel)", (int)src_slot->vas, (int)dst_slot->vas);
    return MC_EINVAL;
  }
  if (src_slot->size < n || dst_slot->size < n)
  {
    WARN_LOG("mc_memcpy_gpu_doorbell_sm: transfer size %zu exceeds buffer "
             "(src=%llu, dst=%llu)", n,
             (unsigned long long)src_slot->size,
             (unsigned long long)dst_slot->size);
    return MC_EINVAL;
  }

  dbell_gpu_va = ctx->vas[MC_VAS_SYSMEM_CARRIER].dbell_gpu_va;

  dma = mc_vas_find_channel(&ctx->vas[MC_VAS_SYSMEM_CARRIER], MC_ROLE_HOST_DMA);
  cmp = mc_vas_find_channel(&ctx->vas[MC_VAS_SYSMEM_CARRIER], MC_ROLE_COMPUTE);
  if (dma == NULL || !dma->h_channel || !dbell_gpu_va)
    return MC_EINTERNAL;
  if (cmp == NULL || !cmp->h_channel)
    return MC_EINTERNAL;

  prim = mc_vas_find_channel(&ctx->vas[MC_VAS_UVM], MC_ROLE_UVM_CE);
  if (prim == NULL) return MC_EINTERNAL;

  /* Arm primary's submission: build pushbuffer + write GPFIFO entry +
   * advance USERD GPPut, but DO NOT ring primary's vf_doorbell.  Same
   * shape as mc_memcpy_gpu_doorbell_ce, including direction-agnostic
   * src/dst handoff to mc_write_transfer_methods. */
  prim->sema_payload++;
  if (prim->sema_payload == 0) prim->sema_payload = 1;

  prim_pb_end = mc_write_transfer_methods(
      ctx->arch,
      prim->pb_cpu,
      src_slot->gpu_va, dst_slot->gpu_va,
      (uint32_t)n, prim->sema_gpu_va, prim->sema_payload);
  prim_copy_bytes = (uint32_t)((prim_pb_end - prim->pb_cpu) * sizeof(uint32_t));

  rc = mc_channel_arm(prim, prim_copy_bytes, &t0);
  if (rc != MC_OK) return rc;

  /* Launch the compute kernel.  Its STG.E.STRONG.SYS writes the UVM
   * channel's work_submit_token to (carrier-VAS dbell_gpu_va + 0x90)
   * — the exact MMIO address the host would write to ring the UVM
   * channel.
   *
   * mc_compute_doorbell_kernel returns only after the kernel's
   * release semaphore fires, which (per SET_REPORT_SEMAPHORE_D
   * _FLUSH_DISABLE = _FALSE) waits for L2 flush — therefore the
   * doorbell write has physically reached BAR1 by the time we
   * return. */
  rc = mc_compute_doorbell_kernel(ctx,
                                  dbell_gpu_va + MC_VF_DOORBELL_OFFSET,
                                  prim->work_submit_token);
  if (rc != MC_OK) return rc;

  /* Poll primary sema with the t0 captured before arm so the
   * end-to-end budget can't hide a wedged primary behind a
   * successful compute launch. */
  return mc_channel_poll_sema(prim->sema_ptr, prim->sema_payload, t0);
}
