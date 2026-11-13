# libmc contributor guide

`libmc` is the standalone userspace library extracted from
[`libcuda-research`](https://github.com/maxime-peim/libcuda-research). This
repository owns the library, integration demos, unit tests, and its exact
driver-source dependency. The research repository owns the instrumented
driver, captures, trace tools, and protocol notes.

## Compatibility contract

- This revision targets NVIDIA driver 610.43.02. Supported architectures are
  Hopper, Ampere GA100 (`mc/arch/ampere`), Ampere GA10x
  (`mc/arch/ampere_ga10x`), Ada and Blackwell GB20x (`mc/arch/blackwell`),
  each verified on hardware. Note GA100 and GA10x report the same
  architecture id and are told apart by `rm_impl_mask`, so a new backend
  sharing an architecture must narrow both masks.
- The Blackwell backend carries two qualifications the others do not. It was
  verified on drivers 620.70.06 (GB206, RTX PRO 2000 Blackwell Laptop) and
  595.71.05 (GB202, RTX PRO 5000 Blackwell) rather than on the pinned
  610.43.02. It offers every feature, including `xfer_sm`, on the strength
  of a QMD V05 builder (`mc/arch/blackwell/qmd_v05.c`, from Mesa NVK under
  the QMD exception below) and sm_120 cubins for both kernels. Its
  `rm_impl_mask` names GB202 and GB206 — the parts actually run — and not
  the rest of GB20x; adding one is a bit and a hardware run. See the header
  of `mc/arch/blackwell/backend.c`.
- Blackwell allocates BLACKWELL_COMPUTE_B (0xCEC0) but is driven with the
  0xC7C0 method dialect, exactly as Hopper allocates 0xCBC0 and is driven
  with 0xC7C0. `class/clcec0.h` is a stub with no method addresses, so that
  rests on `hal/method_abi.h` asserting 0xCDC0 against 0xC7C0, on the same
  file asserting Ampere's _A against its _B, and on the hardware run. Do not
  quietly extend it to a class without at least that much.
- Not every architecture provides every feature, and that is a property of the
  silicon rather than a gap. Ampere and Ada lack a BAR1 usermode aperture, so
  no doorbell PTE, so no FB carrier and nothing SM-authored. Hopper and
  Blackwell offer everything. Query with `mc_feature_supported()`; never
  assume from the driver version.
- `third_party/open-gpu-kernel-modules` is pinned to the matching official
  source. Do not silently build against another driver revision: a different
  `NVIDIA_SRC` release is refused unless `MC_ALLOW_UNTESTED_DRIVER=1` is given,
  and then it warns.
- Private RM and UVM interfaces are version-specific. libmc compiles against
  the target release's own headers, so struct layouts follow the release. A
  source-level difference between releases (a field one release has and
  another lacks) is handled by a feature probe in `tools/nvidia_conftest.sh`
  and an `#if MC_NV_HAVE_*` at the point of use, never by comparing version
  numbers. Add one probe per difference; `make check-conftest` tests the
  generator.
- A new *verified* driver version still updates the pin, probes, tests, and
  release documentation together.

## Implementation invariants

- Use `NVOS64_PARAMETERS`, not the legacy NVOS21 form, for RM allocation.
- Build NVIDIA bit fields with the SDK `DRF_*` macros. Do not replace them with
  handwritten shifts and masks.
- Format `NvU64` values with `NvU64_fmtx`; use `ULL` for 64-bit constants.
- UVM-managed memory follows the UVM external-range path. Do not substitute a
  non-UVM `NV04_MAP_MEMORY_DMA` route.
- UVM host mappings obey the Paper-F1 invariant: CPU VA equals GPU VA. Preserve
  the anchored VA-pool design.
- `FERMI_VASPACE_A` used with UVM must be externally owned.
- Submission order is pushbuffer, GPFIFO entry, USERD GPPut, then doorbell.
  Preserve the existing fence discipline.
- The work-submit doorbell is at `+0x90` in the mapped USERMODE window. This is
  now `lay.vf_doorbell_offset` in each architecture descriptor, and
  `tests/unit/mc_arch_desc_test.c` asserts every backend agrees — state it per
  backend rather than reintroducing a shared constant.
- The CE channel deliberately selects a non-GRCE LCE and uses its COPY engine
  consistently for the TSG and channel. Do not reintroduce the historical RM
  engine-lookup patch as a prerequisite.
- A channel's USERD must satisfy the architecture's runlist-entry address
  width. `NV_RAMRL_ENTRY_CHAN_USERD_PTR_HI_HW` is 8 bits on GA100 against 20 on
  GH100/GB10B, so Ampere and Ada must place USERD in vidmem — a host physical
  address does not reliably fit under 2^40. That is `cap.userd_requires_vidmem`;
  `tests/unit/mc_userd_addr_test.c` checks each backend's flag against the
  header it comes from.
- A kernel under `mc/kernels/` must not name a class id as a constant. The
  copy class differs per architecture (`0xC8B5` on GH100, `0xCAB5` on
  GB20x), and a `SET_OBJECT` naming a class the device does not expose
  wedges the victim channel instead of refusing the method — which looks
  like a malformed submission, not a wrong class. Pass it in through
  `struct mc_sm_owner_args`, from `cls.dma_copy`.
- Changes to a kernel under `mc/kernels/` must retain the Makefile's
  generated-SASS assertions, which are per-architecture and cover every kernel,
  not just `sm_owner.cu`. Review the disassembly when store or fence code
  changes.
- Constant-bank offsets for kernel arguments belong to the compiled SASS, not
  to the QMD version: nvcc places them at `0x210` on sm_90 and `0x160` on
  sm_80/86/89. Read them from `cuobjdump --dump-sass` for the architecture's own
  cubin and record them in its compute ops. Never carry another
  architecture's over.
- Before adding an ioctl or changing a GPU command bit field, establish its
  value from the pinned driver headers or a checked-in research capture. QMD
  layouts are the documented exception: no QMD header past Kepler's
  `cla0c0qmd.h` ships in the pinned tree, so V03 and V04 follow Mesa NVK
  (`src/nouveau/compiler/nak/qmd.rs` plus the `clcXc0qmd.h` headers Mesa
  publishes). Cite the NVK construct you mirrored, and pin the result with
  golden bits in `tests/unit/mc_qmd_test.c` rather than trusting the transcription.

## Verification

Run the host build and unit gates with:

```sh
make libmc mc-all test-unit
```

The integration matrix (`sudo python3 tools/run_mc_tests.py`) runs on any
supported architecture with the matching loaded driver. Official 610.43.02
modules are sufficient. When using the instrumented `libcuda-research` modules
for an `MC_XFER_SM` run, load them with `nv_dbell_disable_intercept=1`.

The matrix asks the GPU what it can do — via `mc_features_demo --porcelain` —
and reports a cell whose feature is absent as `SKIP`, not `FAIL`. A GPU
exercising less of the matrix is expected; a *failure* is not. Two rules follow
for anyone adding to it:

- Give a new cell a `requires` tuple naming the `mc_feature_name()` values it
  needs. Without one it runs everywhere and fails on hardware that cannot
  possibly run it.
- If a test skips only part of its own work and still does real testing, do not
  let its output contain "unsupported" or "unavailable on this gpu". The
  harness's fallback heuristic matches those substrings anywhere in the output
  and discounts the whole cell. `mc_allocation_test` is the worked example.

Keep architecture claims linked to the evidence in `libcuda-research`; do not
present an experimental workaround as a current requirement after the code path
has changed. Say which silicon a measurement came from — H100 SXM5, A100 SXM4
and L4 are not interchangeable, and PCIe generation alone changes the ceiling.
