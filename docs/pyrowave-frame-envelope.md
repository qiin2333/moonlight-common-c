# PyroWave Frame Envelope 视频传输协议

## 范围

本文定义 `PyrowaveProtocol.h`、`PyrowaveProtocol.c` 和 `PyrowaveReassembly.c` 使用的
视频传输封套。它封装 PyroWave 原生码流，不修改原生小波编码块或序列头格式。

原生码流负责颜色合同；Frame Envelope 负责帧标识、长度、分片、可选 metadata 和块级恢复。
`SS_HDR_METADATA` 继续使用现有控制通道，不塞入 Frame Envelope 或原生 color metadata。
H.264、HEVC 和 AV1 的传输路径不使用本封套。

本文描述当前唯一实现的合同，不定义旧实验封套的迁移或多版本兼容分支。

## 1. 协商

客户端只有在应用和解码器均支持 PyroWave 时才声明 `VIDEO_FORMAT_PYROWAVE`。
服务端 DESCRIBE 使用 `x-ss-pyrowave.*`，客户端 ANNOUNCE 使用 `x-ml-pyrowave.*`：

| 属性 | 当前值或含义 |
|---|---|
| `protocolVersion` | `2` |
| `bitstreamVersion` | `2` |
| `payloadVersion` | `3` |
| `capabilityFlags` | 支持能力的位图 |
| `maxPacketSize` | 完整内层包的最大字节数，包括 64 字节头部 |

版本必须与双方和本地编译期合同完全一致，不能只比较双方是否互相相等。
版本字段在收窄到 `uint16_t` 前必须验证范围；`maxPacketSize` 范围为 65 至 65536 字节，
协商结果使用双方上限的较小值。该上限不是网络 MTU；实际包长仍受外层 RTP 包预算约束。

当前所有模式都要求 `REASSEMBLY`、`FRAME_DEADLINE`、`BLOCK_AWARE_FEC` 和 `FRAME_METADATA`，
再加上所选信号模式和范围的能力：

| `dynamicRangeMode` | 信号 | 位深 | 模式能力 |
|---:|---|---:|---|
| `0` | SDR / BT.709 / YUV420 | 8-bit | `SDR_BT709_YUV420` |
| `1` | HDR10/PQ / BT.2020 / YUV420 | 10-bit | `HDR10_PQ_BT2020` |
| `2` | HLG / BT.2020 / YUV420 | 10-bit | `HLG_BT2020` |

范围能力为 `YUV_LIMITED_RANGE` 或 `YUV_FULL_RANGE`；所选范围通过现有 `encoderCscMode`
低位传递，不改变用户的全局范围偏好。能力交集不包含所需位时拒绝 PyroWave 协商。
`PARTIAL_FRAME` 位不是当前必需能力，不代表当前客户端会提交不完整帧。

## 2. 包布局

```text
外层 RTP / NV_VIDEO_PACKET
└─ 64-byte PYRF header
   └─ payloadLength bytes
```

所有多字节整数采用大端字节序。下面的偏移从内层包起始位置计算：

| 偏移 | 字节数 | 字段 | 含义 |
|---:|---:|---|---|
| 0 | 4 | `magic` | ASCII `PYRF` |
| 4 | 1 | `version` | `protocolVersion`，当前为 2 |
| 5 | 1 | `packetKind` | 0=`FRAME_HEADER`，1=`DATA`，2=`PARITY` |
| 6 | 1 | `flags` | 包标记，见下表 |
| 7 | 1 | `reserved` | 必须为 0 |
| 8 | 2 | `headerLength` | 固定为 64 |
| 10 | 2 | `metadataFlags` | 帧级 metadata 标记 |
| 12 | 4 | `frameId` | 非零帧标识 |
| 16 | 4 | `rtpTimestamp` | 90 kHz 媒体时间戳 |
| 20 | 4 | `codecPayloadLength` | 纯 PyroWave 码流字节数，不含 metadata 或填充 |
| 24 | 4 | `protectedPayloadLength` | metadata 与码流的总长度，不含填充 |
| 28 | 2 | `metadataLength` | 受保护 payload 开头的 metadata 字节数 |
| 30 | 1 | `fecScheme` | 0=无 FEC，1=XOR |
| 31 | 1 | `reserved2` | 必须为 0 |
| 32 | 2 | `dataBlockCount` | 数据块总数 |
| 34 | 2 | `parityBlockCount` | 校验块总数 |
| 36 | 2 | `fecBlockPayloadSize` | 数据/校验块的 payload 容量 |
| 38 | 2 | `reserved3` | 必须为 0 |
| 40 | 2 | `blockIndex` | DATA/PARITY 全局块索引 |
| 42 | 2 | `blockCount` | DATA 和 PARITY 数量之和，不计 FRAME_HEADER |
| 44 | 2 | `fecGroupIndex` | FEC 组索引 |
| 46 | 1 | `fecDataCount` | 当前组的数据块数 |
| 47 | 1 | `fecParityCount` | 当前组的校验块数 |
| 48 | 1 | `fecShardIndex` | 当前组内的 shard 索引 |
| 49 | 1 | `reserved4` | 必须为 0 |
| 50 | 2 | `payloadLength` | 当前包体的实际字节数 |
| 52 | 4 | `reserved5` | 必须为 0 |
| 56 | 8 | `reserved6` | 必须全部为 0 |

`LI_PYROWAVE_PACKET_HEADER` 是主机字节序的解析结果，不是可直接复制到网络的 packed struct。
使用 `LiPyrowaveBuildPacket()` 和 `LiPyrowaveParsePacket()` 序列化、解析，不依赖 C struct
填充或主机字节序。

| `flags` 位 | 名称 | 含义 |
|---:|---|---|
| `0x01` | `START_OF_FRAME` | 帧头包或首个 DATA 包 |
| `0x02` | `END_OF_FRAME` | 最后一个 DATA 包 |
| `0x04` | `CRITICAL` | 关键包提示，不改变当前 XOR 恢复规则 |
| `0x08` | `FEC_PARITY` | PARITY 包 |
| `0x10` | `METADATA_PRESENT` | 该帧有 metadata |

未知 flags 位、非零保留字段、空码流、零帧号、零数据块、矛盾的长度或块计数均为非法头部。
完整包长度必须恰好为 `headerLength + payloadLength`，不能隐含拼接额外数据。

每个内层包保持与一个外层 RTP payload 对齐。PyroWave 不插入传统 codec 的 short frame header，
否则丢失一个外层包可能同时破坏两个内层恢复块。

### 包类型

- `FRAME_HEADER`：携带本帧恢复描述；`blockIndex=0`，设置 SOF，不设置 EOF/PARITY；
  组/shard 字段全部为 0，包体长度等于 `fecBlockPayloadSize`。它不计入数据/校验块数量，
  其包体不参与受保护数据拼接。
- `DATA`：索引为 0 至 `dataBlockCount-1`。首块设置 SOF，末块设置 EOF。
- `PARITY`：设置 PARITY，不设置 SOF/EOF；索引从 `dataBlockCount` 开始。

三种包重复相同的帧号、时间戳、长度、metadata 描述和全帧恢复几何。
接收端不能把同一帧中互相矛盾的描述拼接为一个结果。丢失独立 FRAME_HEADER 包不阻止
接收端根据 DATA/PARITY 的重复描述开始重组。

## 3. 受保护数据与 Metadata TLV

```text
[metadataLength bytes of TLVs][codecPayloadLength bytes of PyroWave bitstream]
```

必须满足：

```text
protectedPayloadLength = metadataLength + codecPayloadLength
```

metadata 长度最多 65535 字节。每个 TLV 使用大端字段：

```text
type:uint16 | flags:uint16 | length:uint32 | value:length
```

| metadata 标记 | 值 | 含义 |
|---|---:|---|
| `PROTECTED` | `0x0001` | 与码流一起参与块恢复 |
| `RUNTIME` | `0x0002` | 帧级运行信息 |
| `OPTIONAL` | `0x0004` | 不支持时可跳过 |
| `REQUIRED` | `0x0008` | 不支持时必须丢弃该帧 |

同一组标记不能同时包含 OPTIONAL 和 REQUIRED。头部的 `metadataFlags` 描述整个 metadata
区，各 TLV 的 flags 描述对应条目；接收端分别校验二者，不用其中一层代替另一层。

- `metadataLength > 0` 时，包 flags 必须设置 METADATA_PRESENT，头部 metadataFlags 必须包含 PROTECTED。
- `metadataLength == 0` 时，不能设置 METADATA_PRESENT，且 metadataFlags 必须为 0。
- 未知 TLV 在条目或头部 metadataFlags 标记 REQUIRED 时拒绝该帧；否则跳过该条目。
  OPTIONAL 允许未知条目被忽略，不表示接收端实现了该类型。
- 截断的 TLV 头、超出剩余缓冲区的长度、非法 flags、已知类型的非法长度均拒绝该帧。

| 类型 | 值 | 当前解释 |
|---|---:|---|
| `HDR10_PLUS` | `0x0001` | 预留；当前不产生或呈现动态 HDR10+ |
| `HDR_STATIC_SNAPSHOT` | `0x0002` | 预留；静态 HDR 使用现有控制通道 |
| `COLOR_CONTRACT` | `0x0003` | 预留；颜色合同使用原生码流序列头 |
| `HOST_PROCESSING_LATENCY` | `0x0100` | 已实现，见下文 |
| `FRAME_DEADLINE` | `0x0101` | 预留；当前没有 wire deadline 字段 |
| `TRANSPORT_STATUS` | `0x0102` | 预留 |

`HOST_PROCESSING_LATENCY` 的 value 是一个大端 `uint16_t`，length 必须为 2，单位为 0.1 ms。
发送端以 `PROTECTED | RUNTIME | OPTIONAL` 标记 TLV 和对应的帧级 metadata。转换时饱和到
0 至 65535，不回绕。没有采集时间戳的重复帧可以不携带该条目；客户端清零当前帧的
host processing latency，不能沿用上一帧的值。

## 4. 块级 FEC

令 `P=protectedPayloadLength`、`B=fecBlockPayloadSize`、`N=dataBlockCount`：

```text
N = ceil(P / B)
(N - 1) * B < P <= N * B
```

`B` 非零且不大于 65472 字节。`N` 和 `blockCount` 非零且不大于 65535。
接收端还应限制可分配的总帧大小；当前客户端重组上限为 16 MiB，不按未校验字段分配内存。

### XOR

当前每组最多 16 个 DATA shard，每组恰好一个 XOR PARITY shard：

```text
G = ceil(N / 16)
parityBlockCount = G
blockCount = N + G
```

对于组 `g`，DATA 数量为 `D=min(16, N-16*g)`。DATA 的 `fecGroupIndex=floor(blockIndex/16)`，
`fecShardIndex=blockIndex%16`；PARITY 的 `blockIndex=N+g`、`fecShardIndex=D`。
两者均使用 `fecDataCount=D`、`fecParityCount=1`。

所有 XOR DATA/PARITY 的包体长度为 B。最后一个 DATA 块不足 B 时零填充；PARITY 是当前组
完整 B 字节 DATA shard 的按字节 XOR。metadata 与码流按同一规则恢复，而不是仅保护码流。

每组最多恢复一个缺失 DATA shard；缺两个及以上不能由一个 PARITY shard恢复。
恢复完成后按 P 去掉尾部填充，提取 metadata，再只把 codecPayloadLength 字节交给解码器。
PyroWave 不使用传统 RTP Reed-Solomon parity；应用传输层不能把两种恢复合同混为一套。

### 无 FEC

`fecScheme=0` 时，parityBlockCount 为 0，dataBlockCount 等于 blockCount，所有组/shard
字段为 0。除最后一个 DATA 块外，DATA 包体长度等于 B；末块可以较短，但必须包含剩余的
受保护数据。该布局可供协议工具使用，不替代当前会话协商要求的 BLOCK_AWARE_FEC 能力。

## 5. 重组、时限与失败

重组器接受乱序和重复包，不让重复包延长帧时限。同一帧的冲突描述、超大帧、非法包或
内存失败不会产出 codec bytes。较新帧到来时丢弃旧的不完整帧；停止或重建时清空重组状态。

帧时限由接收端的本地时钟确定，不把 90 kHz RTP 时间戳直接当作主机/客户端共有的墙钟。
当前 common-c 视频路径使用首次收到该帧包的时间加 100 ms；超时仍不完整的帧丢弃，
不阻塞后续帧，也不把未恢复的填充或 metadata 当作图像数据提交。

必须先确认分包返回成功，再发布完整视频帧。错误返回不是空视频帧；发送端不得把空 payload
交给 FEC 分块或 pacing。协议错误影响当前视频帧或视频会话，不要求退出宿主进程。

## 6. HDR 与其他通道

SDR 使用 BT.709，HDR10/PQ 与 HLG 使用 BT.2020，均支持协商的 limited/full YUV 范围。
这些颜色信息来自原生 PyroWave color metadata，不等同于完整显示 mastering metadata。

RGB primaries、white point、显示亮度、MaxCLL、MaxFALL 和 full-frame luminance 使用
现有 `SS_HDR_METADATA` 控制消息，由客户端呈现层处理。不通过新增 TLV 复制另一份合同。
预留 HDR10_PLUS 类型不是动态 HDR10+、HDR Vivid 或 Dolby Vision 支持声明。
音频、麦克风、输入、手柄、USB 和剪贴板协议不由本视频封套修改。

## 7. API 错误码

| 结果 | 值 | 含义 |
|---|---:|---|
| `LI_PYROWAVE_PACKET_OK` | `0` | 包构建或解析成功 |
| `INVALID_ARGUMENT` | `-1` | 参数或传入的 payload 长度不一致 |
| `TRUNCATED` | `-2` | 包或其声明的包体尚未完整 |
| `BAD_MAGIC` | `-3` | 不匹配 PYRF |
| `UNSUPPORTED_VERSION` | `-4` | 不支持的协议版本 |
| `INVALID_HEADER` | `-5` | 头部字段或恢复描述不合法 |
| `OVERSIZE` | `-6` | 超出完整包上限或输出容量不足 |

错误码表示失败，不保证只有一种字段组合会触发同一错误码。

## English summary

This document specifies the current PYRF transport envelope, not the native PyroWave bitstream.
The negotiated contract is protocol 2, bitstream 2, payload 3. Each packet has a fixed 64-byte,
big-endian header with explicit frame, length, metadata, and FEC fields. C structures are parsed
host-endian values and must not be copied directly onto the wire.

Protected payload is `[metadata TLVs][codec bytes]`. Nonempty metadata requires METADATA_PRESENT
and a PROTECTED envelope flag; absent metadata requires zero metadata flags. HOST_PROCESSING_LATENCY
is an optional, protected, big-endian uint16 value in 0.1 ms units. Unknown optional entries are
skipped; malformed or unsupported required entries drop the frame.

XOR FEC protects groups of at most 16 data shards with one parity shard. Receivers recover missing
data before separating metadata, codec bytes, and zero padding. Frame headers are not counted as
data/parity shards. Native color metadata and SS_HDR_METADATA remain separate; reserved dynamic-HDR
types are not an implementation claim. Legacy video codecs and non-video channels are unchanged.

## 实现入口

- [`src/PyrowaveProtocol.h`](../src/PyrowaveProtocol.h)：常量、能力和主机字节序字段。
- [`src/PyrowaveProtocol.c`](../src/PyrowaveProtocol.c)：头部校验、构建、解析和协商。
- [`src/PyrowaveReassembly.c`](../src/PyrowaveReassembly.c)：有界重组和 XOR 恢复。
- [`src/VideoDepacketizer.c`](../src/VideoDepacketizer.c)：metadata 消费和 codec bytes 提交。
- [`tests/PyrowaveProtocolGoldenTest.c`](../tests/PyrowaveProtocolGoldenTest.c)：固定 wire 字段测试。
- [`tests/PyrowaveReassemblyGoldenTest.c`](../tests/PyrowaveReassemblyGoldenTest.c)：重组边界测试。
