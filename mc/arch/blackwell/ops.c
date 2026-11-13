/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * arch/blackwell/ops.c — the Blackwell behaviour hooks.
 *
 * Blackwell needs its own ops table for one reason that is not a matter of
 * taste: BLACKWELL_CHANNEL_GPFIFO_A's USERD has no GPGet.  Nvc96fControl
 * publishes GPPut at 0x8c and stops there, where Nvc86fControl and
 * Nvc56fControl both publish GPGet at 0x88.  A backend cannot express "this
 * field does not exist" by substituting a number, which is exactly the test
 * hal/arch.h sets for what becomes a hook.
 *
 * This file is the only place in the library that names Blackwell's USERD
 * struct, for the same reason arch/hopper/ops.c is for Hopper's: the layout
 * belongs to the channel class, not to the shared submission code.
 *
 * Why this is not a copy of arch/hopper/ops.c with a different typedef
 * -------------------------------------------------------------------
 * The ARCH_SHARES_* mechanism in the Makefile exists so a backend can reuse
 * another's implementation by class identity -- arch/ada and
 * arch/ampere_ga10x both reuse Ampere's ops because they allocate literally
 * AMPERE_USERMODE_A and AMPERE_CHANNEL_GPFIFO_A.  Blackwell allocates
 * neither Hopper class, and its control struct genuinely differs, so reuse
 * here would be resemblance rather than identity.  The USERMODE allocation
 * is the one piece that really is shared, and it is shared honestly: both
 * classes take NV_HOPPER_USERMODE_A_PARAMS, the same struct, per
 * resource_list.h.
 */

#include <stdbool.h>
#include <stdint.h>

#include "nvos.h"
#include "class/cl2080_notification.h"  /* NV2080_ENGINE_TYPE_GR0 */
#include "class/clc96f.h"               /* BlackwellAControlGPFifo */

#include "hal/arch.h"
#include "internal/rm.h"
#include "internal/types.h"

/* ── USERMODE allocation ──────────────────────────────────────────────────
 *
 * BLACKWELL_USERMODE_A is RS_OPTIONAL(NV_HOPPER_USERMODE_A_PARAMS)
 * (resource_list.h:960-964) -- the same optional parameter struct
 * HOPPER_USERMODE_A takes at :950-954, so the BAR0-vs-BAR1 choice is carried
 * the same way and this is Hopper's function with a different class id
 * supplied by the descriptor.
 */
static NvHandle blackwell_alloc_usermode(int ctl_fd, const mc_arch_desc_t *arch,
                                         NvHandle root, NvHandle subdevice,
                                         bool bar1)
{
    NV_HOPPER_USERMODE_A_PARAMS params = {
        .bBar1Mapping = bar1,
        .bPriv        = NV_FALSE,
    };
    return rm_alloc(ctl_fd, root, subdevice, arch->cls.usermode, &params);
}

/* ── USERD access ─────────────────────────────────────────────────────────
 *
 * Nvc96fControl is GPPut-only.  BLACKWELL_CHANNEL_GPFIFO_B's Nvca6fControl
 * agrees field for field, so this code is correct for either class should the
 * descriptor ever select _B.
 */
#define USERD(p) ((volatile BlackwellAControlGPFifo *)(p))

static void blackwell_userd_set_gpput(void *userd, uint32_t value)
{
    USERD(userd)->GPPut = value;
}

static uint32_t blackwell_userd_get_gpput(const void *userd)
{
    return USERD(userd)->GPPut;
}

/* Always false: the class publishes no GPGet, so there is no value to read
 * and no address at which to read one.
 *
 * Returning false rather than a plausible zero is the whole point of the
 * try_ form.  hal/arch.h states it as a rule -- "a caller that treats a false
 * return as zero has no correct behaviour on Blackwell" -- and the reason is
 * that zero is a *valid* GPGet meaning "the GPU has consumed nothing yet".
 * A caller that conflated the two would read an empty ring as a stalled one,
 * or a drained one as empty, depending on which way it tested.  Callers must
 * fall back to the semaphore to observe progress.
 */
static bool blackwell_userd_try_get_gpget(const void *userd,
                                          uint32_t *out_gpget)
{
    (void)userd;
    (void)out_gpget;
    return false;
}

/* ── Engine policy ────────────────────────────────────────────────────────
 *
 * The same policy as Hopper: compute on GR0, every copy role sharing the one
 * non-GRCE LCE that copy-engine selection picked.  Stated here rather than
 * shared because it is policy, not a property of the silicon -- hal/arch.h
 * makes that the reason this is a hook at all.
 *
 * The compute arm is unreachable in this build (the descriptor's compute
 * pointer is NULL, so no compute channel is ever created) and is written
 * anyway so that enabling compute later does not also require rediscovering
 * the engine mapping.
 */
static NvU32 blackwell_engine_for_role(const mc_arch_desc_t *arch,
                                       mc_arch_role_t role, NvU32 selected_lce)
{
    (void)arch;
    if ((mc_channel_role_t)role == MC_ROLE_COMPUTE)
        return NV2080_ENGINE_TYPE_GR0;
    return selected_lce;
}

const mc_arch_ops_t mc_arch_blackwell_ops = {
    .alloc_usermode      = blackwell_alloc_usermode,
    .userd_set_gpput     = blackwell_userd_set_gpput,
    .userd_get_gpput     = blackwell_userd_get_gpput,
    .userd_try_get_gpget = blackwell_userd_try_get_gpget,
    .engine_for_role     = blackwell_engine_for_role,
};
