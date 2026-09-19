# MPC-HC 频闪 / 黑帧 问题分析（Road 1 D3D11→D3D12 桥接路径）

日期：2026-09-12　硬件：RTX 3080 (sm_86) + 驱动 616.56　现场：K-Lite 版 mpc-hc64.exe + MPC-VR

## 0. 一句话结论

**这不是"渲染出来的帧是黑的"，而是"扫描输出（scanout）那一拍没有内容"。**
证据链的第一环来自你的一句观察：**屏幕录制录不出来**。
录屏走 Desktop Duplication / DWM 合成结果——凡是能被录到的黑帧，一定存在于合成后的画面里。
录不到 = 它发生在**合成层之下**（DXGI flip / 独立翻转 / 硬件 plane 切换）
那一段，只在显示器上闪，从不进入任何"画面"。

## 1. 现场参数（实测 + 从 OSD/录屏帧读出）

**显示输出（实测，`capture_out/refresh_probe.exe`）**：

```
DWM composition timing : refresh period 6.945 ms -> 143.99 Hz
EnumDisplaySettings    : 3840x2160 @ 144 Hz, 32bpp
DPI                    : 200% 缩放（DPI-unaware 进程看到桌面为 1920x1080）
```

`Videos/屏幕录制/屏幕录制 2026-09-12 002206.mp4`（1290x834@30，3.27s）里的 OSD：

```
Engine : Road 1 (NvPresent64 D3D11 Bridge) - FP16 HMMA
Display: 48.0 FPS (Base: 24.0) | FG Latency: 0.62 ms
UI Mask: [F10] ENABLED   (注：该开关在 D3D11 桥接路径里只是显示状态，不参与渲染)
```

- 片源 24fps，MPC-VR 每秒 Present 24 次，驱动插帧翻倍 → 48 帧/秒。
- **48 帧/秒 送 144 Hz 面板 = 正好 3:1**，每帧稳定保持 3 个 vblank。
  → **节拍失配（4:5 / 3:2 pulldown）这一类解释可以彻底排除。**
  （注：OSD 里的 "Display: 48.0 FPS" 是 `2 × 实测 app present 率` 推算出来的，不是对面板的测量，
   但 24→48 的翻倍关系与驱动行为一致。）
- 4K@144 走 DSC，链路带宽紧，显示控制器可用的 overlay plane 更少 →
  DWM/驱动在"合成 / 独立翻转 / MPO"之间切换更频繁。
  **每一次 plane 重配都是一次只体现在面板上、不进任何画面的黑闪机会。**

`sm86_debug.log` 里 MPC-VR 的 Present 调用：

```
HookedPresent #N: sync=0, flags=0x00000200 -> hr=0x087A0001
```

`sync=0` + `flags=0x200(DXGI_PRESENT_ALLOW_TEARING)` = **MPC-VR 处于撕裂/无垂直同步模式**，
并且返回过 `DXGI_STATUS_OCCLUDED (0x087A0001)`——DXGI 的节拍反馈链路已经被打断。

> **修正记录**：初版报告曾写"面板 60.2Hz、48 送 60 是 4:5 失配"。
> 那个 16.7 ms 是探针里我自己的 1/60 s 用户态 pacer（`Sleep`）测出来的间隔，
> **不是** vsync 间隔，因此那次推断无效。实测合成刷新为 143.99 Hz，该结论已撤回。

## 2. 已实测排除的假设（本次实验，非推测）

用带探针的桥接测试程序跑 present 参数矩阵（4 种 sync/flags 组合 + 24fps 复现节拍），
一次 200 帧，逐帧记录 QPC/缓冲索引/阻塞时长/返回值。
原始数据已固化：`capture_out/present_pacing_measurement.csv`（200 行，可重复复现；
同目录 `test_present_flags.exe` 可直接重跑，需 `SM86_DIAG=1`）。

| 组合 | 结果 |
|---|---|
| sync=1 flags=0（正常 vsync） | 40/40 成功，dt p50 16.61 ms |
| sync=0 flags=0（立即） | 40/40 成功，dt p50 16.72 ms |
| sync=1 flags=0x200 | 40/40 成功，dt p50 16.58 ms |
| sync=0 flags=0x200（MPC-VR 撕裂） | 40/40 成功，dt p50 41.85 ms |
| 同上，24fps 节拍 | 40/40 成功，dt p50 41.68 ms |

- 每次 Present 都稳定触发 1 次 `cuGraphLaunch`。
- ⚠️ 表里的 `dt` 列是**探针自己的用户态 pacer**（60fps / 24fps 两种），
  它只证明"Present 没有被拖慢到超过我要求的间隔"，**不能**当作 vsync 间隔读。
  有意义的列是：`hr`（全部 0x00000000）、`buf_idx`、`blk11/blk12`。
- **桥接自身每帧阻塞仅 ~0.5 ms**（D3D11 拷贝+查询等待 p50 197 µs；D3D12 拷贝+fence 等待 p50 325 µs），
  远小于 144Hz 的 6.94 ms vblank 预算 → **桥接不是节拍瓶颈，也不是黑帧来源**。
- 5 种组合全部成功，包括把 `ALLOW_TEARING` 传给"没带该 flag 的影子交换链"——NV 代理容忍了，
  但这是 DXGI 契约违规，且会把交换链带进驱动自己的撕裂呈现路径。
- 影子交换链缓冲索引每次 Present 只前进 1（0,1,2,3,0…），**没有前进 2**。

**最后一条很关键**：如果驱动的"生成帧"是走应用可见的交换链翻页上去的，索引应当一次前进 2。
实际只前进 1 → **驱动自己另外持有一条上屏通路（隐藏缓冲/独立 plane），应用侧完全看不到也控制不了**。
这解释了为什么任何基于合成/交换链的抓取（包括录屏）都抓不到那一帧。

## 3. 代码层发现的确定性缺陷（按影响排序）

1. **子窗口影子交换链 = 第二条独立 plane**
   `CreateWindowExA(..., WS_CHILD | WS_VISIBLE, ...)`，`hbrBackground = nullptr`，
   WndProc 的 `WM_ERASEBKGND` 直接 return 1、`WM_PAINT` 只 validate 不画。
   → 子窗口一创建就是"可见且没有任何内容"的黑矩形；必须等第一次 Present 成功才有画面。
   `src/proxy/d3d11_to_d3d12_bridge.cpp`（CreateD3D12Resources / BridgeWndProc）

2. **每次分辨率/尺寸变化都整体重建桥接**
   `needInit` 比较 `GetWidth/GetHeight/GetHwnd/GetFormat`，而 MPC-HC 会随视频宽高比调整窗口客户区，
   `sm86_debug.log` 里 2617x1689 → 3840x2160 → 2617x1689 反复重建。
   每次重建 = `Shutdown()` 销毁子窗口 → 新建一个"可见空窗口" → 再重新激活 Smooth Motion（新 wrapper，
   生成帧没有历史帧）。**每次重建都会闪一下黑**，且是周期性闪。
   （`EnsureOverlay` 在 D3D11 设备变化时也会 `g_bridge.Shutdown()`。）

3. **永久吞掉 Present 的返回值**
   `HookedPresent` 不检查 `g_bridge.Present()` 的返回值，一律 `return S_OK`，
   应用永远收不到 `DXGI_STATUS_OCCLUDED`/错误 → 播放器无法据此停摆/重排，节拍反馈彻底断掉。

4. **撕裂 flag 直传影子交换链**
   `sd.Flags = 0` 创建，却把应用的 `DXGI_PRESENT_ALLOW_TEARING` 原样传下去。
   NVIDIA 的帧生成本身要求走同步路径；把它带进撕裂路径是"驱动不报错但语义不对"的组合，
   属于最可能出 scanout 级异常的地方。应二选一：给影子交换链补
   `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`（并配合 `FRAME_LATENCY_WAITABLE_OBJECT` + `SetMaximumFrameLatency`），
   或把该 bit 从 flags 里掩掉。

5. **D3D11 的 `GetCurrentBackBufferIndex()` 恒为 0（实测）**
   `tools/test_swap_diag` 实测：flip 模式下 D3D11 交换链的 idx 始终 0，`GetBuffer(1..3)` 直接返回
   `0x887A0001`；D3D11 的翻页由运行时代管，`GetBuffer(0)` 就是当前后备缓冲。
   → 为了"修硬编码 index 0"而改成 `GetCurrentBackBufferIndex()` **是空操作**，方向没错但不会有任何变化。
   （D3D12 侧才必须用 `GetCurrentBackBufferIndex()`。）

6. `GetFrameStatistics()` 在 NvPresent 包装后的交换链上**恒返回 0**（本次实测），不能用来做节拍诊断。

## 4. 建议的验证顺序（按性价比，每步 30 秒～2 分钟）

> 前提：面板 144Hz，24fps→48fps 是干净的 3:1，**不要再把时间花在调刷新率/节拍上**。
> 现象只在扫描输出侧，唯一能取证的方式是**光学拍摄**（手机 240/1000fps 慢动作对着屏幕拍）。

1. **关掉 MPC-VR 的撕裂 / 无垂直同步**（MPC-VR 设置里 "Present at display refresh" 打开、
   允许撕裂关闭）。目标：让日志从 `sync=0, flags=0x00000200` 变成 `sync=1, flags=0x00000000`。
   NVIDIA 的帧生成本就要求走同步路径，这是最便宜、最可能的止血点。
2. **排除 VRR / G-Sync**：把 G-Sync（或 FreeSync/VRR）临时关掉，用固定 144Hz 跑一遍。
   VRR 面板在刷新率来回跳（尤其帧生成导致跨越 LFC 门槛）时会出现**面板级亮度/泛白闪烁**——
   这类现象天生不会被任何录屏录到，和你的观察高度吻合。
3. **判断是"桥接引入"还是"驱动插帧本身"**：临时绕过桥接
   （让 MPC-VR 自己的 D3D11 交换链直接 Present）。注意 `sm86_debug.log` 显示该交换链
   **本身就已经被 NvPresent64 包装**、并且 `ActivateSmoothMotionIfWrapped` 已经在它上面
   把 Smooth Motion 打开了——所以"桥接是否仍然必要"本身就是一个待验证的假设。
   - 绕过桥接后闪烁消失 → 元凶是第二条 plane（子窗口影子交换链）。
   - 仍然闪烁 → 元凶在驱动的插帧/上屏路径，与桥接无关。
4. **复现时用 `SM86_DIAG=1` 采集 per-present CSV**，重点看 `dt`、`buf_idx`、`hr` 三列。
   注意：`GetFrameStatistics` 在包装交换链上恒 0（实测），这条列没有意义，别用它判断节拍。

## 5. 本次留下的诊断能力

- `src/proxy/d3d11_to_d3d12_bridge.{h,cpp}`：`SM86_DIAG=1` 环境变量开启
  → 每帧一行 CSV 到 `sm86_present.csv`（qpc/dt/缓冲索引/sync/flags/两段阻塞耗时/Present 统计/返回值），
  退出时输出 vblank 间隔直方图。**实测结论：`GetFrameStatistics` 在包装交换链上恒 0，
  该列无意义；有意义的列是 dt、buf_idx、copy 耗时、hr。**
- `tools/bmp_stats.py`：批量 BMP 亮度统计，按"全黑/近黑"判定黑帧。
- `tools/analyze_frames.py`：对录屏做**逐 tile 时间中值**异常检测（同时能抓整帧黑与局部黑条），
  比"人眼截一张图"可靠。
- `capture_out/refresh_probe.cpp`（已编译为 `refresh_probe.exe`）：打印 DWM 合成周期、DXGI 输出模式、
  EnumDisplaySettings 与 DPI/缩放——**任何节拍类推断前先跑它**，别再拿用户态 pacer 当 vsync 用。
- `capture_out/display_modes.py`：列出每块显示器的当前模式与全部可用刷新率（查"144Hz 到底在跑没在跑"）。

> 注意：本文件写入期间，仓库里另有编辑者在并发修改 `src/proxy/*`（我新建的 `tools/test_present_flags.cpp`
> 在 00:53 被删除，`CMakeLists.txt` 被还原）。上述结论与实验数据独立于那些改动，可复现。

---

# 附录 A：对"另一 agent 已定位并修复"报告的核验（2026-09-12 01:00）

## A.1 报告中**真实存在**的部分（已逐条核对代码/日志/文件哈希）

| 声明 | 核验结果 |
|---|---|
| 写了 `SubclassedParentWndProc` 吞掉 `WM_ERASEBKGND`/`WM_PAINT` | ✅ 存在（`d3d11_to_d3d12_bridge.cpp:26-41`，其余消息正确转发） |
| 强制父窗口 `WS_CLIPCHILDREN` | ✅ 存在（`:361-365`）**且确实有效**：日志记录父窗口原样式 `was 0x14CF0000`，其中 `0x02000000` 位缺失 → 父窗口原本真没设 `WS_CLIPCHILDREN` |
| 子窗口类 `wc.style = 0` | ✅ 存在（`:271`） |
| 去掉 `sync = max(sync,1)` 强制，改为透传 | ✅ 存在（`:664` 已是 `m_swap12->Present(sync, flags)`） |
| `DXGI_PRESENT_TEST` 旁路 | ✅ 存在（`sm86_rehost.cpp:369, 399`）——这是**真正的正确性修复**，TEST 语义的 Present 不该被桥接 |
| 编译并部署 `VERSION.dll` | ✅ MD5 一致：`build/Release/version.dll` = 部署件 = `a71a5fd8bcdf383683ceb98cec367a35`，旧件已备份为 `VERSION.dll.bak` |
| MPC-VR 配置 `VBlankBeforePresent=1`、`AdjustPresentationTime=1` | ✅ 属实（`HKCU\Software\MPC-BE Filters\MPC Video Renderer`），另有 `UseD3D11=1`、`ExclusiveFullscreen=0` |
| OSD 的 `oldRTV == nullptr` 安全分支 | ⚠️ 该改动**早于本次会话**就已在树里（`osd_overlay.cpp` mtime 23:52），属"把已有 diff 记成了本次改动" |

## A.2 报告中**不成立**的部分

1. **"双重 VBlank 等待"这一根因在物理上不可能产生黑帧。**
   flip 模型下，漏掉/迟到的翻页只会**重复显示上一张缓冲**，不会显示黑色（黑色必须来自被显式提交的黑色内容，
   或一条没有有效内容的 plane）。所以"b) 节拍停顿导致 scanout 丢帧"最多解释**抖动/延迟**，
   不能解释你看到的黑色帧。两个"根因"里只有一个具备产生黑帧的机制。

2. **它的验证无法区分"修好了"和"没修好"。**
   它列出的全部"深度验证"证据（5/5 帧成功、每次 Present 一次 `cuGraphLaunch`、索引 0→1→2→3 轮转、
   `Present -> S_OK`）——我用**修复前**的 build 在 00:52 已经 200/200 帧全部量到过。
   也就是说：**这些证据在故障发生的当时同样成立**，因此不能作为"闪烁已消失"的证据。

3. **它换掉的测试素材改变了节拍工况，根本没复现你的原始条件。**
   `C:\Users\lsp\test.mp4`：2560x1440，`r_frame_rate=29/1`，`avg_frame_rate=49860000/1748179 ≈ 28.52 fps`。
   插帧翻倍 ≈ **57 fps 送 144Hz = 2.525 vblank/帧 → 2/3 交替的不规则保持**，天生带脉冲感。
   而你最初的现象是 24fps 片源（OSD `Base: 24.0`）→ 48 → 144Hz **正好 3:1**。
   两者不是同一个工况。它引用的"稳定 ~35ms 间隔"量的是 **app 侧间隔**，不是面板的 beat。

4. **它引以为据的那次运行里，撕裂路径仍然活着，而它的结论只看了前 60 帧。**
   `sm86_debug.log` 的 present 参数分布：`sync=1 flags=0` ×42、`sync=1 flags=0x200` ×2、
   **`sync=0 flags=0x200` ×2**、`sync=0 flags=0` ×2。
   报告写"All 60 frames ... `m_swap12->Present(1, 0) -> S_OK`"，而日志显示第 120/180 次 Present
   已经是 `sync=0, flags=0x200`（撕裂）。**它验证的窗口恰好落在撕裂尚未出现的区间**，
   而撕裂+插帧正是当前首要怀疑，仍未被测。

5. **新增了一个真实的资源泄漏**：日志显示父窗口被 subclass 了 **3 个不同的 HWND**
   （`:26`、`:90`、`:133`），但 `Shutdown()` 只在 `g_subclassedParentHwnd == m_hwnd` 时还原，
   其余两个窗口会**在整个进程生命周期内保持被我们代理的 WNDPROC**，其 `WM_PAINT`/`WM_ERASEBKGND`
   被无声吞掉。此外"吞掉父窗口绘制"本身会永久改变 MPC-HC 自己的绘制行为（它自己的
   Known Issues 里"缩放时出现未缩放边框"很可能就是这个副作用）。

6. **MPC-VR 的设置里并不存在"允许撕裂"开关**（该键下没有 tearing 相关项），
   所以"去设置里关掉撕裂"这条建议不可执行；正确做法是在桥接侧处理
   （给影子交换链补 `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` + `FRAME_LATENCY_WAITABLE_OBJECT`
   + `SetMaximumFrameLatency`，或把该 flag 掩掉）。

## A.3 结论与唯一能定案的测试

- 这批改动**净收益为正**（`WS_CLIPCHILDREN` 缺失属实、TEST 旁路是正确修复、不再强制 sync 也合理），
  **根因 a（GDI 黑底擦除）是两者中唯一具备"产生黑色"机制的** → 值得保留，但**属未证实**。
- 根因 b 是错的归因；"两项根因"的叙事应降级为"一项待证 + 一项清理"。
- **定案只需 2 分钟 A/B（只能靠眼睛，因为现象在扫描输出侧，录屏天生抓不到）**：
  同一个视频、同一播放位置、同一窗口状态，分别用
  `VERSION.dll`（新，MD5 `a71a5fd8…`）与 `VERSION.dll.bak`（旧，MD5 `5c6efb68…`）各看 3 分钟以上；
  再加三组：窗口拖拽缩放、Alt+Enter 全屏往返、时间轴拖动 seek。
  若要复现原始条件，**必须换回原来那个 24fps 的片子**，不要用 28.5fps 的 `test.mp4`。
- 仍未修复（与本次改动无关）：第二条 plane / 子窗口设计、可见空子窗口、resize 触发的重建抖动、
  `HookedPresent` 吞掉 Present 返回值、`ALLOW_TEARING` 契约。

---

# 附录 B：第二轮修复后的状态（01:04）——"呈现参数"这一整类假设已被证伪

## B.1 已部署的改动（已核对）

- 剥离 `flags & ~0x200`（`d3d11_to_d3d12_bridge.cpp:662`）、强制 `shadowSync`（`:667`）、
  子窗口创建不再带 `WS_VISIBLE`（`:381`，改为首次 Present 成功后 `ShowWindow`）。
- 新 DLL MD5 `e18947f7bbe48143609b64b3d6f0bd15`，与部署件一致。
- ⚠️ 两轮修复互相矛盾：第一轮"去掉 sync 强制"，第二轮"加回 sync 强制"。

## B.2 本轮运行日志（`sm86_debug.log`，尾部为最新一次运行）

```
Frame #1  (app: sync=1 flags=0x0 -> shadow: sync=1 flags=0x0) hr=0x00000000
Frame #60 (app: sync=1 flags=0x0 -> shadow: sync=1 flags=0x0) hr=0x00000000
Frame #120(app: sync=1 flags=0x0 -> shadow: sync=1 flags=0x0) hr=0x00000000
```

**应用此时已处于纯 vsync、无撕裂、flag 合法、且 sync 已强制的状态，仍然频闪。**

## B.3 排除法结论

| 假设 | 状态 |
|---|---|
| 撕裂 / `sync=0` / `ALLOW_TEARING` 契约违规 | ❌ **证伪**（合法 vsync 状态下仍闪） |
| GDI 黑底擦除（`WS_CLIPCHILDREN` + 父窗口 subclass） | ❌ 大概率非主因（已在位仍闪） |
| 桥接自身阻塞（每帧 ~0.5ms） | ❌ 早已实测排除 |
| 第二条 plane / 子窗口拓扑 | ⬜ 未测（两轮都没碰结构） |
| 驱动插入帧的独立上屏通路 / 上屏 plane 重配 | ⬜ 未测 |
| 分辨率变化触发的重建 churn（每次重建都要新建 wrapper + 重激活 Smooth Motion） | ⬜ 未测 |
| VRR / G-Sync 面板级亮度闪烁 | ⬜ 未测 |

**教训：两轮改动都在同一条（已排除的）假设线上加码，且都没有配一个能否证伪的观察。**
下一步必须先做"判别实验"，再改代码。

## B.4 判别实验表（全部不需要改代码，合计约 10 分钟）

| # | 操作 | 观察 | 结论 |
|---|---|---|---|
| 1 | 窗口化播放，把窗口拖到一角，让桌面/任务栏可见 | 只有视频矩形内闪？还是整屏（含桌面）都闪？ | **只在视频内 → 我们的第二条 plane（桥接结构）**；整屏都闪 → 面板/驱动级 |
| 2 | 暂停画面 / 播放静态镜头 | 暂停时仍闪？只在运动画面时闪？ | 静态仍闪 → 呈现/plane，与插帧内容无关；只在运动时闪 → 插帧生成帧内容 |
| 3 | 桌面刷新率切到 60Hz（或 120Hz）再看 | 闪烁频率/观感是否随刷新率成比例变化 | 随之变化 → scanout/VRR 相关；不变 → 与刷新率无关 |
| 4 | 关掉 G-Sync / VRR | 闪烁是否消失/减弱 | 是 → VRR 面板级亮度闪烁（天生录不出来） |
| 5 | 全程不缩放窗口、不切全屏、不 seek，静播 3 分钟 | 不闪？仍闪？ | 只在缩放/全屏/seek 附近闪 → **重建 churn（可修）**；静播持续闪 → plane/驱动插帧 |
| 6 | 把 `VERSION.dll` 改名（整个方案关掉） | 不闪？仍闪？ | 不闪 → 是我们这条链路；仍闪 → MP​C-VR/驱动/面板自身 |

**分支处理**：
- 若 #1 判定"只在视频内" → 唯一真正的结构性差异就是那条**子窗口影子交换链**。
  下一步应换拓扑（用 **DirectComposition**：`CreateSwapChainForComposition` +
  `CreateTargetForHwnd(parentHwnd)`，把影子交换链作为视觉挂进父窗口的合成树，
  **彻底不再创建第二个 HWND / 第二条 plane**），而不是继续调 Present 参数。
- 若 #1 判定"整屏都闪" 或 #4 命中 → 与桥接无关，是 VRR/面板/驱动层，本项目的调参无法解决。
- 若 #5 命中 → 做重建去抖：同 HWND + 同格式下不重建、复用 wrapper、避免 `Shutdown()`
  销毁子窗口（这是纯收益，且与黑帧无关的抖动也会一起消失）。

---

# 附录 C：按实测观察收窄后的修复（01:14）

## C.1 用户实测观察（判别实验 #1/#2/#4 的结果）

- **只有视频矩形内闪**，播放器窗口的 UI 不闪。
- 矩形内**所有内容一起闪**：我们画的 OSD 也闪，视频本身没有内容的黑边也闪。
- **只在运动（有插帧生成）时闪**；静止画面不闪。
- 没开 G-Sync / VRR → VRR 分支排除。

## C.2 从观察得到的推论

1. 闪烁范围 = 子窗口 / 影子交换链的范围 → 问题在这条通路，不是整屏、不是面板。
2. "矩形内所有内容一起闪" → 不是局部畸变，而是**整块缓冲内容不对**
   （生成帧那一拍拿到的是空缓冲 / 别的缓冲）。
3. "只在运动时闪" → 只在驱动**真的生成并放置插值帧**时发生；静止时不生成 → 不闪。
4. 前三轮的 sync / flags / GDI 擦除改动都落在 Present 参数上，而这条嫌疑在
   **交换链创建时的契约**——所以三轮都碰不到它。

## C.3 假设：影子交换链缺少帧延迟契约

`DXGI_SWAP_CHAIN_DESC1 sd = {};` → `Flags = 0`，既没有
`DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`，也没有调用
`IDXGISwapChain2::SetMaximumFrameLatency()`。
帧生成（DLSS-G / Smooth Motion 这一类）要求应用用 waitable object 明确表达
"这张缓冲现在归我写，你不要动"。缺了这个契约，驱动在放置生成帧时无法安全判断哪张缓冲是空闲的，
就可能把一帧翻到内容尚未就绪的缓冲上 —— 表现正是"只在插帧发生时、只在视频矩形内、整块内容一闪"。

## C.4 本次改动（最小、可回退、同 DLL 内可 A/B）

`src/proxy/d3d11_to_d3d12_bridge.{h,cpp}`：

| 位置 | 改动 |
|---|---|
| `CreateD3D12Resources` | `sd.Flags \|= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` |
| 同上 | `QueryInterface(IDXGISwapChain2)` → `SetMaximumFrameLatency(2)` + `GetFrameLatencyWaitableObject()` |
| `Present` | 写缓冲前 `WaitForSingleObject(waitable, 1000)`；超时只记日志，不阻塞业务 |
| `Shutdown` | 释放 `m_swap2_12`；**不** CloseHandle waitable 句柄（归交换链所有） |
| 开关 | `SM86_SHADOW_WAITABLE=0` → 完全回退到改动前行为（同一份 DLL 内 A/B，无需重编） |

**核验记录**：
- 桥接 TU 单独编译通过（`BRIDGE_TU_EXIT=0`）。
- 桥接测试程序：5/5 帧 `bridge.Present -> OK`，每帧触发 1 次 `cuGraphLaunch`，无回归。
- 契约日志：`Shadow frame-latency contract: SetMaximumFrameLatency(2) hr=0x00000000, waitable=000000000000117C`。
- OFF 分支：`Shadow frame-latency waitable DISABLED (SM86_SHADOW_WAITABLE=0 ...)`，链路仍 5/5 正常。
- 新 DLL 导出表与旧件逐项一致（17 个 version 转发函数，ordinal/名称相同）。
- 构建说明：本机 MSBuild 在 WorkBuddy 的 shell 环境下会报 `Path`/`PATH` 大小写重复
  (`MSB6001`)，因此改用 `cl.exe` 直编（`/MD /O2 /DNDEBUG`），产物 59,904 字节，与 CMake Release 量级一致。

**部署链（三份都保留）**：

```
VERSION.dll        = 86944B3DC94D5DFCB2413659171184C7  ← 本次（含帧延迟契约）
VERSION.dll.round2 = E18947F7BBE48143609B64B3D6F0BD15  ← 第二轮（剥离 tearing + 强制 sync）
VERSION.dll.bak    = 5C6EFB68DB0B37455785595F0D37B8D8  ← 第一轮 / 原始
```

## C.5 判定与后续分支

- 闪烁消失或明显减弱 → 确认是帧延迟契约缺失导致生成帧翻到未就绪缓冲。
- 完全无变化 → 在同一份 DLL 内 A/B：`setx SM86_SHADOW_WAITABLE 0` 后重启 MPC-HC，
  再 `setx SM86_SHADOW_WAITABLE 1` 对比。
- 仍闪 → 剩下**最后一个结构嫌疑**：子窗口 + 第二个 flip 交换链 = 第二条 scanout plane。
  换法：用 DirectComposition（`CreateSwapChainForComposition` + `CreateTargetForHwnd(parentHwnd)`）
  把影子交换链挂进父窗口合成树，**不再创建第二个 HWND**。

---

# 附录 D：手机慢动作取证结果（01:17）——撕裂，且撕裂的一侧不是有效帧

## D.1 观察原文与机械含义

> "横着的半个屏幕半个屏幕黑的，上半然后下半，中间间隔线在一定范围内随机分布，
> 有正常亮度和过亮还有黑的现象"

逐条翻译：

| 观察 | 机械含义 |
|---|---|
| 画面被一条**水平线**分成两半，两半内容不同 | **扫描输出中途换了缓冲**（flip 没有对齐到 vblank）——这是撕裂 |
| 分界线高度**在一定范围内随机分布** | flip 落在扫描行程上的**相位是随机的**，不是固定偏移 |
| 两半分别是"正常 / 过亮 / 黑" | 撕裂的两侧里**至少有一侧不是一帧有效内容**：黑 = 未填充的缓冲；过亮 = 部分写入的数据或由被回收缓冲算出来的生成帧 |
| 只发生在视频矩形内、只在运动时 | 这块缓冲就是影子交换链的；只有真的生成插值帧时才会发生 |

**结论（关键）**：这不是插值算法画质问题，而是**缓冲交接问题**——
我们的桥接和驱动的帧生成**对"哪张缓冲现在空闲"没有共识**，
于是屏幕上会出现"半张新内容 + 半张未就绪缓冲"。
这也解释了为什么录屏抓不到：撕裂是扫描输出层的现象，不是合成后画面里的内容。

## D.2 为什么前三轮都碰不到它

前三轮改的是 `Present` 的**参数**（sync / flags / GDI 擦除）。
缓冲交接的规则由**交换链创建时的契约**决定，与 Present 参数无关：

- 影子交换链 `Flags = 0` → 没有 `FRAME_LATENCY_WAITABLE_OBJECT`
- 从未调用 `SetMaximumFrameLatency()`
- `BufferCount = 4`，而帧生成要同时占住"上一真实帧 + 当前真实帧 + 生成帧"的槽位

## D.3 现在的可调参数（无需重编，改环境变量后重启播放器）

| 环境变量 | 默认 | 作用 |
|---|---|---|
| `SM86_SHADOW_WAITABLE` | `1` | `0` = 完全回退到无契约行为（A/B 用） |
| `SM86_SHADOW_MAX_LATENCY` | `1` | 1..3，同时在飞的帧数。**1 = 写缓冲前等上一帧退场，理论上消灭撕裂** |
| `SM86_SHADOW_BUFFERS` | `6` | 4..8，影子交换链缓冲数，给帧生成留槽位 |
| `SM86_DIAG` | `0` | `1` = 逐帧 CSV → `sm86_present.csv` |

## D.4 部署状态

| 文件 | MD5 | 说明 |
|---|---|---|
| `VERSION.dll`（当前部署） | `86944B3D…` | 含帧延迟契约，`SetMaximumFrameLatency(2)`、`BufferCount=4` |
| `VERSION.dll.round3` | `86944B3D…` | 同上（备份） |
| `VERSION.dll.round2` | `E18947F7…` | 第二轮（剥离 tearing + 强制 sync） |
| `VERSION.dll.bak` | `5C6EFB68…` | 第一轮 / 原始 |
| `build/Release/version.dll`（待部署） | `A7C4F5F6…` → 已更新为 latency=1 / buffers=6 | 需关闭 MPC-HC 后由 `deploy_version_dll.bat` 换上 |

> 部署失败原因记录：MPC-HC（PID 71468）正在运行并持有 `VERSION.dll`，
> `Copy-Item` 报"正由另一进程使用"。为此新增 `deploy_version_dll.bat`：
> 先检测进程占用、失败即报错退出，并把当前部署件备份为 `VERSION.dll.prev_<时间戳>`，不删除任何东西。

## D.5 下一轮慢动作要回答的问题（用来区分类似机制）

1. **事件频率**：慢动作一般是 240fps，所以"每 N 帧慢动作出现一次"→ 事件频率 = 240/N。
   是每个显示帧都发生，还是几十帧才一次？
2. **分界线位置**：每次随机高度，还是在一个范围内来回漂移（漂移=固定相位差；随机=真撕裂）？
3. **"过亮"是一整帧，还是只出现在分界线附近的窄带？**（窄带 → 部分写入；整帧 → 生成帧本身错了）
4. **黑的那半是否出现过"错位的视频内容"**（→ 两张有效帧之间撕裂），还是纯黑（→ 未填充缓冲）？

这四条能把"驱动回收了我们正在写的缓冲"与"驱动自己的生成帧翻页"区分开。

---

# 附录 E：慢动作追问结果（01:23）——内容级假设全部排除，只剩"翻页瞬间"

## E.1 新观察

> "过亮是上下某半帧，**没看到过完整的一帧**，全是有分界线的。"

## E.2 推论

- 若驱动的**生成帧本身**是坏的（色调映射错 / 半写 / 由被回收缓冲算出），
  它会被当作一帧完整显示（144Hz 下约 3 个 vblank ≈ 21ms），慢动作一定能拍到**整帧**异常。
- **从来没有整帧异常，每一次异常都带分界线** → **生成帧的内容是好的**。
- 所以缺陷**只存在于缓冲切换的那一瞬间**：屏幕在扫描中途被换到了另一张缓冲，
  而那张缓冲的那部分内容不是本帧应有的内容（黑 = 未写入/被释放；过亮 = 陈旧或垃圾数据）。

**这一步的价值**：把"插值算法 / 色彩 / HDR 色调映射 / 生成帧内容"一整类可能性全部排除，
问题收敛到**交换链缓冲的归属与翻页时序**。

## E.3 新增：结构性 A/B 开关 `SM86_NO_BRIDGE=1`

`src/proxy/sm86_rehost.cpp` 的 `HookedPresent` / `HookedPresent1` 增加旁路：
置 `SM86_NO_BRIDGE=1` 后**整个影子交换链桥接被跳过**，由应用自己的（已被 NvPresent64 包装的）
D3D11 交换链直接 Present，日志会打印
`SM86_NO_BRIDGE=1: shadow bridge bypassed, app swapchain presents directly`。

这一次性回答两个问题：

1. **撕裂是否来自"第二条 plane"**（子窗口 + 第二个 flip 交换链）→ 撕裂消失即确认。
2. **桥接到底是否必需** → 若旁路后 `cuGraphLaunch` 仍然出现，说明驱动在 D3D11 交换链上也能跑帧生成，
   那么桥接整体可以退场（这是比继续调参更彻底的解法）。

## E.4 当前部署与可调项

| 文件 | MD5 | 内容 |
|---|---|---|
| `VERSION.dll`（已部署） | `9E8BDF11051255845B96F96D70D13ACA` | `SM86_SHADOW_WAITABLE=1`、`MAX_LATENCY=1`、`BUFFERS=6`、`SM86_NO_BRIDGE` 开关 |
| `build/Release/version.dll` | 同上 | 与部署件一致 |
| `VERSION.dll.round3` / `.prev_20260912_周六` | `86944B3D…` | 帧延迟契约 latency=2 / buffers=4 |
| `VERSION.dll.round2` | `E18947F7…` | 第二轮（剥离 tearing + 强制 sync） |
| `VERSION.dll.bak` | `5C6EFB68…` | 第一轮 / 原始 |

## E.5 下一步的关键判别（已升级为最重要的一条）

**撕裂事件的频率**——它是"结构性"还是"竞态"的分水岭：

- 若**几乎每个生成帧都在撕**（慢动作 240fps 下每 2-3 帧就能看到一条分界线）
  → 驱动的额外翻页**系统性地**没有对齐 vblank → 参数调优救不了，必须去掉第二条 plane（DComp / 单交换链）。
- 若**只是偶发**（每秒几次）→ 是竞态（缓冲归属偶发不一致）→ `MAX_LATENCY=1` + `BUFFERS=6` 这一类收紧有机会修掉。

慢动作录 3–5 秒，数分界线出现次数即可判断。

---

# 附录 F：A/B 结果（01:26）——"1 有 2 无"，但必须修正解读

## F.1 结果

- **步骤 1**（`MAX_LATENCY=1` + `BUFFERS=6` + waitable）：**仍然有撕裂**。
- **步骤 2**（`SM86_NO_BRIDGE=1`）：**撕裂消失**。

## F.2 日志核验：步骤 2 里插帧其实一次都没跑

对 `sm86_debug.log` 两次运行区段分别统计**真实**的图执行
（`cuGraphLaunch #N (Ampere …)`，注意排除 `IAT cuGraphLaunch hooked` 这类安装日志）：

| 运行 | 真实 cuGraphLaunch | Present 走哪条路 |
|---|---|---|
| 桥接生效（行 1069–1218） | **13 次**（里程碑计数，实际更多） | 部分 `(Bridge Active)` |
| `SM86_NO_BRIDGE=1`（行 1219–末尾） | **0 次** | 全部 `-> hr=0x00000000`（应用自己 Present） |

**所以步骤 2 是"关掉了整个插帧"**：没有生成帧 → 没有驱动额外的翻页 → 自然不撕。
它**不能**证明"第二条 plane 是元凶"，只能证明"**经过影子交换链的插帧会产生撕裂**"。

## F.3 由此确认的结论

1. **桥接是必需的**：旁路后驱动在 D3D11 交换链上**不会**执行插帧（0 次图执行）。
   这一条同时验证了项目最初的设计判断。
2. **撕裂由 NvPresent64 的帧生成在该路径上产生**，与我们的缓冲记账无关
   （waitable / latency 1,2 / buffers 4,6 都试过，无差别）。
3. 撕裂的形态（水平分界、位置随机、两侧为 正常/过亮/黑）与"驱动的合成帧翻页没有落在 vblank 上"一致
   —— 属于**驱动侧的翻页时序**，不是我们能通过 Present 参数修好的东西。
4. ⚠️ **注意**：`setx` 设置的环境变量是**持久的**。`SM86_NO_BRIDGE=1` 现在仍在用户环境里，
   Smooth Motion 处于关闭状态。要恢复插帧：`setx SM86_NO_BRIDGE 0` 后重启播放器。

## F.4 下一步（按性价比）

| 方案 | 做法 | 能回答什么 |
|---|---|---|
| **A. PresentMon / GPUView 测翻页时间线（推荐）** | Intel PresentMon 抓 `mpc-hc64.exe`，看每次 present/flip 的 `DisplayedTime`、`msBetweenPresents`、是否 dropped | 直接看出驱动的翻页是否对齐 vblank、合成帧占几个 vblank —— 这是唯一能把"驱动侧翻页时序"变成数字的手段 |
| B. 单变量 A/B（无需改码） | `SM86_SHADOW_WAITABLE=0` + `SM86_SHADOW_BUFFERS=6`（桥接+插帧，但不用 waitable 契约） | 排除"waitable 契约本身让驱动更难排期" |
| C. 接受现状 | 用 MPC-VR 自带插值（`InterpolateAt50pct`）替代 Smooth Motion 看画质/流畅取舍 | 工程上的兜底 |

**仍需回答的关键数字**：撕裂事件的**频率**。
几乎每个生成帧都撕 → 驱动翻页系统性未对齐 vblank（结构性，需驱动侧或拓扑解决）；
仅偶发 → 竞态/余量不足（还有调参空间）。

---

# 附录 G：PresentMon 实测（01:37–01:43）——根因确认为驱动侧"撕裂模式呈现"

用户回答：**"几乎每个帧都撕"**；**"拖着播放器窗口动时不闪，但好像也没补帧"**。

## G.1 采集方法

`PresentMon 2.5.1 --process_name mpc-hc64.exe --timed 20 --v1_metrics`，
配自生成的 `testsrc2` 24/36/48 fps 1080p 片源；用
`capture_out/display_timeline.py` 从 `TimeInSeconds + msUntilDisplayed` 重建**真实上屏时刻**，
统计"相邻上屏更新间隔 < 1 个 vblank（6.944 ms）"的次数——**每一次即一次扫描中途换缓冲（撕裂）**。

## G.2 关键测量结果

| 源帧率 | 上屏更新率 | 撕裂（亚 vblank 间隔） | 从未上屏的 present |
|---|---|---|---|
| 24 fps | 48.4 /s（= 2×24，插帧翻倍✓） | **143 / 724 = 19.8%** | 4 |
| 36 fps | 57.5 /s | 126 / 867 = 14.5% | 215 |
| 48 fps | 96.1 /s（= 2×48✓） | **295 / 1246 = 23.7%** | 4 |

**所有 present 的参数（三份采集、数千次）**：

```
SyncInterval   = 0        （全部）
PresentFlags   = 0x200    （DXGI_PRESENT_ALLOW_TEARING，全部）
PresentMode    = Hardware: Independent Flip  +  Composed: Flip
```

而同一时刻桥接日志显示我们提交的是 **`app: sync=1 flags=0x0 -> shadow: sync=1 flags=0x0`**。

## G.3 结论

1. **驱动/代理把我们的 `sync=1`、无撕裂请求覆盖成了 `sync=0 + ALLOW_TEARING`。**
   PresentMon 抓到的交换链地址（`0x6963910`）与桥接日志里我们自己的影子交换链
   （`m_swap12 = 0x794290C0`）**不同** → 真正上屏的是**驱动内部那条交换链**，
   我们的 Present 参数根本到不了那一层。
2. **`sync=0` 下翻转是"立刻生效"** → 扫描到一半就换缓冲 → 水平撕裂线、位置随机；
   合成帧因此**基本只以"半屏"形式出现**（与"从没看到完整的一帧"完全吻合）。
3. **撕裂与源帧率无关**（24/36/48 fps 都在 14–24%），所以"对齐帧率"不是解法。
4. **"拖窗口时不闪"正好印证机制**：窗口被拖动时无法使用 Independent Flip，
   走 DWM 合成路径 → 翻转由 DWM 按 vblank 调度 → 不撕（同时插帧效果也消失）。
5. 撕裂比例约 **每 5 次上屏一次**，即每个生成帧都极可能被撕一次。

## G.4 由此得出的可行路径

| 方案 | 原理 | 代价 |
|---|---|---|
| **A. 开启 G-Sync / VRR（先试这个）** | VRR 下一次翻转本身就是一帧扫描的起点，"立刻生效"的翻转不再产生撕裂 | 30 秒，零改动；需面板支持且 VRR 范围覆盖 48Hz |
| **B. 强制走 DWM 合成**（让窗口不被提升为独立翻转层，例如 MPO 禁用或让窗口被遮挡/重叠） | 翻转改由 DWM 按 vblank 调度 → 不撕 | 需确认此时插帧是否仍在工作（看 `sm86_debug.log` 的 `cuGraphLaunch`） |
| **C. 换插值实现（播放器的正解）** | MPC-VR 自带插值 `InterpolateAt50pct=1`；或本项目 **Road 2 的 HLSL VFI**（基于整帧，天然不涉及扫描输出层） | 画质低于 NN 生成帧，但不撕 |
| D. 继续调桥接参数 | — | **已证伪**：参数到不了驱动那一层 |

**结论（诚实版）**：撕裂来自 NvPresent64 自己的呈现路径（`sync=0 + ALLOW_TEARING`），
在**独立翻转 + 固定 144Hz**的组合下必然发生；应用侧无法改它的参数。
因此要么改变显示侧状态（A/B），要么改用不依赖驱动 FG 的插值实现（C）。

---

# 附录 H：ReShade 路线实测（01:45–02:10）——撕裂找到解药，插帧还需自造

## H.0 方案 C 的落地：已把 ReShade add-on 通道打通

按方案 C 的方向，给 MPC-HC 装了 ReShade 6.8.0（**API 20 精确匹配**本项目 add-on）
并挂上了 `sm86_smooth.addon64`。安装与验证细节见 `RESHADE_ADDON_INSTALL.md`。

## H.1 决定性测量：ReShade 路径撕裂 = 0

同素材（1920x1080@24）、同位置，PresentMon + 上屏时刻重建（vblank = 6.944 ms）：

| 配置 | 上屏更新率 | 撕裂率（上屏间隔 < 1 vblank） |
|---|---|---|
| `VERSION.dll` 桥接（驱动 FG） | 48.4 /s | **19.8 %** |
| **ReShade 注入（无桥接）** | 24.4 /s | **0.0 %**（0 / 222） |
| ReShade + `InterpolateAt50pct=1` | 24.4 /s | 0.0 % |
| ReShade + 桥接**同时** | — | **启动即崩** |

**这是本轮最重要的结果**：ReShade 把 present 拉回 DWM 合成路径后，
**扫描层撕裂彻底消失**。反过来印证了附录 G 的判断——撕裂是"驱动 FG + independent flip"
的组合行为，而非我们的缓冲记账错误。

## H.2 两条路线互斥（实测）

ReShade(`dxgi.dll`) 与桥接(`VERSION.dll`) 同时存在时，MPC-HC 在
`ResizeBuffers(2617x1689)` 处弹 `Unexpected error / ACCESS VIOLATION` 崩溃——两套 DXGI 包装互相踩。
已把桥接件停放在 `VERSION.dll.off_for_reshade`，当前为 ReShade-only。

## H.3 add-on 崩溃根因与修复

首次加载即崩，`Crashing module: dxgi.dll, read memory at address 0x0`。
用三段式日志定位到 `on_bind_render_targets_and_depth_stencil`：D3D11 传 `count=8` 而
**`rtvs[0].handle == 0`**（未绑定的空 slot），原代码把它直接交给
`get_resource_from_view()` → ReShade 内部解引用 0x0。

修复：跳过空 slot、逐级判空、并按真实 RT 尺寸重建快照纹理
（init 时 D3D11 给的是 **8x8 占位 backbuffer**）。修复后连续播放 20 秒无崩溃。

## H.4 仍未达成的部分（下一步的工程量）

ReShade 路径下**没有插帧**（24.4 ≈ 源 24fps）。要补上，有两道明确的坎：

1. **跨 API**：ReShade 层是 D3D11，而现成的 `sm86::VfiEngine` 是 **D3D12 专用**。
   需要 D3D11↔D3D12 共享纹理（`d3d11_to_d3d12_bridge.cpp` 里有可复用实现）。
2. **帧率倍增**：MPC-VR 只按源帧率 present，`InterpolateAt50pct=1` **不会**提高 present 频率
   （实测仍 24.4/s），MPC-VR 也没有"每 vblank 呈现"选项。
   必须在 `on_present` 里**自己多产生一次上屏**（flip 模型下多调一次 `Present`）。

## H.5 当前部署状态

```
...\MPC-HC64\
  dxgi.dll                        ReShade 6.8.0（含 add-on 支持）
  sm86_smooth.addon64             本轮修复版（30208 B，02:01）
  VERSION.dll.off_for_reshade     桥接件（已停放）
  VERSION.dll.{round3,round2,bak,prev_*}   历史备份，均保留
```

回退到驱动插帧路径：删/改名 `dxgi.dll` 与 `sm86_smooth.addon64`，
把 `VERSION.dll.off_for_reshade` 改回 `VERSION.dll`（但会带回 19.8% 撕裂）。
