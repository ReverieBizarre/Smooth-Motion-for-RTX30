# 给 MPC-HC 装 ReShade + sm86_smooth add-on（D3D11）

> 目的：走 ReShade 的 **add-on** 通道原生挂接播放器画面（swapchain / Present / DrawCall 级），
> 绕开需要"第二条交换链 + 第二窗口"的驱动插帧路径。

## 0. 结论（已验证可用）

本机已装好并通过实测：

```
ReShade.log:
  Initializing crosire's ReShade version '6.8.0.2155' (64-bit)
      loaded from '...\MPC-HC64\dxgi.dll' into '...\MPC-HC64\mpc-hc64.exe' ...
  Redirecting D3D11CreateDevice(...)            <- 注入 D3D11 设备创建
  > Using feature level b100.
  Redirecting IDXGIFactory2::CreateSwapChainForHwnd(...)  <- 注入交换链创建
  Searching for add-ons (*.addon, *.addon64) in '...\MPC-HC64' ...
  Loading add-on from '...\MPC-HC64\sm86_smooth.addon64' ...
  Registered add-on "SM86 Smooth Motion" v0.0.0.0 using ReShade API version 20.
```

## 0.1 实测数据（关键：ReShade 路径 0% 撕裂）

同一素材（`clip24.mp4`，1920x1080@24）、同一位置、PresentMon 采集 + 上屏时刻重建：

| 配置 | 上屏更新率 | 撕裂率（上屏间隔 < 1 vblank） |
|---|---|---|
| `VERSION.dll` 桥接（驱动 FG） | 48.4 /s | **19.8 %** |
| **ReShade 注入（无桥接）** | 24.4 /s | **0.0 %** |
| ReShade + `InterpolateAt50pct=1` | 24.4 /s | 0.0 % |
| ReShade + 桥接**同时** | — | **启动即崩，不可用** |

两条硬结论：

1. **ReShade 把 present 拉回 DWM 合成路径，扫描层撕裂彻底消失（0/222）。**
   这也解释了之前的困惑：**撕裂是"驱动 FG + independent flip"的组合行为**，不是我们的缓冲记账。
2. **必须二选一。** ReShade(`dxgi.dll`) 与桥接(`VERSION.dll`) 同时存在时，
   MPC-HC 在 `ResizeBuffers(2617x1689)` 处弹出 `Unexpected error / ACCESS VIOLATION` 崩溃
   —— 两套 DXGI 包装互相踩。已在 `VERSION.dll.off_for_reshade` 停放桥接件。

代价：ReShade 路径下**没有插帧**（24.4 ≈ 源 24fps）。驱动 FG 不参与，
所以平滑运动必须**由我们自己在这条路径上做**。

## 0.2 add-on 崩溃根因（已修复）

第一版 add-on 一加载就崩：

```
Exception: ACCESS VIOLATION
Crashing module: ...\MPC-HC64\dxgi.dll      <- ReShade 自身
Offset: 0x125336   tried to read memory at address 0x0
```

**根因**：`on_bind_render_targets_and_depth_stencil` 里收到 `count=8`，而
`rtvs[0].handle == 0`（D3D11 的 `OMSetRenderTargets` 会传满 8 个 slot，其中未绑定的是空）。
原代码直接把空 view 丢给 `device->get_resource_from_view(rtvs[0])`，
ReShade 内部随即解引用 0x0。三条 `AddonLog` 定位到 `on_bind_rt #0` 后即中断。

**修复**（`src/addon/sm86_addon.cpp`）：
1. 跳过 `handle == 0` 的 slot（取第一个非空 RTV）；
2. `cmd_list` / `device` / 结果 handle 逐级判空；
3. init 时 D3D11 给的是 **8x8 占位 backbuffer**，快照纹理尺寸会错 →
   在 bind 时按真实 RT 的 desc **重新分配**快照纹理，避免尺寸不匹配的 `copy_resource`。

修复后实测：连续播放 20 秒无崩溃、进程响应正常、画面正常。

## 1. 关键前提：API 版本必须精确匹配

add-on 编译时用的 `deps/reshade/include/reshade.hpp` 里是 `RESHADE_API_VERSION 20`，
ReShade 在 `ReShadeRegisterAddon(module, 20)` 时会**精确校验**，不匹配直接拒绝加载。

各版本对照（实测抓取自各 tag 的 `include/reshade.hpp`）：

| ReShade | API |
|---|---|
| 6.5.0 | 17 |
| 6.6.0 – 6.7.3 | 18 |
| **6.8.0** | **20** ← 与本项目 add-on 匹配 |
| master | 20 |

**所以必须用 6.8.0**。查法：`curl -s https://raw.githubusercontent.com/crosire/reshade/<tag>/include/reshade.hpp | grep RESHADE_API_VERSION`

## 2. 安装步骤（D3D11）

1. 下载 **带 add-on 支持** 的安装包（标准版不带）：
   `https://reshade.me/downloads/ReShade_Setup_6.8.0_Addon.exe`
2. 它是自解压包，内含 `ReShade32/64.dll` + Vulkan 用的 json（D3D11 不需要 json）：
   `7z x ReShade_Setup_6.8.0_Addon.exe -orsx`
3. 把 `ReShade64.dll` 复制成目标目录的 **`dxgi.dll`**（官方安装器对 D3D11 就是这么做的）。
4. 把 `build\Release\sm86_smooth.addon64` 复制到**同一目录**（ReShade 只从 DLL 所在目录加载 `*.addon64`）。
5. **把 `VERSION.dll` 改名停用**（见下）。

## 3. 为什么必须停用 version.dll

`version.dll` 那条路会把应用的 `Present` **吞掉**（由桥接走影子交换链）。
ReShade 挂在真实的 DXGI/swapchain 上，如果 Present 被吞，ReShade 与 add-on 就**看不到任何帧**。
本机已改名为 `VERSION.dll.off_for_reshade`；要恢复驱动插帧路径就把名字改回 `VERSION.dll`。

> 两条路目前互斥：**ReShade add-on 路径（无驱动插帧，但拿到原生帧）** vs
> **version.dll 桥接路径（有驱动插帧，但已证实必然出现扫描层撕裂）**。

## 4. 使用与验证

- 目录内容应为：`dxgi.dll`（ReShade 6.8.0）、`sm86_smooth.addon64`、`VERSION.dll.off_for_reshade`。
- 启动 `mpc-hc64.exe` 播放视频 → 目录下生成 `ReShade.log`，其中应出现
  `Registered add-on "SM86 Smooth Motion" ... API version 20`。
- 游戏中/播放中按 **Home** 打开 ReShade 覆盖层，里面有 add-on 注册的 **SM86 Smooth Motion** 页签。
- `ReShade.ini` 在首次**正常退出**时生成（强制结束进程不会写）；没有该文件时使用默认值，不影响加载。

## 5. 卸载 / 回退

| 想回到 | 操作 |
|---|---|
| 驱动插帧（version.dll 桥接） | 删除或改名 `dxgi.dll`、`sm86_smooth.addon64`；把 `VERSION.dll.off_for_reshade` 改回 `VERSION.dll` |
| 完全干净 | 上述 + 删除 `ReShade.log` / `ReShade.ini` / `reshade-shaders`（如有） |

## 6. 这个 add-on 现在能做什么 / 不能做什么

**能**：拿到播放器真实 swapchain 的全部事件
（`init_swapchain` / `present` / `draw` / `draw_indexed` / `bind_render_targets_and_depth_stencil`），
可以做 Pass 级 DrawCall 拦截（UI Mask 保护）与 ImGui 覆盖层。

**暂不能**：这个 add-on 当前**不做插帧**。它的 `on_present` 只做帧率/耗时统计与计数器归零；
`sm86::UiMaskEngine` 是 D3D12 取向的实现，在 D3D11 播放器里那部分可能不生效。

## 7. 要在这条路径上做插帧，还差什么

ReShade 层是 **D3D11**（`device api=45056 = d3d11`），而项目现成的插帧引擎
`sm86::VfiEngine`（`src/vfi.h` / `src/vfi.cpp` / `src/shaders/vfi.hlsl`）是
**D3D12 专用**（`ID3D12Device` / `ID3D12GraphicsCommandList`）。这是硬约束，两道坎：

1. **跨 API**：需要 D3D11↔D3D12 共享纹理。项目里 `src/proxy/d3d11_to_d3d12_bridge.cpp`
   已有完整可复用的实现（共享句柄 + keyed mutex + fence 同步）。
2. **帧率倍增**：MPC-VR 只按**源帧率**（24/s）present，实测 `InterpolateAt50pct=1`
   **不会**提高 present 频率（仍 24.4/s），MPC-VR 也没有"每 vblank 呈现"选项。
   要在 144Hz 上看到 48fps，必须在 add-on 的 `on_present` 里**自己多产生一次上屏**
   （flip 模型下多调一次 `Present` 把插值帧推上去）。这是本路线真正的工程点。

> 换句话说：ReShade 给了我们**一条不撕裂的呈现通道**；插帧要自己造。
> 但这条路不必再开第二条交换链，所以不会再触发已经实测到的驱动侧撕裂。
