# RADV shared-load benchmark

Compare RADV's original 64-bit shared-load limit with a driver that allows aligned 96-bit and 128-bit LDS loads. Each A/B comparison uses the same SPIR-V, input data and addresses.

## Files

- `shared_load.comp`: dependent-load chains and throughput shaders.
- `lds_vulkan.cpp`: Vulkan execution, required subgroup size, GPU timestamps and CPU reference checks.
- `compare.py`: build shaders, inspect ISA, check correctness and compare two RADV libraries.

## Requirements

Linux, an AMD GPU supporting the requested Wave32/Wave64 subgroup sizes, Python 3, a C++17 compiler, `pkg-config`, Vulkan headers/loader, `glslangValidator`, `spirv-val` and `VK_LAYER_KHRONOS_validation`.

Prepare two matching release builds of `libvulkan_radeon.so`:

- **Baseline:** the original 64-bit shared-load limit.
- **Patched:** both 96-bit and 128-bit shared loads enabled.

The recorded runs used Mesa `582bd880ff140bc38563eb78db6766ef3a02b7b6`, GCC 14.2.0 and glslang 16.2.0. The [Mesa patch branch](https://gitlab.freedesktop.org/phg000210/mesa/-/tree/nir/wide-ldsload) contains the corresponding shared-load change. Build the parent and patched versions with the same options.

## Run

Replace the two library paths with your own builds:

```sh
python3 compare.py all --bits 96 \
  --baseline /absolute/path/to/baseline/libvulkan_radeon.so \
  --patched /absolute/path/to/patched/libvulkan_radeon.so

python3 compare.py all --bits 128 \
  --baseline /absolute/path/to/baseline/libvulkan_radeon.so \
  --patched /absolute/path/to/patched/libvulkan_radeon.so
```

The script creates its own ICD manifests in `results/96/` and `results/128/`. It selects the supplied libraries per process and does not replace the system driver.

The default GPU is RX 9070 XT (`0x7550`). For Radeon 8060S, set `LDS_DEVICE_ID=0x1586`. For another supported AMD GPU, set its PCI device ID. If your Mesa builds use a private libdrm, set `LDS_LIB_DIR` to that library directory.

Individual phases are `build`, `isa`, `correctness` and `performance`. Only `build` can run without the two driver paths. `all` runs them in that order. Vulkan/ACO validation is enabled for ISA and correctness runs and disabled for performance runs.

## What is measured

Records contain three or four `uint32` components: 12 or 16 bytes. Wide-load cases start at 16-byte-aligned addresses. The negative controls use 64-bit loads or 8-byte-aligned addresses.

The dependent chain derives the next address from every loaded component. Throughput accumulates eight independent records per iteration. Both paths check all final output words against a CPU reference.

Performance uses 512 workgroups, 128 threads per workgroup and 1,024 shader-loop iterations. Each process warms up for at least 500 ms, then measures 31 identical GPU dispatches and reports their median time. Each condition runs five A/B pairs in AB, BA, AB, BA, AB order. Wave32 and Wave64 are selected explicitly. Record strides are 16, 32 and 128 bytes.

The GPU timestamps include shared-memory initialization, SSBO accesses, address arithmetic, accumulation and loop overhead. The dependent-chain result is not isolated instruction latency. These results do not establish application-level gains or performance on untested GPUs.

`results/<bits>/` contains ISA dumps and hashes, CPU-reference results, per-process timing records and `performance.md` with median times and run ranges. The percentage in that generated report is the median of the five paired time reductions.

## Recorded throughput results

The following tables use the default 16-byte stride. Speedup is the ratio of the median split-load dispatch time to the median native-load dispatch time, using five process medians per driver. The two record widths are separate workloads. Each row compares the same logical bytes on both sides.

### Wave32

| GPU | Data per record | Load comparison | Speedup |
|---|---:|---|---:|
| RX 9070 XT | 12 bytes | `b64+b32` → `b96` | 1.540× |
| RX 9070 XT | 16 bytes | `b64+b64` → `b128` | 1.215× |
| Strix Halo (Radeon 8060S) | 12 bytes | `b64+b32` → `b96` | 1.814× |
| Strix Halo (Radeon 8060S) | 16 bytes | `b64+b64` → `b128` | 1.325× |

### Wave64

| GPU | Data per record | Load comparison | Speedup |
|---|---:|---|---:|
| RX 9070 XT | 12 bytes | `b64+b32` → `b96` | 1.534× |
| RX 9070 XT | 16 bytes | `b64+b64` → `b128` | 1.211× |
| Strix Halo (Radeon 8060S) | 12 bytes | `b64+b32` → `b96` | 1.913× |
| Strix Halo (Radeon 8060S) | 16 bytes | `b64+b64` → `b128` | 1.318× |

The historical timing runs used the equivalent pre-simplification Mesa patch. The final size-limit simplification was checked separately for identical RX 9070 XT shader ISA and outputs. The shader and Vulkan host sources here are unchanged from those measurements; the Python wrapper was simplified for portable driver-path arguments.
