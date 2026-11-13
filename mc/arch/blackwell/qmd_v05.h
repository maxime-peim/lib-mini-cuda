/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * arch/blackwell/qmd_v05.h — Blackwell QMD V05 builder.
 *
 * Built from zero, NVK-style, exactly as arch/hopper/qmd_v04.c is.  See Mesa
 * NVK `src/nouveau/compiler/nak/qmd.rs` (`Qmd5_0`, qmd.rs:807-839) for the
 * ground-truth recipe this mirrors.
 *
 * Provenance
 * ----------
 * CLAUDE.md makes QMD layouts the documented exception to "establish it from
 * the pinned headers": no QMD header past Kepler's cla0c0qmd.h ships in the
 * driver tree -- the 620.70.06 tree contains no file matching *qmd* at all --
 * so V03 and V04 follow Mesa NVK, and V05 does the same.
 *
 * Field positions here are the NVCEC0_QMDV05_00_* macros from Mesa's
 * `src/nouveau/headers/nvidia/classes/clcec0qmd.h`, which is the QMD header
 * for BLACKWELL_COMPUTE_B (0xCEC0) -- the exact class this backend
 * allocates, not a near relative.  That file and clcdc0qmd.h
 * (BLACKWELL_COMPUTE_A) are byte-identical apart from their include guard
 * and the NVCDC0/NVCEC0 prefix, which is why NVK can drive both from one
 * `Qmd5_0` and say so in a comment (qmd.rs:794-796: "if Hopper is v4 and
 * Blackwell_B is v5, then Blackwell_A also has to be v5").
 *
 * V05 is a different layout, not a field shuffle
 * ----------------------------------------------
 * Almost nothing sits where V04 put it, and three fields changed encoding:
 *
 *                              V04 (clcbc0qmd.h)   V05 (clcec0qmd.h)
 *   QMD_TYPE                   MW(25:23)           MW(153:151)
 *   QMD_MAJOR_VERSION          MW(583:580)         MW(471:468)
 *   PROGRAM_ADDRESS_LOWER      MW(1247:1216)       MW(1055:1024) SHIFTED4
 *   CTA_THREAD_DIMENSION2      MW(1199:1184)       MW(1127:1120)  <- 8 bits
 *   REGISTER_COUNT             MW(1208:1200)       MW(1136:1128)
 *   SHARED_MEMORY_SIZE         MW(601:584)         MW(1162:1152) SHIFTED7
 *   SHADER_LOCAL_MEMORY_LOW    MW(535:512)         MW(1199:1184) SHIFTED4
 *   GRID_WIDTH                 MW(1055:1024)       MW(1279:1248)
 *   CONSTANT_BUFFER_VALID(i)   MW(416+i*4)         MW(1856+i*4)
 *
 * Note GRID_WIDTH's V05 position is V04's PROGRAM_ADDRESS_LOWER and vice
 * versa, so a V04 builder pointed at a V05 QMD would write a grid dimension
 * where SKED reads the program address.  That is the failure this file
 * exists to avoid, and the reason the two builders share no field constants
 * -- only the generic bit-range writer, which is duplicated deliberately
 * rather than hoisted, so neither file can silently change the other.
 *
 * Two things V05 requires that V04 did not:
 *
 *   - QMD_GROUP_ID = 0x1f.  NVK sets it in `Qmd5_0::new` (qmd.rs:817) and
 *     sets nothing like it for V04.
 *   - PROGRAM_ADDRESS and the SLM/SMEM sizes carry shifts, so the values
 *     written are not the values passed.  Each shift is asserted exact
 *     before it is applied, mirroring NVK's `assert!((x << shift) == x)`.
 *
 * Size is unchanged: `Qmd5_0` is `[u32; 96]` (qmd.rs:807-809), 384 bytes,
 * the same as V04.
 *
 * Verified against hardware, not just against the headers: the golden dwords
 * in tests/unit/mc_qmd_test.c pin this builder's output, and the launch it
 * produces runs on a GB206.
 */
#ifndef MC_ARCH_BLACKWELL_QMD_V05_H
#define MC_ARCH_BLACKWELL_QMD_V05_H

#include <stdint.h>

/* Blackwell QMD V05 fixed size (96 dwords), per NVK's `Qmd5_0`. */
#define MC_QMD_V05_BYTES             384u

/* The field setters are private to qmd_v05.c: they are how a V05 QMD is
 * assembled, not what a caller asks for.  Callers go through the
 * architecture's compute ops (hal/arch.h), whose build_qmd covers the one
 * launch shape libmc uses.  mc_arch_blackwell_compute is that table.
 *
 * CB0 is built by the shared hal/cbuf.c, parameterised by the cb0_* offsets
 * in that table.  Those offsets are sm_120's and do NOT carry to other SM
 * versions -- see the note on them in qmd_v05.c. */

#endif /* MC_ARCH_BLACKWELL_QMD_V05_H */
