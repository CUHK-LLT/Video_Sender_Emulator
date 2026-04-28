# Video Sender Emulator

这个项目实现了一对基于 UDP 的 `sender` / `receiver`，用于模拟：

- 视频发送
- RTP + AV1 Dependency Descriptor 封装
- 接收端逐包观测
- RTCP 风格接收反馈
- 发送端码率控制
- 分层播放统计与离线分析

当前实现已经不是最初的“纯自定义 UDP 负载”版本。现在每个发送包都会带上：

- RTP fixed header
- RTP header extension
- RFC5285 one字节扩展元素
- AV1 Dependency Descriptor
- 自定义 `stream_payload_header`

其中 AV1 相关头主要用于协议验证、分层语义表达、交换机/SFU 识别和日志分析；负载本身仍然是填充字节，不是真实 AV1 OBU。

## 当前支持的可扩展模式

`sender` 当前支持三种 `--av1-scalability` 模式：

- `off`
  单层模式。每个发送周期只产生一个 frame。
- `s1t3`
  单空间层、三时间层。默认增量层比例为 `60/20/20`，可通过 `--temporal-layer-ratios` 覆盖。
- `l2t1`
  双空间层、单时间层。每个 temporal unit 会生成两个独立 frame：
  - `S0` 基础层
  - `S1` 增强层

`l2t1` 下：

- 同一时刻的 S0 / S1 共享同一个 RTP timestamp
- 两层使用独立的 AV1 DD `frame_number`
- 发送预算通过 `--spatial-layer-ratios R0,R1` 在 S0 / S1 之间分配
- 接收端会按 deadline 选择可播放层并输出 `playback_*.csv`

更完整的 `l2t1` 说明见 [`README_L2T1.md`](/home/llt/Video_Sender_Emulator/README_L2T1.md)。

## 仓库结构

- `src/`
  - [`src/sender.c`](/home/llt/Video_Sender_Emulator/src/sender.c): 生成帧、封装 RTP+AV1 DD、接收反馈并动态调整目标码率
  - [`src/receiver.c`](/home/llt/Video_Sender_Emulator/src/receiver.c): 解析 RTP+AV1 DD、检测丢包、回发反馈、输出接收与播放日志
  - [`src/rate_estimator.c`](/home/llt/Video_Sender_Emulator/src/rate_estimator.c): 码率估计器统一入口
  - `src/estimator_*.c`: 各种估计器实现
  - [`src/proto.h`](/home/llt/Video_Sender_Emulator/src/proto.h): RTP / RTCP / 自定义头定义
- `scripts/`
  - [`scripts/analyze_l2t1_logs.py`](/home/llt/Video_Sender_Emulator/scripts/analyze_l2t1_logs.py): 联合分析 `send/recv/playback` 三类日志
  - [`scripts/plot_echo_monitor.py`](/home/llt/Video_Sender_Emulator/scripts/plot_echo_monitor.py): 其他离线绘图脚本
- `plots/`: 示例图
- `testlogs/`: 本地实验日志目录（默认已忽略，不提交）

## 当前实现框架

### sender

`sender` 负责：

- 按 `fps` 周期生成发送时刻
- 根据目标码率计算该时刻的发包预算
- 在包前写入 RTP + AV1 DD + `stream_payload_header`
- 接收 `receiver` 返回的 RTCP 风格反馈
- 调用码率估计器动态调整 `target_bitrate_bps`
- 输出逐时刻发送日志 CSV

`l2t1` 模式下，单个发送时刻会拆成 S0 和 S1 两个独立 frame；日志仍然按 temporal unit 记一行：

- `planned_frame_packets` 表示该时刻总预算
- `sent_frame_packets` 表示 S0 + S1 实际发出的总包数

目前 sender 侧已经具备：

- `fix`
- `naive-ewma`
- `camel`
- `gcc-remb`

四种估计算法入口。

代码入口：

- [`src/sender.c`](/home/llt/Video_Sender_Emulator/src/sender.c)
- [`src/rate_estimator.h`](/home/llt/Video_Sender_Emulator/src/rate_estimator.h)
- [`src/rate_estimator.c`](/home/llt/Video_Sender_Emulator/src/rate_estimator.c)

### receiver

`receiver` 负责：

- 监听 UDP 包
- 解析 RTP fixed header、RTP extension、AV1 DD
- 读取自定义 `stream_payload_header`
- 按包输出接收日志 CSV
- 根据 `send_seq` 间隙检测丢包
- 返回 RTCP 风格接收反馈
- 在 `l2t1` 语义下做 deadline 播放决策并输出播放日志

当前 receiver 的 playback 逻辑会按 temporal unit 聚合 S0 / S1：

- 若 S1 完整且 S0 也完整，则播放 S1
- 否则若 S0 完整，则回落到 S0
- 否则记为 stall

代码入口：

- [`src/receiver.c`](/home/llt/Video_Sender_Emulator/src/receiver.c)
- [`src/proto.h`](/home/llt/Video_Sender_Emulator/src/proto.h)

## 包格式

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

- RTP / AV1 DD 用于表达分层和依赖语义
- `stream_payload_header` 继续承载项目内部使用的关键字段
- 末尾 payload 仍然是测试填充数据

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
./bin/receiver --bind-ip 127.0.0.1 --bind-port 5201 --log-dir ./testlogs/run
```

终端 2：

```bash
./bin/sender --dst-ip 127.0.0.1 --dst-port 5201 --log-dir ./testlogs/run
```

### 固定码率单层模式

```bash
./bin/sender \
  --dst-ip 127.0.0.1 --dst-port 5201 \
  --estimator fix \
  --bitrate 10M \
  --log-dir ./testlogs/fix_run
```

### `s1t3` 示例

```bash
./bin/sender \
  --dst-ip 127.0.0.1 --dst-port 5201 \
  --av1-scalability s1t3 \
  --temporal-layer-ratios 60,20,20 \
  --bitrate 8M --fps 30 \
  --log-dir ./testlogs/s1t3_run
```

### `l2t1` 示例

终端 1：

```bash
./bin/receiver \
  --bind-ip 127.0.0.1 --bind-port 5201 \
  --log-dir ./testlogs/l2t1_run \
  --deadline-ms 100 \
  --playout-fps 30
```

终端 2：

```bash
./bin/sender \
  --dst-ip 127.0.0.1 --dst-port 5201 \
  --av1-scalability l2t1 \
  --spatial-layer-ratios 1,1 \
  --bitrate 5M --fps 30 --pkt-size 1200 \
  --pacer off \
  --log-dir ./testlogs/l2t1_run
```

## 默认行为

### sender 默认值

- `--dst-ip 192.168.1.10`
- `--dst-port 5201`
- `--bitrate 10M`
- `--fps 30`
- `--pkt-size 1400`
- `--fill zero`
- `--batch 64`
- `--pacer off`
- `--pacer-window-ms 5`
- `--estimator naive-ewma`
- `--av1-scalability off`
- `--temporal-layer-ratios 60,20,20`
- `--spatial-layer-ratios 50,50`
- `--log-dir .`

说明：

- 默认不是固定码率模式
- 默认会启用反馈接收和估计器闭环
- 如果要固定码率，显式指定 `--estimator fix`
- 当前 sender 内部有丢包上报与重传缓存逻辑，但默认 `retransmit` 仍关闭

### receiver 默认值

- `--bind-ip 0.0.0.0`
- `--bind-port 5201`
- `--log-dir .`
- `--idle-timeout 2000`
- `--rcvbuf 8388608`
- `--deadline-ms 100`
- `--playout-fps 30`

## 主要参数

### sender

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
- `--av1-scalability off|s1t3|l2t1`
- `--temporal-layer-ratios R0,R1,R2`
- `--spatial-layer-ratios R0,R1`
- `--debug-estimator`
- `--debug-naive-ewma-monitor`
- `--log-dir`
- `--log-file`

说明：

- `--temporal-layer-ratios` 仅对 `s1t3` 生效
- `--spatial-layer-ratios` 仅对 `l2t1` 生效
- `--log-file -` 会把发送日志输出到 stdout

### receiver

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
- `--deadline-ms`
- `--playout-fps`

说明：

- 未指定 `--log-file` 时，会自动生成 `recv_<sec>.csv` 和 `playback_<sec>.csv`
- 指定 `--log-file <path>` 时：
  - 接收逐包日志写到 `<path>`
  - playback 日志写到 `<path>.playback.csv`
- 指定 `--log-file -` 时，两类 CSV 都会输出到 stdout

## 日志

### sender CSV

`sender` 输出逐时刻日志：

- 文件名：`send_<sec>.csv`
- 表头：
  - `timestamp_us`
  - `frame_id`
  - `target_bitrate_bps`
  - `send_this_frame`
  - `planned_frame_packets`
  - `sent_frame_packets`

说明：

- 这里的 `frame_id` 实际上对应发送主循环里的 temporal unit 序号
- 在 `l2t1` 模式下，一行会汇总该时刻 S0 / S1 两层

### receiver 逐包 CSV

`receiver` 输出逐包接收日志：

- 文件名：`recv_<sec>.csv`
- 表头：
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

### receiver playback CSV

`receiver` 还会输出逐 temporal unit 的播放统计：

- 文件名：`playback_<sec>.csv`
- 表头：
  - `deadline_us`
  - `temporal_unit_id`
  - `chosen_sid`
  - `stall_us`
  - `decodable_bytes`
  - `total_bytes`
  - `quality`

其中：

- `chosen_sid = 1` 表示播放 S1
- `chosen_sid = 0` 表示回落到 S0
- `chosen_sid = -1` 表示 stall

## L2T1 分析脚本

项目提供 [`scripts/analyze_l2t1_logs.py`](/home/llt/Video_Sender_Emulator/scripts/analyze_l2t1_logs.py) 用于联合分析：

- `send_*.csv`
- `recv_*.csv`
- `playback_*.csv`

示例：

```bash
python3 scripts/analyze_l2t1_logs.py \
  --send-csv ./testlogs/l2t1_run/send_<sec>.csv \
  --recv-csv ./testlogs/l2t1_run/recv_<sec>.csv \
  --playback-csv ./testlogs/l2t1_run/playback_<sec>.csv \
  --spatial-ratios 1,1 \
  --pkt-size 1200 \
  --denom sent \
  --window-ms 1000 \
  --join-key send_seq_tu \
  --top-n 8
```

脚本会输出可播放率、stall、层切换、deadline 裕量、接收 goodput、乱序情况等统计指标，适合拿来比较不同 estimator 或不同分层配置。

## 相关文档

- [`README_L2T1.md`](/home/llt/Video_Sender_Emulator/README_L2T1.md): `l2t1` 模式的详细语义、运行方式和分析说明
