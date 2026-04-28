## L2T1 模式说明（两空间层、单时间层）

本文档专门描述本项目中 `--av1-scalability l2t1` 的行为、依赖语义、以及如何运行发送端/接收端做本地封装与统计验证。

### 1) 这个模式解决什么问题？

在没有真实 AV1 编码器的前提下，`l2t1` 用 **“随机负载 + RTP + AV1 Dependency Descriptor (DD)”** 模拟：

- **S0（基础层）**：每个时刻都可独立解码（可理解为每个时刻都是可切入点）。
- **S1（增强层）**：只依赖**同一时刻的 S0**，不依赖历史 S1。

接收端实现 **deadline 播放**：在每个时刻的 deadline 到达时，尽力选择最高可解码层播放（S1 > S0），并输出 stall/质量评估。

### 2) 发包组织方式（非常重要）

- 每个“时刻/temporal unit”（由 RTP timestamp 标识）会产生 **2 个独立 frame**：
  - `spatial_id=0` 的 S0 frame
  - `spatial_id=1` 的 S1 frame
- **同一时刻的两个 frame 共享同一个 RTP timestamp**（便于接收端把它们归为同一时刻）。
- DD 的 `frame_number` 是 16-bit 循环递增值，用于每个 frame 的解码序编号。

### 3) AV1 DD 模板结构（Template Dependency Structure）

`l2t1` 会在会话开始时通过 DD 的 `template_dependency_structure_present_flag=1` 发送一次模板结构（接收端会缓存），模板包含：

- 两个 template（S0 和 S1）
- 两个 decode targets（用于语义表达，当前接收端不依赖 DTI 做决策，但 DD 语义是自洽的）
- S1 的 `fdiff=1`（表示它依赖紧邻的前一个 DD frame_number；结合我们的发送顺序，使其依赖同一时刻的 S0）
- `chain_cnt=0`（不使用 chain）

### 4) 发送端如何运行（`bin/sender`）

#### 4.1 编译

在仓库根目录：

```bash
make -j4
```

可执行文件位置：
- `bin/sender`
- `bin/receiver`

#### 4.2 本机回环联调（推荐）

先启动接收端：

```bash
./bin/receiver \
  --bind-ip 127.0.0.1 --bind-port 5201 \
  --log-dir ./testlogs/l2t1_run \
  --deadline-ms 100 --playout-fps 30
```

再启动发送端（`l2t1`）：

```bash
./bin/sender \
  --dst-ip 127.0.0.1 --dst-port 5201 \
  --av1-scalability l2t1 \
  --spatial-layer-ratios 1,1 \
  --bitrate 5M --fps 30 --pkt-size 1200 \
  --pacer off \
  --log-dir ./testlogs/l2t1_run
```

#### 4.3 关键参数

- **`--av1-scalability l2t1`**：启用 L2T1 模式。
- **`--spatial-layer-ratios R0,R1`**：S0/S1 两层的发包预算比例（默认 `50,50`）。例如：
  - `1,1`：S0/S1 基本均分
  - `1,3`：增强层占更大比例（更容易观察 S1 丢包/延迟时的回落行为）

### 5) 接收端如何运行（`bin/receiver`）

接收端会做三类事情：

1. 解析 RTP + DD + 自定义 `stream_payload_header`
2. 生成 TWCC 风格的接收反馈（项目里用自定义 RTCP 包承载）
3. **新增：按时刻 deadline 的播放/评分器**，输出 stall 与质量

#### 5.1 关键参数

- **`--deadline-ms`**：每个时刻从“首次收到该时刻任意包”开始计时的 deadline（默认 `100` ms）。
- **`--playout-fps`**：stall 统计使用的帧率（默认 `30`）。

### 6) 输出日志在哪里？

发送端（逐帧）：
- `send_<sec>.csv`（位于 `--log-dir`）

接收端（逐包）：
- `recv_<sec>.csv`（位于 `--log-dir`）

接收端（播放级统计，逐时刻一行）：
- `playback_<sec>.csv`（位于 `--log-dir`）

`playback_*.csv` 列含义：
- `deadline_us`：该时刻的 deadline 时间戳（接收端单调时钟 us）
- `temporal_unit_id`：RTP timestamp（我们把它当作时刻 id）
- `chosen_sid`：最终选择播放的层（1=S1，0=S0，-1=stall）
- `stall_us`：该时刻贡献的 stall（按 `1/playout_fps` 计）
- `decodable_bytes`：被选中层在该时刻收齐后可解码的字节数
- `total_bytes`：分母（优先用 S1 的估算总字节，缺失则回退 S0，避免除零）
- `quality`：`decodable_bytes/total_bytes`

### 7) 具体程序端在哪里（代码入口/关键文件）

- **发送端入口**：`src/sender.c`
  - 参数解析：`parse_args()`
  - RTP+DD 封装：`build_rtp_av1_prefix()` / `build_av1_dependency_descriptor()`
  - L2T1 模板结构：`build_av1_l2t1_template_structure()`
  - L2T1 帧层信息：`av1_get_frame_layer_desc(..., spatial_id)`
  - 按时刻产生 S0/S1 两个 frame：主循环（`while (!g_stop)`）

- **接收端入口**：`src/receiver.c`
  - RTP+DD 解析：`look_up_pack_av1_in_core()` / `parse_av1_dependency_descriptor()` / `parse_av1_template_dependency_structure()`
  - 播放/评分器（deadline、stall、quality）：`playback_*` 相关结构与函数

- **公共协议头**：`src/proto.h`
  - RTP header 结构体
  - DD 扩展 id 常量：`RTP_EXT_ID_AV1_DEPENDENCY_DESCRIPTOR`
  - 自定义 `stream_payload_header`

### 8) 已知限制/说明

- 当前 `total_bytes` 是基于接收端估算（按 `frame_packet_count * 首包长度` 近似），用于快速实验；如果你希望分母严格等于发送端“计划总字节”，建议在自定义头里增加显式字段再做精确计算。

### 9) 如何使用脚本评估（send/recv/playback 联合统计）

项目提供离线分析脚本 `scripts/analyze_l2t1_logs.py`，用于从三类 CSV 日志联合统计并输出到控制台：

- `send_*.csv`：发送端逐帧统计（目标码率、planned/sent 包数）
- `recv_*.csv`：接收端逐包统计（到达时间、`send_seq`、空间层等）
- `playback_*.csv`：接收端逐时刻播放决策与质量（`chosen_sid/stall/quality`）

#### 9.1 使用方式

在仓库根目录执行：

```bash
python3 scripts/analyze_l2t1_logs.py \
  --send-csv <log_dir>/send_<sec>.csv \
  --recv-csv <log_dir>/recv_<sec>.csv \
  --playback-csv <log_dir>/playback_<sec>.csv \
  --spatial-ratios 1,1 \
  --pkt-size 1200 \
  --denom sent \
  --window-ms 1000 \
  --join-key send_seq_tu \
  --top-n 8
```

参数说明（常用）：

- **`--spatial-ratios`**：对应发送端 `--spatial-layer-ratios`（用于把每个时刻的包预算按 S0/S1 拆分，从而做“方案B真值分母”质量评估）
- **`--pkt-size`**：对应发送端 `--pkt-size`（用于把包数换算为字节数分母）
- **`--denom sent|planned`**：真值分母使用 `sent_frame_packets`（默认）或 `planned_frame_packets`
- **`--window-ms`**：窗口化统计接收 goodput/乱序等动态指标的窗口大小
- **`--join-key send_seq_tu`**：推荐默认。通过 `send_seq` 与 `send_*.csv` 的包数区间映射，把 `recv_*.csv` 的逐包数据对齐到 `playback_*.csv` 的 `temporal_unit_id`（更稳健；不要假设 `recv.frame_id == temporal_unit_id`）

#### 9.2 输出包含哪些指标（用于对比 estimator）

脚本会输出以下摘要（按整体与按层统计）：

- **可播放率/卡顿**：`playable_rate`、`stall_frames`、连续 stall run 的分位数
- **层占用与切换稳定性**：`chosen_sid` 分布、切换次数/频率
- **方案B真值分母 quality**：使用 `send_*.csv` 还原每时刻每层总包数（结合 `--spatial-ratios/--pkt-size`）后重算 `quality_true(B)`
- **接收侧逐包健康度**：重复率、乱序率、乱序深度分位数
- **完成时间与 deadline 余量**：`margin_us = completion_ts - deadline_us` 的分布与 miss 率、`completion_ratio`、`goodput_before_deadline_ratio`
- **分层解释性**：`S1-S0 completion` 差值分布、`S1 aggressive` / `could_up_not_up` 事件率
- **动态指标**：窗口 goodput 分布与 `CV`（抖动强弱）

在AV1的分层编码（SVC）体系中，为不同层分配独立的 `frame_id`，而不是让所有层共用一个`frame_id`再辅以`spatial_id`和`temporal_id`的区分，其根本原因在于 **AV1通过一个统一的“依赖描述符”来管理所有帧的依赖关系，并将每一层都视为一个独立的“帧”进行处理，而不是一个“大帧”的子集**。

这主要涉及到“语义驱动”与“ID驱动”两种设计哲学的区别，并深刻影响了编解码的效率与灵活性。

### 🧐 AV1的核心设计：依赖描述符

AV1采用**依赖描述符（Dependency Descriptor，DD）** 来精确管理帧间依赖关系，这是它实现高效分层编码的基石，而`frame_id`是其中的关键。

| 特性 | 依赖描述符 (DD) | 作用 |
| :--- | :--- | :--- |
| **核心概念** | 将每个时/空层输出视为一个独立“帧”，每个帧都有自己专属的`frame_id`。 | 实现ID驱动的灵活帧引用。 |
| **依赖描述** | 通过`frame_dependencies`列表记录当前帧直接依赖的所有帧的`frame_id`。 | 建立精确的、跨层的参考关系。 |
| **帧标识 (ID驱动)** | `frame_id`作为唯一标识，`spatial_id`和`temporal_id`成为该帧的属性。 | 接收端无需解析码流，仅凭DD即可构建完整、精确的解码依赖图。 |
| **与VP9 PID的对比** | VP9的PID用于标识“图像”，同一图像的所有层**必须**共享同一个PID。 | AV1的DD打破了这种“图像”的强绑定，让每个层都成为可独立管理的一等公民。 |

> 在上一轮讨论的“何时解码基础层”问题中，解码调度器正是依赖DD提供的这个依赖图，才能精准判断一个基础层帧是否被后续的增强层帧所依赖，从而决定是等待还是立即解码。

### 🤔 对比：AV1的ID驱动 vs VP9的“语义驱动”

为了更直观地理解，我们可以将AV1的`frame_id`机制与你之前了解的VP9 SVC进行对比。VP9代表了一种更接近“语义驱动”的思路：

| 特性 | AV1 (ID驱动，依赖描述符) | VP9 (语义驱动，PID) |
| :--- | :--- | :--- |
| **核心问题** | 如何最精确地描述 **“谁依赖谁”** ？ | 如何最高效地标识 **“这是什么”** ？ |
| **处理方法** | 将每个层视为独立“帧”，用`frame_id`唯一标识，并在DD中列出其依赖的所有`frame_id`。 | 用PID标识一个“图像”（包含所有层），再用`spatial_id`/`temporal_id`说明该层在图像中的位置。 |
| **优势** | **灵活性和精确性极高**。依赖关系可以跨任意层、跨任意图像。 | **包头开销相对较小**，对于依赖关系简单的视频序列可能更高效。 |
| **劣势** | 每个层都需独立管理，依赖关系描述可能带来额外的**元数据开销**。 | **灵活性受限**。描述复杂的跨层、非对称依赖关系变得困难甚至不可能。 |

### 💡 用具体编码场景模拟

假设一段视频有2个空间层（Layer 0, Layer 1）和2个时间层（TID 0, TID 1），其中Layer 1依赖于Layer 0。

1.  **在VP9这样的“语义驱动”模型下**：
    *   编码器可能会给第N个图像的所有层（L0T0, L0T1, L1T0, L1T1）都分配同一个`PID=N`。
    *   接收端收到`PID=N`、`L1T0`的数据包后，通过“语义”知道它依赖于同一个`PID=N`下的`L0T0`。
    *   这种做法的**局限**在于，依赖关系被严格限制在了同一个PID之内，无法高效地描述跨越PID边界的依赖。例如，新的`L0T1`帧是否可以参考前一个PID的`L1T0`帧？这在“语义驱动”模型中会变得非常复杂。

2.  **在AV1的ID驱动模型下**：
    *   编码器会为这四个层分配四个独立的`frame_id`，例如：`F100`（L0T0）, `F101`（L0T1）, `F102`（L1T0）, `F103`（L1T1）。
    *   在`F103`的依赖描述符中，会明确列出它依赖于`F102`和`F101`。如果条件允许，它甚至可以声明依赖于更早的帧，比如`F95`。
    *   这种做法的**优势**在于，它为编码器提供了**极致的灵活性**，可以实现最优的压缩效率。接收端和SFU（选择性转发单元）无需理解复杂的码流语义，只需解析标准化的`frame_id`依赖列表，即可精确获知解码关系，这对于复杂的SVC场景至关重要。

### 💎 总结

总而言之，AV1选择为每个层分配独立的`frame_id`，并非无端增加复杂度，而是为了采用一种更强大、更灵活的依赖关系描述模型（依赖描述符）。**这种设计将“层级关系”从“标识符”中解放出来，通过“依赖描述符”实现了精确的“ID驱动”的依赖管理，这是AV1在支持SVC时实现更高灵活性和效率的关键所在。** 它允许SFU和中继服务器在不解析AV1码流的情况下，仅通过依赖描述符就能准确路由和转发分层流，是实现上一轮讨论中“智能降级”和“通知停发”等策略的基础。