# DX11 GPU Timestamp Queries Unavailable on Windows on ARM (SVGAMPWDDM)

## Environment

| Component | Value |
|-----------|-------|
| Device | Huawei MateBook E (Qualcomm Snapdragon 850) |
| GPU | SVGAMPWDDM Device (Hardware), 2304 MB VRAM |
| Feature Level | D3D_FEATURE_LEVEL_11_1 |
| Driver Type | Hardware |
| OS | Windows 11 ARM64 |
| CPU | StratoVirt (reported by OS) |

## Symptom

DX11 timestamp query objects are created successfully — `ID3D11Device::CreateQuery` returns `S_OK` for both `D3D11_QUERY_TIMESTAMP` and `D3D11_QUERY_TIMESTAMP_DISJOINT`. The application reports:

```
[Profiling] DX11 timestamp queries enabled.
```

However, when reading back the disjoint query data via `ID3D11DeviceContext::GetData`, the call perpetually returns `S_FALSE` (`0x1`), indicating the result is never ready:

```
[Profiling] DX11 disjoint GetData hr=0x1 slot=1 frame=8
```

As a result, the benchmark summary prints:

```
(No timestamp data available for analysis.)
```

No per-frame compute/render/total GPU timing is recorded.

## Root Cause

The D3D11 specification allows drivers to **silently accept** timestamp query creation but **never produce valid results**. The `SVGAMPWDDM` driver (Qualcomm's generic ARM GPU driver for Windows) appears to fall into this category — it exposes Feature Level 11_1 and creates query objects without error, but the underlying GPU or driver does not implement the hardware counters needed to resolve timestamp queries.

This is consistent with the DX11 documentation:

> `ID3D11DeviceContext::GetData` returns `S_FALSE` if the data is not yet available. [...] If the query was created with `D3D11_QUERY_TIMESTAMP_DISJOINT`, the driver may return `S_FALSE` indefinitely if timestamp queries are not supported by the hardware.

## Impact

- **FPS measurement** is unaffected — frame timing is calculated on the CPU side via `glfwGetTime()` and remains accurate.
- **GPU-side profiling** (compute time, render time, total GPU time) is unavailable on this device under DX11.
- The benchmark summary still reports `Avg FPS` correctly but omits the GPU timing breakdown.

## Comparison with Other APIs on the Same Device

| API | Timestamp Support | Notes |
|-----|-------------------|-------|
| DX11 (Hardware GPU) | Query created OK, `GetData` returns `S_FALSE` forever | Driver accepts but never resolves |
| DX12 (WARP) | Hardware GPU does not support DX12; WARP provides timestamps | Software renderer, runs on CPU |
| Vulkan (WARP via Dozen) | Dozen translates to DX12 WARP | Software renderer, runs on CPU |

## Workaround

No application-level workaround exists — the driver simply does not produce timestamp data. The application already handles this gracefully by checking `GetData` return values and skipping GPU timing when unavailable.

For GPU profiling on this class of device, the only option is to use an external tool such as [GPUView](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpuview) or [PIX on Windows](https://devblogs.microsoft.com/pix/), though ARM64 support for these tools may be limited.

## Affected Devices

This issue is expected on any Windows on ARM device using the `SVGAMPWDDM` driver (generic Qualcomm/ARM GPU adapter) that lacks native DX12 or Vulkan support. Devices with Qualcomm Adreno drivers (e.g. Snapdragon X Elite with proper Adreno GPU driver) are not affected, as those drivers implement timestamp queries correctly.
