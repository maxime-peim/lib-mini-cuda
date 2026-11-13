/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * Prove that mc_init rejects a driver mismatch before touching the GPU device
 * or issuing any RM allocation ioctl.  All relevant syscalls are link-wrapped;
 * no NVIDIA driver or GPU is needed.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#include "nv-ioctl.h"
#include "nv-ioctl-numbers.h"

#include <mc/mc.h>

#define FAKE_CTL_FD 71

static int failures;
static int open_calls;
static int ioctl_calls;
static int close_calls;
static int unexpected_open_calls;
static int unexpected_ioctl_calls;

#define EXPECT(cond)                                                  \
  do                                                                  \
  {                                                                   \
    if (!(cond))                                                      \
    {                                                                 \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                     \
    }                                                                 \
  } while (0)

int __wrap_open(const char *path, int flags, ...)
{
  (void)flags;
  open_calls++;
  if (strcmp(path, "/dev/nvidiactl") == 0)
    return FAKE_CTL_FD;

  unexpected_open_calls++;
  errno = ENOENT;
  return -1;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
  nv_ioctl_rm_api_version_t *p;
  va_list                    args;

  ioctl_calls++;
  if (fd != FAKE_CTL_FD || _IOC_NR(request) != (NV_ESC_CHECK_VERSION_STR & 0xff)
      || _IOC_SIZE(request) != sizeof(*p))
  {
    unexpected_ioctl_calls++;
    errno = EIO;
    return -1;
  }

  va_start(args, request);
  p = va_arg(args, nv_ioctl_rm_api_version_t *);
  va_end(args);

  EXPECT(p->cmd == NV_RM_API_VERSION_CMD_STRICT);
  EXPECT(strcmp(p->versionString, MC_NVIDIA_DRIVER_VERSION) == 0);

  p->reply = NV_RM_API_VERSION_REPLY_RECOGNIZED;
  snprintf(p->versionString, sizeof(p->versionString), "%s", "001.01.01");
  errno = EINVAL;
  return -1;
}

int __wrap_close(int fd)
{
  close_calls++;
  EXPECT(fd == FAKE_CTL_FD);
  return 0;
}

int main(void)
{
  mc_ctx_t   *ctx = (mc_ctx_t *)1;
  mc_status_t status;

  status = mc_init(&ctx);

  EXPECT(status == MC_EVERSION);
  EXPECT(ctx == NULL);
  EXPECT(open_calls == 1);
  EXPECT(unexpected_open_calls == 0);
  EXPECT(ioctl_calls == 1);
  EXPECT(unexpected_ioctl_calls == 0);
  EXPECT(close_calls == 1);

  if (failures != 0)
  {
    fprintf(stderr, "%d init-order assertion(s) failed\n", failures);
    return 1;
  }

  puts("PASS: mc_init rejects mismatch before RM initialization");
  return 0;
}
