# sm86_smooth: 在 NVIDIA Ampere (SM86) 架构上解锁 Smooth Motion 平滑运动插帧

[简体中文](README.md) | [English](README_en.md)

[![最新发布版本](https://img.shields.io/github/v/release/ReverieBizarre/Smooth-Motion-for-RTX30?style=flat-square&color=blue)](https://github.com/ReverieBizarre/Smooth-Motion-for-RTX30/releases/latest)
[![构建状态](https://img.shields.io/badge/build-passing-brightgreen.svg?style=flat-square)](#编译指南)
[![运行平台](https://img.shields.io/badge/platform-Windows%20x64-blue.svg?style=flat-square)](#环境依赖)
[![目标架构](https://img.shields.io/badge/target-NVIDIA%20Ampere%20(SM86)-76b900.svg?style=flat-square)](#核心逆向突破深度解析-road-1)
[![图形接口](https://img.shields.io/badge/API-Direct3D%2011%20%7C%20Direct3D%2012%20%7C%20DXGI-orange.svg?style=flat-square)](#架构与注入流程)
[![开源协议](https://img.shields.io/badge/license-MIT%20%2F%20Academic%20Research-lightgrey.svg?style=flat-square)](#法律与合规免责声明)

**`sm86_smooth`** 通过对 NVIDIA 官方驱动组件 `NvPresent64.dll` 进行纯内存二进制重宿主（In-Memory Binary Re-Hosting）与驱动钩子拦截，成功在 **Ampere 架构（GeForce RTX 30 系列 / `sm_86`）** 显卡上解锁了 NVIDIA 官方驱动级 **Smooth Motion（平滑运动 / 驱动级 DLSS 插帧 / 视频插帧 VFI）** 功能。

此前，NVIDIA 在驱动中人为设定了架构软锁，仅允许 Ada Lovelace（RTX 40 系列 / `sm_89`）与 Blackwell（RTX 50 系列 / `sm_120`）使用该特性。本项目不仅在理论与硬件层面证实了 Ampere 的 Tensor Core 能够以**单帧耗时低于 1 毫秒**的极高效率执行该神经网络生成管线，更通过零修改系统磁盘文件的安全方式实现了生产级落地。

除主路线 **Road 1（`NvPresent64` 驱动级纯内存重宿主）** 外，本项目还完整包含 **Road 2（纯 HLSL 独立计算着色器插帧方案）** 作为跨厂商、跨图形 API 的独立纯计算后备路径。

---

## 目录

- [执行摘要与硬件实测状态](#执行摘要与硬件实测状态)
- [Smooth Motion 与 DLSS-G 的本质区别](#smooth-motion-与-dlss-g-的本质区别)
- [核心逆向突破深度解析 (Road 1)](#核心逆向突破深度解析-road-1)
  - [1. 架构分级器双门控绕过 (Dual-Gate Bypass)](#1-架构分级器双门控绕过-dual-gate-bypass)
  - [2. Fatbinary 动态 IAT 挂钩与容器双头补丁](#2-fatbinary-动态-iat-挂钩与容器双头补丁)
  - [3. FP16 与 FP8 内核隔离机制（完全避开 QMMA 715 异常）](#3-fp16-与-fp8-内核隔离机制完全避开-qmma-715-异常)
  - [4. 5 层 COM 交换链对象包装层级与 Smooth Motion 激活](#4-5-层-com-交换链对象包装层级与-smooth-motion-激活)
  - [5. CUDA Graph 乒乓执行流水线与 480x480 分辨率守卫](#5-cuda-graph-乒乓执行流水线与-480x480-分辨率守卫)
- [RTX 3080 实机硬件性能实测 (Benchmarks)](#rtx-3080-实机硬件性能实测-benchmarks)
- [Road 2: 独立纯 HLSL 计算着色器备用方案](#road-2-独立纯-hlsl-计算着色器备用方案)
- [架构与注入流程](#架构与注入流程)
- [项目源码目录结构](#项目源码目录结构)
- [编译指南](#编译指南)
- [游戏部署与使用方法](#游戏部署与使用方法)
- [实机验证与基准测试工具](#实机验证与基准测试工具)
- [反作弊与安全警告](#反作弊与安全警告)
- [法律与合规免责声明](#法律与合规免责声明)

---

## 执行摘要与硬件实测状态

| 评估维度 | Road 1 (`NvPresent64` 内存重宿主) | Road 2 (纯 HLSL 独立计算着色器) |
|---|---|---|
| **核心机制** | 运行时 IAT 内存拦截与 `NvPresent64.dll` 动态重宿主 | 独立的 Direct3D 12 经典金字塔块匹配计算着色器 (`vfi.hlsl`) |
| **硬件需求** | Ampere Tensor Cores (`sm_86`, RTX 30 系列) | 跨厂商通用 (NVIDIA, AMD, Intel, 满足 D3D12 FL 11_0 即可) |
| **1080p GPU 耗时** | **0.622 ms** (理论上限 ~1607 FPS) | 4.31 ms (理论上限 ~232 FPS) |
| **1440p GPU 耗时** | **0.759 ms** (理论上限 ~1317 FPS) | 7.47 ms (理论上限 ~133 FPS) |
| **4K GPU 耗时** | **1.900 ms** (理论上限 ~526 FPS) | 18.2 ms (理论上限 ~55 FPS) |
| **显存占用 (VRAM)** | ~419 MB (1080p) 至 ~526 MB (4K) | ~85 MB (1080p) 至 ~160 MB (4K) |
| **CUDA 依赖** | 复用显卡已安装驱动的 `nvcuda.dll` | 零 CUDA 依赖；纯原生 D3D12 流水线 |
| **运动矢量来源** | 内置光流神经网络自推算；**无需游戏提供运动矢量** | 分层多尺度金字塔块匹配 (SAD) |
| **实测验证状态** | **RTX 3080 12GB 物理硬件 100% 验证通过**：19/19 FP16 fatbin 零报错加载、50 次预热内核通过、连续 `cuGraphLaunch` 帧生成、生成帧 BMP 回读落盘确认无误 | **RTX 3080 12GB 物理硬件 100% 验证通过**：31 个计算 pass、良态区域 28.4 dB PSNR、无头测试通过 |

---

## Smooth Motion 与 DLSS-G 的本质区别

业界常将“DLSS 帧生成”与“Smooth Motion”混为一谈，但二者在架构设计与输入要求上有根本性差异：

```
+-----------------------------------------------------------------------------------+
| DLSS-G (DLSS 3 游戏级帧生成，经由 Streamline / nvngx_dlssg.dll)                  |
| 依赖输入: 游戏引擎运动矢量 (DLSSG.MVecs)、深度缓冲、相机透视矩阵                 |
| 已有项目: dlssg_for_sm86 (NVIDIA 在官方模型包中本来就带有 sm_86 的 Ampere 内核!)  |
+-----------------------------------------------------------------------------------+
                                        对比
+-----------------------------------------------------------------------------------+
| Smooth Motion (驱动级平滑运动插帧，经由 NvPresent64.dll)                          |
| 依赖输入: 仅需 DXGI Present 时的原始渲染后备缓冲 (Backbuffer)。无需游戏任何运动矢量!|
| NVIDIA 官方软锁: 驱动中严格仅包含 sm_89 (Ada) 与 sm_120 (Blackwell)，硬编码阻断 sm_86 |
| 本项目工作: sm86_smooth (本项目 - 攻破架构软锁与 Fatbin 校验，实机成功运行)     |
+-----------------------------------------------------------------------------------+
```

1. **DLSS-G (`nvngx_dlssg.dll`)**：必须由游戏引擎深度集成。游戏必须输出高精度的运动矢量和深度通道。事实上，NVIDIA 在 `C:\ProgramData\NVIDIA\NGX\models\` 中分发的所有 DLSSG 模型（从 310.1 到 310.8）中，**核心神经网络本身早已为 sm_86 编译了 Ampere 原生内核**。因此其他开源项目只需补全包装层与主机入口即可。
2. **Smooth Motion (`NvPresent64.dll`)**：工作在操作系统与 DXGI 交换链层级，面向任意游戏画面的最终呈现。它通过自身包含的双向光流卷积神经网络与特征金字塔，**完全脱离游戏引擎自主估算运动**。NVIDIA 在此处对 Ampere 实施了绝对的驱动级封锁（镜像中完全剥离了 sm_86 内核与 PTX 源码，并设有架构校验门禁）。

`sm86_smooth` 的突破在于将 **Smooth Motion** 完整带入 Ampere 架构，无需游戏专门支持 DLSS 3，即可在单机游戏中享受驱动级硬件插帧。

---

## 核心逆向突破深度解析 (Road 1)

通过对 NVIDIA 驱动核心组件 `NvPresent64.dll` 进行深度反汇编与静态/动态分析，锁定了所有阻碍 Ampere 运行的瓶颈，并逐一予以纯内存级突破：

```
                  +------------------------------------------------+
                  |               游戏进程主程序                    |
                  +------------------------------------------------+
                                          |
                                          | (Windows DLL 搜索优先加载同目录 version.dll)
                                          v
+-----------------------------------------------------------------------------------+
| sm86_smooth (version.dll 代理拦截层)                                              |
|                                                                                   |
|  [1] 双门控特征校验热补丁:                                                         |
|      0xc41f: cmp [rcx+0x14], 3 -> 2   (放行 Tier 2 Ampere 架构进入)               |
|      0xc437: setge sil -> mov sil, 1; nop  (强制置位交换链核心能力标志)             |
|                                                                                   |
|  [2] NvPresent64.dll IAT 动态加载挂钩:                                            |
|      cuModuleLoadData (RVA 0x1d2820) -> hook_cuModuleLoadData                     |
|      -> 动态识别 0xba55ed50 Fatbin 容器二进制结构                                  |
|      -> 重写容器 entry header 架构字段: 0x78/0x59 -> 0x56                         |
|      -> 重写内嵌 ELF 头部 e_flags 标志: -> 0x560556                               |
|      -> 结果: 19 个 FP16 Fatbinaries 零报错返回 0 CUDA_SUCCESS                    |
|                                                                                   |
|  [3] 全局配置门禁开启:                                                             |
|      结构体基地址 base+0x7d7810: S[0x4c]=1, S[0xe8]=1, S[0xe9]=1, S[0x12a5]=1     |
|      调用 NVP_Init_D3D() -> 成功返回 TRUE，激活 DXGI 交换链拦截 Detours            |
|                                                                                   |
|  [4] 交换链包装器拦截与状态激活:                                                   |
|      挂钩 DXGI Present() 与 Present1()                                            |
|      定位 [swap + 0x18] 处的 0x1720 字节内部包装器                                 |
|      调用 vt[19](wrapper, 1) 与 vt[20](wrapper, 1) -> 激活 Smooth Motion 状态     |
|      调度光流 CNN 引擎并在乒乓执行流上启动 cuGraphLaunch 进行实时插帧                |
+-----------------------------------------------------------------------------------+
```

### 1. 架构分级器双门控绕过 (Dual-Gate Bypass)

在设备创建期间，`NvPresent64.dll` 调用 `cuDeviceGetAttribute` 读取主次计算能力版本号，并将 GPU 划分为内部架构层级：
- `sm_75` (Turing) $\rightarrow$ Tier 1
- `sm_86` (Ampere) $\rightarrow$ Tier 2
- `sm_89` (Ada Lovelace) $\rightarrow$ Tier 3
- `sm_120` (Blackwell) $\rightarrow$ Tier 4

位于 RVA `0xc41c` 的门禁校验函数包含如下汇编逻辑：
```x86asm
83 79 14 03       cmp dword ptr [rcx+0x14], 3   ; 检查当前架构层级是否 >= 3 (Ada 或 Blackwell)?
0f 4d eb          cmovge ebp, ebx               ; 若 tier >= 3，则 ebp=0 (成功放行)；否则 ebp=4 (抛出不支持错误码)
0f 9d c6          setge sil                     ; 若 tier >= 3，则 sil=1；否则 sil=0
```

#### 致命陷阱与正确双补丁：
1. **立即数比较补丁**：将偏移 `0xc41f` 处的比较立即数由 `0x03` 改为 `0x02`，使得 Tier 2（Ampere）满足 `>= 2` 条件，从而执行 `cmovge ebp, ebx`，将错误码清零为 `ebp = 0`。
2. **能力标志位 (`sil`) 的关键作用**：早期尝试曾将 `sil` 强制置零以图禁用 FP8。然而逆向深入发现，`sil` 是该函数的主返回值，被直接存入 `[wrapper + 0x3a]`。若 `sil == 0`，`NvPresent64.dll` 会判定设备缺乏插帧能力，**进而彻底跳过 CUDA Graph 的分配并放弃包装交换链**！
3. **最终解决方案**：将 RVA `0xc437` 处的指令替换为 `40 B6 01 90`（即 `mov sil, 1; nop`）。至此双门禁同时成立：错误码被清零，硬件能力被授权。

---

### 2. Fatbinary 动态 IAT 挂钩与容器双头补丁

`NvPresent64.dll` 内部固化了 37 个 Fatbinary 容器（Magic 为 `0xba55ed50`），每个容器均包裹有 `sm_89` 与 `sm_120` 两个架构版本的编译镜像。

如果直接使用未修改的 Fatbin 在 Ampere 上调用 `cuModuleLoadData`，驱动将直接返回错误码 `209 CUDA_ERROR_NO_BINARY_FOR_GPU`。

**为什么单纯修改内嵌 ELF 的 `e_flags` 仍旧报错**：
CUDA 驱动程序内部的 Fatbin 容器解析器具有双重检查逻辑：它会**先验证 16 字节容器 entry header 中的架构字段**。entry header 中偏移 `+0x2c` 处存有 `0x78`（sm_120），偏移 `+0x22fc` 处存有 `0x59`（sm_89）。若这两个容器标签与当前 GPU 不匹配，驱动在触及内部 ELF 之前就已提前失败！

**纯内存 IAT 挂钩解决方案**：
`sm86_smooth` 直接挂钩 `NvPresent64.dll` 导入表中位于 RVA `0x1d2820` 的 `cuModuleLoadData` 函数指针：
```cpp
static void patch_fatbin(uint8_t* b, size_t size) {
    // 1. 扫描并修改 Fatbin 容器 entry header 中的 32 位架构标识
    for (size_t off = 0; off + 3 < size; off += 4)
        if ((b[off] == 0x78 || b[off] == 0x59) && b[off+1] == 0 && b[off+2] == 0 && b[off+3] == 0)
            b[off] = 0x56; // 重写为 sm_86

    // 2. 扫描并修改内嵌 ELF 镜像的 e_flags
    for (size_t off = 0; off + 0x34 < size; ++off)
        if (b[off] == 0x7f && b[off+1] == 'E' && b[off+2] == 'L' && b[off+3] == 'F') {
            uint32_t fl = 0x560556; // 重写为 sm_86 标识
            memcpy(b + off + 0x30, &fl, 4);
        }
}
```
当 `NvPresent64.dll` 请求驱动加载模块时，钩子函数在私有缓冲区中实时重写上述字段，随后交由 `nvcuda.dll` 处理。**全部 19 个 FP16 Fatbinaries 零报错成功加载（状态码 0），完全无需改动系统驱动文件。**

---

### 3. FP16 与 FP8 内核隔离机制（完全避开 QMMA 715 异常）

`NvPresent64.dll` 在 VA `0x18022ead0` 处维护了 36 个内核指针入口表：
- 索引 `[0..18]`：**19 个 FP16 Fatbinaries**（包含 `conv1..8`、`conv_fused`、`conv_proj1/2`、`conv_out1/2/3`、`attn1/2`、`depth_to_space`、`downscale_kernel`、`warp_coarse_kernel`、`main_kernel`）。
- 索引 `[19..35]`：**17 个 FP8 Fatbinaries**（名称带有 `_fp8` 后缀）。

#### SASS 硬件级执行实测验证 (RTX 3080)：
| 内核分类 | 核心指令 | `sm_89` 原生二进制 | `sm_89` 补丁至 `sm_86` | Ampere 物理硬件执行结果 |
|---|---|---|---|---|
| **FP16 MMA** | `HMMA.16816.F32` | 209 `NO_BINARY` | 0 `CUDA_SUCCESS` | **0 `CUDA_SUCCESS`** (128/128 元素计算结果与原生 sm_86 完全一致) |
| **FP8 MMA** | `QMMA.16832.F32` | 209 `NO_BINARY` | 0 `CUDA_SUCCESS` | **715 `CUDA_ERROR_ILLEGAL_INSTRUCTION` (非法指令)** |

Ampere 架构的第三代 Tensor Core 不包含 FP8 矩阵乘加硬件单元（`QMMA`）。如果盲目将设备架构伪报为 `sm_89`，会导致驱动调度执行 `QMMA` 指令，从而立刻引发 GPU 崩溃。

**核心发现**：由于我们的门控补丁让设备保持真实的 **Tier 2 (Ampere)** 身份，`NvPresent64.dll` 内部的调度器**会自动且仅选用 FP16 内核表**！驱动仅加载 19 个 FP16 内核，从始至终不会触发任何 FP8 内核。所有指令完全运行在 Ampere 的原生 `HMMA` 单元之上。

---

### 4. 5 层 COM 交换链对象包装层级与 Smooth Motion 激活

`NvPresent64.dll` 并未直接修改应用自身的交换链指针，而是在 DXGI 外部构建了一套复合 COM 包装代理：

```
[游戏主渲染引擎] -> IDXGISwapChain* (大小为 0x20 字节的代理对象，虚表 0x1801d3228)
                       |
                       +--> 偏移 +0x18: 内部交换链包装器 (大小 0x1720 字节，虚表 0x1801d39c0)
                                 |
                                 +--> vt[19](wrapper, 1): 开启 Smooth Motion 特征使能
                                 +--> vt[20](wrapper, 1): 设置呈现模式生效
                                 +--> 偏移 +0x80: 隐藏后备缓冲池 (Hidden buffer [0..1]，2-4 张内部纹理)
                                 +--> 偏移 +0xa28: 光流 CNN 推理与扭曲调度引擎
```

当游戏主循环调用 `IDXGISwapChain::Present`（槽位 8）或 `Present1`（槽位 22）时：
1. 代理钩子提取 `[swap + 0x18]` 指针。
2. 首次呈现时，依次调用 `vt[19](wrapper, 1)` 与 `vt[20](wrapper, 1)`。
3. 驱动将渲染后备缓冲导入内部 DXGI/CUDA 互操作缓冲池（512x512 至 4K 分辨率，格式 `R8G8B8A8_UNORM`）。

---

### 5. CUDA Graph 乒乓执行流水线与 480x480 分辨率守卫

激活插帧后，`NvPresent64.dll` 将创建两个可执行 CUDA 计算图实例：
- `gExec_0 = 0x...120`
- `gExec_1 = 0x...C60`

在后续的每一次 `Present()` 调用中，驱动在专用 CUDA 流上交替调度 `cuGraphLaunch`，使光流估算与神经网络扭曲完全并行：

```
[呈现 #0] -> 捕获初始帧 0
[呈现 #1] -> 启动 cuGraphLaunch #1 (gExec_0) -> 生成并呈现 0.5 帧 -> 紧接着呈现真实第 1 帧
[呈现 #2] -> 启动 cuGraphLaunch #2 (gExec_1) -> 生成并呈现 1.5 帧 -> 紧接着呈现真实第 2 帧
[呈现 #3] -> 启动 cuGraphLaunch #3 (gExec_0) -> 生成并呈现 2.5 帧 -> 紧接着呈现真实第 3 帧
```

> [!NOTE]
> **分辨率安全阈值**：在逆向分析 `CreateSwapChainForHwnd` 拦截函数时，发现了内置的分辨率过滤条件：
> `Width >= 480 && Height >= 480`。低于 480x480 分辨率的微型窗口或辅助控件将自动直通显示，不参与插帧。

---

## RTX 3080 实机硬件性能实测 (Benchmarks)

以下测试数据均在 **NVIDIA GeForce RTX 3080 12GB** 物理显卡（驱动版本 616.56，Ampere `sm_86`）上通过基准测试工具（`tools/nvp_perf_bench.cpp`）实测获得：

```
================================================================
  NvPresent64 Smooth Motion (Road 1) Performance Benchmark
  GPU: NVIDIA GeForce RTX 3080 (sm_86, Ampere)
================================================================
```

### 延迟与显存开销测试表：

| 目标渲染分辨率 | 画面比例 | 平均 GPU 插帧耗时 | 最低耗时 | 最高耗时 | 理论吞吐上限 FPS | 显存增量开销 (VRAM) |
|---|---|---|---|---|---|---|
| **1920 x 1080 (1080p)** | 16:9 | **0.622 ms** | 0.552 ms | 1.011 ms | **1607.1 FPS** | **+419.2 MB** |
| **2560 x 1440 (1440p)** | 16:9 | **0.759 ms** | 0.722 ms | 1.055 ms | **1317.1 FPS** | **+465.0 MB** |
| **3840 x 2160 (4K UHD)** | 16:9 | **1.900 ms** | 1.322 ms | 4.181 ms | **526.2 FPS** | **+526.8 MB** |

### 测试结论分析：
- **帧时间开销极小**：在 1080p 与 1440p 主流分辨率下，单次插帧操作的 GPU 耗时**不到 0.8 毫秒**。在 60Hz（单帧预算 16.66 ms）或 144Hz（单帧预算 6.94 ms）下，插帧仅占用画面渲染周期的 **5% ~ 11%**，玩家几乎感知不到额外的 GPU 负载。
- **显存常驻开销固定**：无论游戏场景几何体或贴图有多复杂，Smooth Motion 的显存占用均严格保持在 419 MB 至 527 MB 之间，仅包含光流金字塔特征图、乒乓交互缓冲与计算图执行权重的开销。

---

## Road 2: 独立纯 HLSL 计算着色器备用方案

对于非 NVIDIA 显卡（如 AMD Radeon、Intel Arc）或不包含 CUDA 环境的轻量化场景，项目完整保留了 **Road 2** 纯着色器插帧方案（`src/shaders/vfi.hlsl` 与 `src/vfi.cpp`）：

```
luma (x2)                          RGBA 转换为 R32F / R16F 亮度（Rec.709 权重）
luma pyramid (x6)                  生成 1/1 -> 1/2 -> 1/4 -> 1/8 四级金字塔
half-res colour base (x4)          低频基底与高频细节拆分
block matching, 4x4, 双向搜索      1/8 广域搜索 -> 1/4 -> 1/2 -> 1/1 逐级局部细化
3x3 median filtering (x2)          中值滤波剔除运动突刺噪点
multi-scale hole fill (x4)         跨距为 6 与 12 的遮挡区域填充
warp + blend (x1)                  遮挡掩码加权双向扭曲 + 高频细节保真回贴
```

### Road 2 核心设计要点：
- **FP16 亮度通道加速**：通过使用 `R16_FLOAT` 存储格式，金字塔读取显存带宽减半，运算性能提升 2.67 倍（1080p 耗时从 11.49 ms 缩减至 4.31 ms）。
- **共享内存平铺 (Groupshared Tile)**：在细化阶段利用 76x76 浮点平铺共享内存（23 KB），将全局显存读取带宽降低了约 20 倍。
- **基底与细节频段分离**：对平滑的低频颜色执行运动扭曲，对原始高频边缘细节予以直接保真回贴，有效消除运动模糊。

---

## 架构与注入流程

```
[游戏可执行程序] (例如 Game.exe)
        |
        | [Windows 加载机制优先载入同目录下的 version.dll]
        v
+--------------------------------------------------------------------+
| version.dll (sm86_smooth 代理模块)                                 |
|                                                                    |
|  1. 将全部 17 个 Version API 无缝转发至 C:\Windows\System32\version.dll|
|  2. DllMain 创建独立初始化线程 (彻底规避 Loader Lock 死锁风险)      |
|  3. 初始化工作线程:                                                 |
|     a. 从驱动仓库或系统目录中动态加载 NvPresent64.dll                |
|     b. 内存补丁: RVA 0xc41f (cmp 3->2), RVA 0xc437 (mov sil, 1)   |
|     c. IAT 钩子: cuModuleLoadData -> 动态重写 Fatbin 架构与 e_flags |
|     d. 开启全局配置门禁: S[0x4c]=1, S[0xe8]=1, S[0xe9]=1, S[0x12a5]=1|
|     e. 调用 NVP_Init_D3D() -> 安装 DXGI 交换链拦截 Detours          |
|     f. 在 DXGI 虚表槽位 8 与 22 处挂载 HookedPresent                |
+--------------------------------------------------------------------+
        |
        v
[游戏调用 DXGI CreateSwapChainForHwnd]
        |
        +--> NvPresent64 自动生成代理对象并封装内部交换链包装器 (+0x18)
        |
[游戏调用 DXGI Present / Present1 呈现画面]
        |
        +--> HookedPresent 调用 vt[19] 与 vt[20] 激活 Smooth Motion
        +--> cuGraphLaunch 异步执行插帧神经网络
        +--> 生成帧与真实帧依次翻转呈现在屏幕上
```

---

## 项目源码目录结构

```
sm86_smooth/
├── CMakeLists.txt              # 现代化 CMake 统一工程配置 (MSVC C++17)
├── build.bat                   # 一键自动化编译脚本 (Visual Studio 2022)
├── dev_build.bat               # 快速 cl.exe 增量编译脚本
├── README.md                   # 英文技术文档
├── README_zh.md                # 中文技术文档 (本文件)
│
├── config/
│   └── sm86_smooth.ini         # 运行时配置文件 (搜索半径、融合权重、日志级别)
│
├── src/
│   ├── proxy/
│   │   ├── sm86_rehost.cpp     # [Road 1] 生产级 version.dll 动态代理与 NvPresent64 注入重宿主
│   │   ├── proxy.cpp           # [Road 2] 原生 D3D12 代理注入层
│   │   └── version.def         # Version.dll 导出函数转发定义
│   ├── shaders/
│   │   └── vfi.hlsl            # [Road 2] 纯 HLSL 计算着色器插帧内核 (cs_5_0)
│   ├── vfi.cpp                 # [Road 2] Direct3D 12 独立插帧引擎实现
│   └── vfi.h                   # [Road 2] 插帧引擎头文件与描述符管理
│
├── tools/
│   ├── nvp_live_test.cpp       # [Road 1] 端到端实机验证与生成帧 BMP 回读落盘工具
│   ├── nvp_perf_bench.cpp      # [Road 1] 高精度 GPU 耗时与显存基准测试套件
│   ├── selftest.cpp            # [Road 2] 无头模式 D3D12 测试与 PSNR 评估程序
│   ├── proxytest.cpp           # version.dll 导出转发有效性校验工具
│   ├── isa_exec_test.py        # SASS 级 HMMA 与 QMMA 指令硬件执行测试脚本
│   ├── kernel_twin_compare.py  # FP16 与 FP8 内核镜像资源比对工具
│   └── patch_nvpresent.py      # NvPresent64 静态分析与补丁分析脚本
│
└── demo_out/                   # 实机测试生成的 BMP 画面回读存放目录
```

---

## 编译指南

### 环境依赖
- **操作系统**：Windows 10 / Windows 11 (64 位)
- **编译工具**：Visual Studio 2022（Community、Professional 或 Build Tools 均可，勾选“使用 C++ 的桌面开发”）
- **SDK**：Windows 10 / 11 SDK（内置 DirectX 11 与 DirectX 12 支持）
- **构建工具**：CMake $\ge$ 3.20
- *注：构建本项目**完全不需要**预先安装 CUDA Toolkit 或 NVIDIA Optical Flow SDK！*

### 一键构建
在项目根目录下直接运行 `build.bat`：

```cmd
build.bat
```

如需清理并完全重新编译：
```cmd
build.bat clean
```

### 生成产物说明
编译成功后，产物将输出在 `build\Release\` 目录下：
- `version.dll`：用于放入游戏目录下的免侵入式注入代理 DLL。
- `nvp_live_test.exe`：用于验证本机显卡是否已完全激活 Smooth Motion 的测试程序。
- `nvp_perf_bench.exe`：用于多分辨率插帧延迟与显存消耗的实机基准测试程序。
- `vfi_selftest.exe`：Road 2 纯计算着色器管线的无头自测程序。
- `proxytest.exe`：用于验证 17 个 Version API 是否成功转发至系统库的校验程序。

---

## 游戏部署与使用方法

### 第一步：复制必要文件
将编译生成的 `version.dll` 复制到目标游戏的根目录中（即游戏主程序 `.exe` 所在的同级目录）：

```
游戏目录/
├── Game.exe
├── version.dll             <-- 复制自 build\Release\version.dll
├── sm86_smooth.ini         <-- 复制自 config\sm86_smooth.ini (可选配置)
└── shaders/
    └── vfi.hlsl            <-- (仅在选用 Road 2 纯着色器路线时需要)
```

### 第二步：参数调优（可选 `sm86_smooth.ini`）
如果需要微调运行时表现，可编辑配置文件：
```ini
[frame_gen]
enabled=1                   ; 1 = 开启插帧, 0 = 旁路直通
extrapolate=0               ; 0 = 中间插帧 (0.5 时间步), 1 = 外推预测

[present]
buffer_bump=2               ; 额外申请的交换链后备缓冲数量
max_in_flight=3             ; 队列中允许并行的最大命令列表帧数

[log]
log=1                       ; 输出 JSONL 诊断日志至 logs\native_<pid>.jsonl
```

### 第三步：启动游戏
照常启动游戏即可。Windows 加载器将自动优先加载同目录下的 `version.dll`，随后代理库会在后台线程中自动完成对 `NvPresent64.dll` 的内存补丁与交换链挂钩，并无感开启 Smooth Motion。

---

## 实机验证与基准测试工具

如果你想在本机显卡上验证 `NvPresent64.dll` 的插帧计算图是否顺利跑通：

### 1. 运行实机端到端验证程序
```cmd
build\Release\nvp_live_test.exe
```
**正常输出示例**：
```
[+] Loaded NvPresent64.dll @ 00007FFA5E9C0000
[+] Gate patched: Tier 2 allowed, sil=1 forced
[+] NvPresent64.dll IAT hooked (cuModuleLoadData, cuLaunchKernel, cuGraphLaunch)
[+] NVP_Init_D3D() -> TRUE
[+] SwapChain created (Proxy COM Object, Internal Wrapper @ 0000018A688E5160)
[+] Smooth Motion activated on wrapper (vt[19]=1, vt[20]=1)
  >>> cuGraphLaunch #1: gExec=0000018ACF6A0120 stream=...
  <<< cuGraphLaunch result=0
[Frame 0] swap->Present -> 0x00000000 (graphs launched: +1)
================================================================
  SUMMARY STATS:
    - FP16 Fatbinaries Loaded: 19 (19 expected on Tier 2)
    - Warmup cuLaunchKernel:   50
    - Frame cuGraphLaunch:     5 (1 per Present)
================================================================
[+] SUCCESS: Road 1 Live Frame Generation fully verified on sm_86!
```

### 2. 运行性能与显存基准测试
```cmd
build\Release\nvp_perf_bench.exe
```
将自动测试 1080p、1440p 与 4K 分辨率下的精确 GPU 渲染耗时并计算出理论帧率与显存开销。

---

## 反作弊与安全警告

> [!CAUTION]
> **严正警告：本项目仅限单机与离线游戏研究使用！**
> 
> `sm86_smooth` 使用了标准的 DLL 劫持代理（`version.dll`）、内存热补丁与 DXGI 虚表挂钩技术。
> 
> 现今的绝大多数在线多人反作弊系统（包括 Easy Anti-Cheat、BattlEye、Ricochet、Vanguard、VAC 等）均会对加载模块签名、驱动内存完整性以及 DXGI 呈现接口进行严格校验。
> 
> **切勿在任何联机游戏、防作弊对战游戏中使用本工具，否则将面临封号风险！**

---

## 法律与合规免责声明

1. **净室设计实现（Clean-Room）**：本项目不包含、不复制、不重新分发任何 NVIDIA 专有代码、CUDA 编译二进制文件、模型权重文件或预编译 Fatbin。
2. **纯内存动态转换**：所有补丁与挂钩均在程序运行期间仅作用于调用进程自身的私有虚拟内存中，未对操作系统 `C:\Windows` 或驱动仓库目录下的任何物理文件进行篡改。
3. **学术研究与互操作性目的**：本项目旨在用于 GPU 微架构指令集兼容性分析、驱动底层呈现机制研究以及合理使用（Fair Use）范畴内的学术探索。
4. **商标声明**：NVIDIA、GeForce、RTX、DLSS 及其架构代号均为 NVIDIA Corporation 的注册商标。本项目与 NVIDIA 公司无任何关联、赞助或背书关系。
