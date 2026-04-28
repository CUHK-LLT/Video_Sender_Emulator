# Video Sender Emulator

这个项目实现了一对基于 UDP 的 `sender` / `receiver`，用于模拟“视频发送 → 接收 → 反馈 → 码率控制”的闭环，并为后续分析保留 CSV 日志。

当前代码已经不是最初的“纯自定义 UDP 负载”版本，而是在原有自定义应用头之外，加了一层最小可运行的 RTP + AV1 Dependency Descriptor 扩展头，主要用于协议验证、交换机识别和日志观测。

在此基础上，`sender` 现在已经支持一个最小可用的可选分层模型：

- `off`：默认单层行为
- `s1t3`：单 spatial layer、三 temporal layers

`s1t3` 默认按常见累计阈值拆成增量层比例 `60/20/20`，用户可以通过命令行覆盖。

## 仓库结构（你大概率只需要看这些）

- `src/`: 发送端/接收端/码率估计器（`make` 默认构建这里）
  - `src/sender.c`: 生成“帧”、分包、封装 RTP+AV1 DD、接收反馈并动态调整目标码率
  - `src/receiver.c`: 解析 RTP+AV1 DD、检测丢包、回发 RTCP 风格反馈并落盘 CSV
  - `src/rate_estimator*.c`: 码率估计器入口与多种算法实现
- `scripts/`: 离线绘图脚本（目前包含 `plot_echo_monitor.py`）

## 当前实现框架

### `sender`

`sender` 负责：

- 按 `fps` 周期生成“帧”
- 按目标比特率把每帧拆成若干固定大小 UDP 包
- 在每个包前写入：
  - RTP fixed header
  - RTP header extension
  - AV1 Dependency Descriptor extension element
  - 自定义 `stream_payload_header`
- 监听 `receiver` 返回的 RTCP 风格反馈
- 调用码率估计器，动态调整目标发送码率
- 记录逐帧发送日志 CSV

发送相关代码主要在：

- [src/sender.c](/home/llt/Video_Sender_Emulator/src/sender.c)
- [src/rate_estimator.h](/home/llt/Video_Sender_Emulator/src/rate_estimator.h)
- [src/rate_estimator.c](/home/llt/Video_Sender_Emulator/src/rate_estimator.c)
- [src/estimator_fix.c](/home/llt/Video_Sender_Emulator/src/estimator_fix.c)
- [src/estimator_naive_ewma.c](/home/llt/Video_Sender_Emulator/src/estimator_naive_ewma.c)
- [src/estimator_camel.c](/home/llt/Video_Sender_Emulator/src/estimator_camel.c)
- [src/estimator_gcc_REMB.c](/home/llt/Video_Sender_Emulator/src/estimator_gcc_REMB.c)

### `receiver`

`receiver` 负责：

- 监听 UDP 包
- 解析 RTP fixed header 和 RTP extension
- 在 RTP extension 里寻找 AV1 Dependency Descriptor
- 读取自定义 `stream_payload_header`
- 按包记录接收日志 CSV
- 检测发送序号间隙，生成丢包反馈
- 周期性或按帧边界回发 RTCP 风格反馈

接收相关代码主要在：

- [src/receiver.c](/home/llt/Video_Sender_Emulator/src/receiver.c)
- [src/proto.h](/home/llt/Video_Sender_Emulator/src/proto.h)

### 包格式

当前每个发送包的逻辑布局是：

```text
UDP payload =
  RTP fixed header
  + RTP extension header
  + RFC5285 one-byte extension element
  + AV1 Dependency Descriptor bytes
  + padding
  + stream_payload_header
  + payload fill bytes
```

其中：

- RTP / AV1 头用于协议和扩展字段验证
- `stream_payload_header` 仍然承载项目内部真实需要的字段
- 后续 payload 目前仍然是填充字节，不是真实 AV1 OBU 数据

## 构建

```bash
make
```

生成：

- `bin/sender`
- `bin/receiver`

## 运行

### 本机回环测试

终端 1：

```bash
./bin/receiver --bind-ip 127.0.0.1 --bind-port 5201 --log-file -
```

终端 2：

```bash
./bin/sender --dst-ip 127.0.0.1 --dst-port 5201 --log-file -
```

如果要强制固定码率发送：

```bash
./bin/sender --dst-ip 127.0.0.1 --dst-port 5201 --estimator fix --bitrate 10M --log-file -
```

## 默认行为

### `sender`

默认配置：

- 目的 IP：`192.168.1.10`
- 目的端口：`5201`
- 初始比特率：`10 Mbps`
- FPS：`30`
- 包大小：`1400`
- 填充模式：`zero`
- `sendmmsg` batch：`64`
- pacer：关闭
- estimator：`naive-ewma`
- AV1 scalability：`off`

注意：

- 默认不是固定码率模式
- 默认会启用反馈接收和码率估计逻辑
- 如果需要固定码率，显式指定 `--estimator fix`
- 如果需要发送端 Kalman 风格估计，指定 `--estimator gcc-remb`

### `receiver`

默认配置：

- 绑定 IP：`0.0.0.0`
- 绑定端口：`5201`
- 空闲超时：`2000 ms`
- 接收缓冲：`8 MB`

## 主要参数

### `sender`

```bash
./bin/sender --help
```

主要参数：

- `--dst-ip`
- `--dst-port`
- `--bitrate`
- `--fps`
- `--pkt-size`
- `--fill zero|random`
- `--batch`
- `--pacer on|off`
- `--pacer-window-ms`
- `--estimator fix|naive-ewma|camel|gcc-remb`
- `--av1-scalability off|s1t3`
- `--temporal-layer-ratios R0,R1,R2`
- `--debug-estimator`
- `--debug-naive-ewma-monitor`
- `--log-dir`
- `--log-file`

### `receiver`

```bash
./bin/receiver --help
```

主要参数：

- `--bind-ip`
- `--bind-port`
- `--log-dir`
- `--log-file`
- `--idle-timeout`
- `--rcvbuf`

## 日志

### 发送端 CSV

`sender` 输出逐帧日志，字段如下：

- `timestamp_us`
- `frame_id`
- `target_bitrate_bps`
- `send_this_frame`
- `planned_frame_packets`
- `sent_frame_packets`

### 接收端 CSV

`receiver` 输出逐包日志，字段如下：

- `timestamp_us`
- `ts_source`
- `frame_id`
- `packet_id`
- `frame_packet_count`
- `send_seq`
- `pkt_len`
- `src_ip`
- `src_port`
- `rtp_version`
- `rtp_ext_profile`
- `av1_sof`
- `av1_eof`
- `av1_template_id`
- `av1_tds_flag`
- `av1_spatial_id`
- `av1_temporal_id`
- `av1_frame_number`
- `av1_prefix_len`

## AV1 Header 扩展现在做到哪一步

这部分是当前 README 最重要的说明。

### 已完成

目前代码已经实现了以下内容：

1. 发送端在每个 UDP 包前封装 RTP fixed header

对应代码：

- [src/sender.c](/home/llt/Video_Sender_Emulator/src/sender.c)
- [src/proto.h](/home/llt/Video_Sender_Emulator/src/proto.h)

2. 使用 RFC5285 one-byte RTP header extension profile `0xBEDE`

3. 在 RTP extension 中放置 AV1 Dependency Descriptor 扩展元素

扩展 ID 当前固定为：

- `RTP_EXT_ID_AV1_DEPENDENCY_DESCRIPTOR = 7`

4. 已正确编码 AV1 DD 的基本固定字段

包括：

- `start_of_frame`
- `end_of_frame`
- `frame_dependency_template_id`
- `frame_number`

5. 已修正 `SOF` / `EOF` 语义

当前行为：

- 帧首包：`SOF = 1`
- 帧中包：`SOF = 0, EOF = 0`
- 帧尾包：`EOF = 1`

6. 已修正 RTP sequence number 语义

当前 RTP sequence number 使用全局 `send_seq` 的低 16 位，不再按每帧 `packet_id` 重置。

7. 已补上 RTP marker bit 语义

当前行为：

- 非帧尾包：`M = 0`
- 帧尾包：`M = 1`

8. 已发送可解析的 AV1 template dependency structure

当前仅在“发送生命周期内的首个序列首包”发送一次 `template_dependency_structure_present_flag = 1`。

当 `--av1-scalability off` 时，发送单层单模板结构。

当 `--av1-scalability s1t3` 时，发送一组固定的 `S1T3` 模板结构，后续帧根据节奏在多个 `template_id` 之间切换。

9. `sender` 已支持最小可用的 `S1T3` 分层调度

当前行为：

- temporal pattern：`T0 -> T2 -> T1 -> T2 -> ...`
- 首帧使用初始化模板
- 默认增量层比例：`60/20/20`
- 用户可通过 `--temporal-layer-ratios` 覆盖，例如 `70,20,10`

10. 接收端已经可以识别并解析 DD 的固定字段、模板结构和层信息

接收端当前可观测到：

- `SOF`
- `EOF`
- `template_id`
- `tds_flag`
- `spatial_id`
- `temporal_id`
- `frame_number`

### 还没有完成

当前实现还没有做到这些事情：

1. 还没有发送真实 AV1 RTP payload

现在 RTP 扩展后面接的仍然是：

- 自定义 `stream_payload_header`
- 填充 payload

不是：

- AV1 aggregation header
- OBU payload

因此当前实现还不是一个完整的 AV1 RTP payload sender。

2. 还没有支持更一般的 AV1 分层模型

当前只实现了一个固定的最小模型：

- `off`
- `s1t3`

还没有：

- `S2T1`
- `S2T3`
- 用户自定义模板集合
- 多 spatial layer 组合

3. `receiver` 目前只解析到本版本 sender 需要的模板信息

目前已经能还原：

- `template_id`
- `spatial_id`
- `temporal_id`

但还没有把 decode target / chain 细节全部日志化。

4. 还没有实现 AV1 OBU 级别的 fragmentation / aggregation 规则

### 当前可以怎么理解这层扩展

当前这层 AV1 header 扩展更适合被理解为：

- 一个“最小自洽”的 RTP + AV1 DD 协议外壳
- 可以用于 sender / receiver / 交换机日志观测
- 可以验证帧边界、模板初始化、marker bit、RTP sequence number 等语义
- 但还不是完整意义上的 AV1 RTP 发流器

## 分层编码状态

当前发送端已经支持一个最小的 `S1T3` 模型：

- `template_id = 0..4`
- 单 spatial layer
- 三 temporal layers
- 三 decode targets
- 固定模板依赖关系

如果后续要继续扩展到更完整的 AV1 SVC，至少还需要：

- 定义更多层模型，例如 `S2T1`、`S2T3`
- 为不同 spatial layer 建立独立模板集合
- 让模板结构、active decode target 和实际调度策略解耦
- 在 receiver 中继续补全 decode target / chain 观测

## 现阶段建议的验证方法

### 1. 检查帧边界语义

在 `receiver` 输出中确认：

- 首包：`sof=1`
- 中间包：`sof=0 eof=0`
- 尾包：`eof=1`

### 2. 检查首包模板初始化

确认首个序列首包出现：

- `tds=1`

其余包应为：

- `tds=0`

### 3. 检查 `S1T3` 层切换

如果使用：

```bash
./bin/sender --dst-ip 127.0.0.1 --dst-port 5201 --av1-scalability s1t3 --log-file -
```

则 `receiver` 输出中应能看到：

- `template_id` 在 `0/1/2/3/4` 之间切换
- `tid` 在 `0/1/2` 之间切换
- 首帧首包 `prefix` 比后续普通包更长

### 4. 检查发送序号

确认接收日志里的 `send_seq` 单调递增。

### 5. 检查固定码率发送

如果使用：

```bash
./bin/sender --dst-ip 127.0.0.1 --dst-port 5201 --estimator fix --bitrate 10M --log-file -
```

则 `sender` CSV 中 `target_bitrate_bps` 应稳定为 `10000000`。

## 已知限制

- 目前 AV1 扩展是“最小可运行验证版本”，不是完整 AV1 RTP 实现
- 当前只支持 `off` 和 `s1t3` 两种 sender 层模型
- 发送负载仍为自定义头 + 填充字节，不是真实编码视频
- 默认 `README` 中的回环测试依赖本机允许 UDP socket 创建

## 绘图与离线分析（可选）

仓库提供了一个绘图脚本 `scripts/plot_echo_monitor.py`，用于读取一个名为 `echo_monitor.csv` 的 CSV 并输出 `plots/rate_and_queue.png`。

当前脚本期望 CSV 至少包含这些列：

- `timestamp_ns`
- `avg_rate_bps`
- `target_bps`
- `ingress`
- `egress`

运行示例：

```bash
python3 scripts/plot_echo_monitor.py --input echo_monitor.csv --outdir plots
```

提示：这个 `echo_monitor.csv` 通常需要你把“应用侧的目标/估计速率”与“eBPF 侧的 ingress/egress 计数”做一次对齐/合并后生成；仓库内目前只提供绘图脚本本身。

## 当前遗留问题

下面这些问题是当前代码仍然保留的主要工程欠账，后续继续优化时建议优先按这个列表推进。

### 1. AV1 payload 仍然不是真实码流

当前 UDP payload 中虽然已经带有：

- RTP fixed header
- RTP extension
- AV1 Dependency Descriptor

但真正的媒体负载仍然只是：

- `stream_payload_header`
- 填充字节

这意味着当前工程仍然是“带 AV1 DD 外壳的视频发送模拟器”，不是完整 AV1 RTP sender。

### 2. 分层模型还是最小实现

当前 sender 只支持：

- `off`
- `s1t3`

并且 `s1t3` 目前仍然是固定模板、固定帧节奏、固定 decode target 关系的最小版本。

还没有做的事情包括：

- `S2T1`
- `S2T3`
- 更多可配置 layer pattern
- 用户自定义模板集合
- 多 spatial layer 调度

### 3. sender 的层调度策略还比较粗

目前 `s1t3` 的发送节奏是固定的：

- `T0 -> T2 -> T1 -> T2 -> ...`

码率分配也是按增量层比例把包预算累积到各层。

这适合最小验证，但还没有做到：

- 基于实际反馈动态调整各层占比
- 单独控制 base / enhancement 的保底或上限
- 在拥塞时优先保基础层、抑制增强层的更细策略

### 4. receiver 对 DD 的解析仍然是“够当前 sender 用”

现在 receiver 已经可以从模板结构中恢复：

- `template_id`
- `spatial_id`
- `temporal_id`

但仍然没有把下面这些信息完整展开到日志里：

- decode target 细节
- chain 细节
- 更通用的 active decode targets 状态

也就是说，receiver 现在更偏向“验证 sender 当前输出是否正确”，还不是完整的 DD 观测器。

### 5. 还没有做 AV1 OBU 级别的 fragmentation / aggregation

目前没有实现真正 AV1 RTP payload 所需的：

- aggregation header
- OBU 边界处理
- OBU fragmentation / aggregation

后续如果要接近真实编码器输出，这部分是必补项。

### 6. 交换机 /中间节点联动还停留在协议验证阶段

当前 DD 扩展已经足够拿来做：

- 帧边界验证
- template 初始化验证
- layer id 可见性验证

但如果目标变成：

- 基于 decode target 的智能转发
- 基于链路状态的层裁剪
- 更真实的中间节点自适应

那还需要把 sender、receiver、以及中间节点对 DD 的理解继续补全。
