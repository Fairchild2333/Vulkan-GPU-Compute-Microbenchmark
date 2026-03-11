# DX11 Timestamp Queries Unavailable on Virtual GPU (Windows on ARM)

## Environment

| Component | Value |
|-----------|-------|
| Host CPU | Qualcomm Snapdragon 8cx Gen3 |
| Hypervisor | StratoVirt (Huawei virtualisation engine) |
| Virtual GPU | SVGAMFM53M Device, 2504 MB VRAM |
| Feature Level | D3D_FEATURE_LEVEL_11_1 |
| DX12 Support | Disabled (virtual GPU driver does not implement D3D12) |
| OS | Windows 11 ARM64 (Build 26100) |
| System Model | Virtual Machine (VirtualBox/StratoVirt) |

> **Note:** The `SVGA`-prefixed GPU name indicates a **virtual machine display adapter** (VMware/VirtualBox lineage), not a native Qualcomm Adreno GPU. The host device's physical GPU is not directly exposed to the VM.

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

The D3D11 specification allows drivers to **silently accept** timestamp query creation but **never produce valid results**. The virtual GPU driver (`SVGAMFM53M` / `SVGAMPWDDM`) falls into this category — it exposes Feature Level 11_1 and creates query objects without error, but does not implement the hardware performance counters required to resolve timestamp queries.

This is consistent with the D3D11 documentation:

> `ID3D11DeviceContext::GetData` returns `S_FALSE` if the data is not yet available. [...] If the query was created with `D3D11_QUERY_TIMESTAMP_DISJOINT`, the driver may return `S_FALSE` indefinitely if timestamp queries are not supported by the hardware.

**Key distinction:** On the same PC (AMD Ryzen 9800X3D), WARP running DX11 **does** produce valid timestamps. This confirms the issue is specific to the WoA VM's virtual GPU driver, not to software rendering in general.

## Impact

- **FPS measurement** is unaffected — frame timing is calculated on the CPU side via `glfwGetTime()` and remains accurate.
- **GPU-side profiling** (compute time, render time, total GPU time) is unavailable on this device under DX11.
- The benchmark summary still reports `Avg FPS` correctly but omits the GPU timing breakdown.

## Comparison with Other APIs on the Same Device

| API | Timestamp Support | Notes |
|-----|-------------------|-------|
| DX11 (Virtual GPU) | `CreateQuery` succeeds, `GetData` returns `S_FALSE` forever | Driver accepts but never resolves |
| DX12 (WARP) | Timestamps available via WARP | Virtual GPU does not support DX12; WARP provides software DX12 with working timestamps |
| Vulkan (Dozen → WARP) | Timestamps available via Dozen + WARP | Dozen translates Vulkan to DX12, WARP executes on CPU |

DX12 and Vulkan timestamps work because they both go through **WARP** (Microsoft's CPU-based software rasteriser), which fully implements timestamp queries. The DX11 path is the only one that hits the virtual GPU driver directly, where the implementation is incomplete.

## Workaround

No application-level workaround exists — the virtual GPU driver simply does not produce timestamp data. The application already handles this gracefully by checking `GetData` return values and skipping GPU timing when unavailable.

Alternative approaches on this class of device:
- **Use DX12 or Vulkan** — both route through WARP, which provides working timestamp queries at the cost of running entirely on the CPU.
- **External tools** — [GPUView](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpuview) or [PIX on Windows](https://devblogs.microsoft.com/pix/) may provide some profiling data, though ARM64 support for these tools is limited.

## Affected Devices

This issue is expected on any system where DX11 runs on a **virtual GPU driver** that does not implement timestamp hardware counters:

- **VirtualBox / VMware VMs** on Windows on ARM (SVGA-prefixed display adapters)
- **Hyper-V VMs** with the Basic Display Adapter (non-GPU-PV configurations)
- Any WoA device where the GPU driver exposes DX11 Feature Level 11_x but lacks timestamp counter support

Devices with **native GPU drivers** (e.g. Qualcomm Adreno on Snapdragon X Elite, NVIDIA, AMD discrete/integrated) are not affected — their drivers implement DX11 timestamp queries correctly.
