/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * mc_arch_detect_test — architecture detection and backend selection.
 *
 * Two halves.  The registry lookup is a pure function of the backend table,
 * so it needs no mocking at all.  mc_detect_arch talks to the driver, so its
 * two control calls are interposed with --wrap=ioctl.
 *
 * The case worth the most here is the implementation-namespacing one: GA100,
 * GH100, AD100, GB100 and GB200 all report implementation 0, so a lookup that
 * considered implementation before architecture would hand an H100 to
 * whichever backend happened to come first.  That is a silent
 * wrong-class-id bug -- an Xid on hardware, not a test failure -- so it is
 * pinned here where no GPU is required to catch it.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#include "nvos.h"
#include "nv-ioctl-numbers.h"
#include "nv_escape.h"
#include "ctrl/ctrl2080/ctrl2080mc.h"
#include "ctrl/ctrl0080/ctrl0080gpu.h"

#include "hal/arch.h"
#include "hal/detect.h"
#include "hal/registry.h"
#include "internal/log.h"

static int failures;

#define EXPECT(cond)                                                  \
  do {                                                                \
    if (!(cond)) {                                                    \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                     \
    }                                                                 \
  } while (0)

/* ── Mock driver state ────────────────────────────────────────────────────*/
static NvU32 mock_architecture;
static NvU32 mock_implementation;
static NvU32 mock_classes[NV0080_CTRL_GPU_CLASSLIST_MAX_SIZE];
static NvU32 mock_class_count;
static int   mock_arch_info_fails;

int __wrap_ioctl(int fd, unsigned long request, ...)
{
  NVOS54_PARAMETERS *p;
  va_list            args;

  (void)fd;
  if (_IOC_NR(request) != (NV_ESC_RM_CONTROL & 0xff)) return -1;

  va_start(args, request);
  p = va_arg(args, NVOS54_PARAMETERS *);
  va_end(args);
  if (p == NULL) return -1;

  if (p->cmd == NV2080_CTRL_CMD_MC_GET_ARCH_INFO)
  {
    NV2080_CTRL_MC_GET_ARCH_INFO_PARAMS out = {};
    if (mock_arch_info_fails) { p->status = NV_ERR_NOT_SUPPORTED; return 0; }
    out.architecture   = mock_architecture;
    out.implementation = mock_implementation;
    memcpy((void *)(uintptr_t)p->params, &out, sizeof(out));
    p->status = NV_OK;
    return 0;
  }

  if (p->cmd == NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2)
  {
    NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS out = {};
    out.numClasses = mock_class_count;
    memcpy(out.classList, mock_classes,
           (size_t)mock_class_count * sizeof(NvU32));
    memcpy((void *)(uintptr_t)p->params, &out, sizeof(out));
    p->status = NV_OK;
    return 0;
  }

  return -1;
}

/* Populate the mock class list with everything the given backend needs. */
static void mock_classes_for(const mc_arch_desc_t *a)
{
  mock_class_count = 0;
  mock_classes[mock_class_count++] = a->cls.usermode;
  mock_classes[mock_class_count++] = a->cls.channel_gpfifo;
  mock_classes[mock_class_count++] = a->cls.dma_copy;
  mock_classes[mock_class_count++] = a->cls.compute;
  mock_classes[mock_class_count++] = a->cls.channel_group;
  mock_classes[mock_class_count++] = a->cls.vaspace;
  mock_classes[mock_class_count++] = a->cls.virtual_memory;
}

static void reset(void)
{
  mock_architecture    = mc_arch_hopper.rm_architecture;
  mock_implementation  = 0;
  mock_arch_info_fails = 0;
  mock_classes_for(&mc_arch_hopper);
}

/* ── Registry lookup (no driver involved) ─────────────────────────────────*/

static void test_lookup_matches_architecture(void)
{
  const mc_arch_desc_t *a =
      mc_arch_registry_lookup(mc_arch_hopper.rm_architecture, 0);
  EXPECT(a == &mc_arch_hopper);
  printf("PASS: registry lookup resolves a known architecture\n");
}

static void test_lookup_rejects_unknown_architecture(void)
{
  /* 0x160 is TU100 (Turing) -- a real architecture with no backend here. */
  EXPECT(mc_arch_registry_lookup(
             NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_TU100, 0) == NULL);
  /* And something that is not an architecture at all. */
  EXPECT(mc_arch_registry_lookup(0xdead, 0) == NULL);
  printf("PASS: registry lookup rejects unknown architectures\n");
}

/* The regression this file exists for. */
static void test_lookup_does_not_confuse_implementations(void)
{
  size_t                       i, n = 0;
  const mc_arch_desc_t *const *all = mc_arch_registry(&n);

  /* Every other architecture that also reports implementation 0 must NOT
   * resolve to a backend registered for a different architecture. */
  static const NvU32 other_archs[] = {
    NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GA100,
    NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_AD100,
    NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GB100,
    NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GB200,
  };
  size_t k;

  for (k = 0; k < sizeof(other_archs) / sizeof(other_archs[0]); k++)
  {
    const mc_arch_desc_t *got = mc_arch_registry_lookup(other_archs[k], 0);
    if (got != NULL)
      EXPECT(got->rm_architecture == other_archs[k]);
  }

  /* Every backend is reachable, and no backend may claim an architecture it
   * was not registered for.
   *
   * The implementation to look up has to come from the backend's own mask
   * rather than being 0.  Implementation 0 is only reachable when some
   * backend of that architecture claims it, and that is not guaranteed: the
   * Blackwell backend claims GB206 (implementation 6) and nothing claims
   * GB200 (implementation 0), so a hardcoded 0 would assert that a GB200
   * part resolves -- a chip this build deliberately does not support.
   * Asking each backend for an implementation it actually claims keeps the
   * assertion at full strength and tests something true. */
  for (i = 0; i < n; i++)
  {
    const mc_arch_desc_t *got;
    NvU32 impl = 0;

    /* Lowest implementation in the mask; 0 when the mask means "any". */
    if (all[i]->rm_impl_mask != 0)
      while ((all[i]->rm_impl_mask & (1u << impl)) == 0)
        impl++;

    got = mc_arch_registry_lookup(all[i]->rm_architecture, impl);
    EXPECT(got != NULL && got->rm_architecture == all[i]->rm_architecture);
  }
  printf("PASS: implementation 0 does not cross architectures\n");
}

static void test_lookup_by_name(void)
{
  EXPECT(mc_arch_registry_by_name("hopper") == &mc_arch_hopper);
  EXPECT(mc_arch_registry_by_name("nonesuch") == NULL);
  EXPECT(mc_arch_registry_by_name(NULL) == NULL);
  printf("PASS: registry lookup by name\n");
}

/* ── Detection (driver mocked) ────────────────────────────────────────────*/

static void test_detect_success(void)
{
  const mc_arch_desc_t *arch = NULL;
  reset();
  EXPECT(mc_detect_arch(3, 0xf0000000, 0xf0000001, 0xf0000002, &arch) == 0);
  EXPECT(arch == &mc_arch_hopper);
  printf("PASS: detection selects the matching backend\n");
}

static void test_detect_unknown_chip_fails(void)
{
  const mc_arch_desc_t *arch = (const mc_arch_desc_t *)0x1;
  reset();
  mock_architecture = NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_TU100;
  EXPECT(mc_detect_arch(3, 0xf0000000, 0xf0000001, 0xf0000002, &arch) != 0);
  /* Must not leave a stale descriptor behind for the caller to use. */
  EXPECT(arch == NULL);
  printf("PASS: detection fails closed on an unsupported architecture\n");
}

static void test_detect_missing_class_fails(void)
{
  const mc_arch_desc_t *arch = (const mc_arch_desc_t *)0x1;
  reset();
  /* Architecture matches, but the device does not expose the channel class --
   * the MIG / feature-reduced case.  Detection must catch it here rather than
   * letting the channel allocation fail later with no indication of why. */
  mock_classes[1] = 0;
  EXPECT(mc_detect_arch(3, 0xf0000000, 0xf0000001, 0xf0000002, &arch) != 0);
  EXPECT(arch == NULL);
  printf("PASS: detection rejects a device missing a required class\n");
}

static void test_detect_missing_compute_is_tolerated(void)
{
  const mc_arch_desc_t *arch = NULL;
  reset();
  /* The compute class is optional: without it, host-authored copies still
   * work and only the compute-dependent paths become unavailable. */
  mock_classes[3] = 0;
  EXPECT(mc_detect_arch(3, 0xf0000000, 0xf0000001, 0xf0000002, &arch) == 0);
  EXPECT(arch == &mc_arch_hopper);
  printf("PASS: a missing compute class does not fail detection\n");
}

static void test_detect_query_failure(void)
{
  const mc_arch_desc_t *arch = (const mc_arch_desc_t *)0x1;
  reset();
  mock_arch_info_fails = 1;
  EXPECT(mc_detect_arch(3, 0xf0000000, 0xf0000001, 0xf0000002, &arch) != 0);
  EXPECT(arch == NULL);
  printf("PASS: detection reports a failed architecture query\n");
}

int main(void)
{
  test_lookup_matches_architecture();
  test_lookup_rejects_unknown_architecture();
  test_lookup_does_not_confuse_implementations();
  test_lookup_by_name();
  test_detect_success();
  test_detect_unknown_chip_fails();
  test_detect_missing_class_fails();
  test_detect_missing_compute_is_tolerated();
  test_detect_query_failure();

  if (failures) {
    printf("%d detection check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
