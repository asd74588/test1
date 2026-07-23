# MQTT 下行升级方案讨论

> 文档状态：第一阶段协议层代码已落地，ThingsBoard 业务接入待继续。
>
> 当前代码已经完成 ESP8266 `+IPD` 数据与 AT 文本分流、MCU 侧 MQTT 增量收包、ACK 等待上下文和 PUBLISH 队列。旧的 ESP-AT MQTT/ThingsBoard 路径仍由 `ESP8266_MQTT_BACKEND_AT_ENABLE` 条件编译开关保留；ThingsBoard 属性响应和固件块业务尚未切换到新队列接口。

## 1. 改造背景

项目最初通过 ESP32-C3 AT 固件自带的 MQTT 指令完成 ThingsBoard 通信。为了让 STM32 自己掌握 MQTT 协议处理过程，目前正在将架构调整为：

```text
ESP32-C3：负责 WiFi 和 TCP
STM32：负责 MQTT 协议和 ThingsBoard 业务
```

底层已经形成以下接收链路：

```text
UART RX 中断
-> s_rx_ring
-> esp_at_process()
   |- AT 文本       -> 命令解析 / g_wifi_rxbuf
   `- +IPD payload -> s_tcp_rx_ring
-> esp8266_sock_recv()
-> transport_getdata()
-> MQTTPacket_readnb()
```

这里有三个关键边界：

1. `esp8266` 层只负责从串口混合流中提取纯 TCP payload。
2. `transport` 层把纯 TCP 字节流适配成 MQTT 库需要的读取回调。
3. `core_mqtt` 层负责识别 MQTT 报文，不再依赖 ESP AT MQTT 指令。

`g_wifi_rxbuf` 只保存 AT 命令回复和调试文本，不应再作为 socket 接收缓存使用。TCP payload 只能从 `s_tcp_rx_ring` 进入上层，从根本上避免 AT 文本、`OK`、`CLOSED` 等内容污染 MQTT 数据。

## 2. 改造前 `core_mqtt` 的工作方式

以下内容记录改造前的同步请求模型，便于对照新实现：

```text
mqtt_connect()
-> 发送 CONNECT
-> mqtt_wait_packet_type(CONNACK)

mqtt_subscribe_topic()
-> 发送 SUBSCRIBE
-> mqtt_wait_packet_type(SUBACK)

mqtt_unsubscribe_topic()
-> 发送 UNSUBSCRIBE
-> mqtt_wait_packet_type(UNSUBACK)
```

`mqtt_wait_packet_type()` 的核心逻辑是不断调用：

```c
rv = MQTTPacket_read(buf, buflen, transport_getdata);
if (rv == expect_type)
{
    return rv;
}
```

这种实现适合验证 `CONNECT -> CONNACK`、`SUBSCRIBE -> SUBACK` 等最小链路，但它默认“收到的下一包就是当前函数想要的包”。

### 2.1 异步 PUBLISH 被误消费

假设设备正在等待 `SUBACK`，Broker 此时先下发了一条 `PUBLISH`：

```text
STM32                         Broker
  |---- SUBSCRIBE ------------>|
  |<--- PUBLISH ---------------|  属性更新或固件响应
  |<--- SUBACK ----------------|
```

当前 `mqtt_wait_packet_type(SUBACK)` 会先读走 `PUBLISH`。由于返回类型不等于 `SUBACK`，它只会继续等待，并不会保存该 `PUBLISH`。从 TCP 和 MQTT 解析角度看报文已经被消费，从业务角度看消息则已经丢失。

因此，当前问题不是“库里没有 PUBLISH 解析函数”，而是：

```text
接收入口分散在各个同步 API 中，
没有统一分发机制保存当前请求之外的合法 MQTT 报文。
```

### 2.2 当前实现的其他限制

1. `mqtt_publish()` 的 QoS1 发送尚未等待和匹配 `PUBACK`。
2. QoS1 上行当前固定使用 `packet_id = 1`，后续无法可靠区分多次发布。
3. 当前 `CORE_MQTT_BUF_SIZE` 为 256 字节，不能容纳 512 字节固件 payload 加 topic 和 MQTT 头部后的完整报文。
4. 当前每个同步函数在栈上创建报文缓冲，后续扩大缓冲会增加主线程栈压力。
5. `MQTTPacket_read()` 是整包同步读取函数，超时期间若只消费了半包，需要额外考虑后续报文重新同步问题。

## 3. ThingsBoard 下行为什么要求统一收包

ThingsBoard 的主要下行业务都通过 MQTT `PUBLISH` 发送：

| 业务 | 设备订阅的 topic | Broker 实际下发方式 |
| --- | --- | --- |
| 共享属性更新 | `v1/devices/me/attributes` | `PUBLISH` |
| 属性请求响应 | `v1/devices/me/attributes/response/+` | `PUBLISH` |
| 固件分块响应 | `v2/fw/response/+/chunk/+` | `PUBLISH` |

设备向请求 topic 发布消息后，会出现两类不同含义的返回：

1. `PUBACK`：仅表示 Broker 已收到本次 QoS1 发布。
2. 下行 `PUBLISH`：才是 ThingsBoard 对属性请求或固件块请求给出的业务响应。

二者不能混为一谈。例如固件块请求成功收到 `PUBACK`，不代表固件块已经到达；设备仍需等待对应响应 topic 的 `PUBLISH`。

## 4. 目标架构与职责划分

目标是把 `core_mqtt` 从“同步请求封装”升级为“统一收包和分发的 MQTT 会话层”：

```text
transport_getdata()
-> MQTTPacket_readnb()
-> mqtt_poll_once()
-> mqtt_dispatch_packet()
   |- ACK 类报文     -> 当前等待上下文
   |- PUBLISH 报文   -> 下行消息队列
   `- 连接控制报文   -> 会话状态
```

各层职责如下：

| 层次 | 负责内容 | 不负责内容 |
| --- | --- | --- |
| ESP8266 驱动层 | WiFi、TCP、`+IPD` 提取 | MQTT 报文和 ThingsBoard topic |
| transport 层 | socket 风格收发、TCP 字节缓存 | MQTT 包类型判断 |
| `core_mqtt` 层 | MQTT 编解码、ACK 关联、PUBLISH 队列、心跳 | 固件版本和 OTA 状态决策 |
| ThingsBoard 业务层 | topic 组织、requestId、属性及固件响应解析 | UART 与 AT 混合流解析 |
| OTA 状态机 | 下载、校验、安装和启动决策 | MQTT 报文细节 |

升级后只有 `mqtt_poll_once()` 可以调用 `MQTTPacket_readnb()` 读取下行报文。同步 API 也必须通过它等待结果，不能再直接从 transport 抢包。

## 5. 第一版并发约束：控制请求串行化

第一版采用以下约束：

```text
同一时刻只允许一个需要 ACK 的 MQTT 控制请求处于等待状态。
```

例如设备需要订阅两个 topic 时，按下面的顺序执行：

```text
SUBSCRIBE(topic_A, msgid=1)
-> 等待并校验 SUBACK(msgid=1)
-> 成功后再继续

SUBSCRIBE(topic_B, msgid=2)
-> 等待并校验 SUBACK(msgid=2)
```

这样即使第一个订阅成功、第二个订阅超时，也能明确知道失败的是 `topic_B`。第一版不在一个 `SUBSCRIBE` 报文中携带多个 topic，也不同时挂起多个订阅请求。

串行化只限制需要匹配 ACK 的控制请求，不会阻止异步下行：等待 `SUBACK` 期间收到的 `PUBLISH` 仍由统一收包器解析，并进入独立消息队列。

该策略适合当前项目，因为：

1. ThingsBoard OTA 流程本身按连接、订阅、请求属性、逐块下载依次执行。
2. 第一版无需引入 pending request table。
3. ACK 关联关系清晰，调试成本较低。

未来确实需要同时挂起多个请求时，再将单一等待上下文升级为按 `packet_id` 索引的等待表。

## 6. 当前等待上下文设计

等待上下文用于记录“当前同步 API 正在等待什么”，建议结构如下：

```c
typedef struct
{
    uint8_t        active;
    uint8_t        done;
    uint8_t        expect_type;
    unsigned short expect_msgid;
    int            result;

    unsigned short ack_msgid;
    unsigned char  connack_rc;
    unsigned char  session_present;
    int            granted_qos;
} mqtt_wait_ctx_t;
```

字段含义：

| 字段 | 作用 |
| --- | --- |
| `active` | 当前是否存在合法等待者 |
| `done` | 匹配报文是否已经到达并完成解析 |
| `expect_type` | 期待 `CONNACK`、`SUBACK`、`PUBACK` 等哪类报文 |
| `expect_msgid` | 期待的 packet identifier；无 packet id 的报文填 0 |
| `result` | 协议解析和 Broker 返回结果 |
| 其余字段 | 保存不同 ACK 中需要返回给同步 API 的细节 |

### 6.1 正确的建立顺序

等待上下文必须在发送报文前进入有效状态：

```text
1. 检查当前没有其他 active wait
2. 分配本次 packet_id
3. 填写并激活 wait context
4. 序列化并发送 MQTT 报文
5. 发送失败则立即取消 wait context
6. 循环调用 mqtt_poll_once()
7. 收到匹配 ACK 或超时后读取结果并清理
```

如果先发送、后激活上下文，Broker 返回很快时，`mqtt_poll_once()` 可能在上下文建立前收到 ACK，产生时间窗口竞争。

### 6.2 ACK 匹配规则

| 报文 | 匹配条件 |
| --- | --- |
| `CONNACK` | `active` 且 `expect_type == CONNACK` |
| `SUBACK` | 类型匹配，并且 `submsgid == expect_msgid` |
| `UNSUBACK` | 类型匹配，并且 `unsubmsgid == expect_msgid` |
| `PUBACK` | 类型匹配，并且 `packet_id == expect_msgid` |
| `PINGRESP` | `active` 且 `expect_type == PINGRESP` |

只收到通用的“某个 ACK”不代表当前请求完成。对带 packet identifier 的报文，类型和 `msgid` 必须同时匹配。未匹配的 ACK 只能记录异常或按后续策略保存，不能修改当前等待结果。

## 7. 统一收包器 `mqtt_poll_once()`

建议接口为：

```c
int mqtt_poll_once(void);
int mqtt_poll(uint32_t timeout_ms);
```

`mqtt_poll_once()` 每次只处理一个完整 MQTT 报文：

```c
static unsigned char s_mqtt_packet_buf[MQTT_PACKET_BUF_SIZE];

int mqtt_poll_once(void)
{
    int packet_type;

    packet_type = MQTTPacket_read(s_mqtt_packet_buf,
                                  (int)sizeof(s_mqtt_packet_buf),
                                  transport_getdata);
    if (packet_type < 0)
    {
        return MQTT_POLL_READ_ERROR;
    }

    switch (packet_type)
    {
        case CONNACK:
            return mqtt_dispatch_connack(s_mqtt_packet_buf);

        case SUBACK:
            return mqtt_dispatch_suback(s_mqtt_packet_buf);

        case UNSUBACK:
            return mqtt_dispatch_unsuback(s_mqtt_packet_buf);

        case PUBACK:
            return mqtt_dispatch_puback(s_mqtt_packet_buf);

        case PUBLISH:
            return mqtt_dispatch_publish(s_mqtt_packet_buf);

        case PINGRESP:
            return mqtt_dispatch_pingresp();

        default:
            return MQTT_POLL_UNHANDLED_PACKET;
    }
}
```

该代码是方案示意，不代表当前仓库已经存在这些函数和错误码。

`mqtt_poll(timeout_ms)` 可以在时间窗口内反复调用 `mqtt_poll_once()`，用于同步 API 等待 ACK，也可由主循环周期调用以接收异步消息。

## 8. `MQTTPacket_read()` 的读取过程

调用形式如下：

```c
rv = MQTTPacket_read(buf, sizeof(buf), transport_getdata);
```

它不是简单地“从 TCP 读若干字节”，而是按照 MQTT 报文格式依次读取：

```text
1. 固定头第 1 字节
   -> 得到 MQTT 报文类型、DUP、QoS、RETAIN

2. Remaining Length
   -> 这是 1~4 字节的变长整数
   -> 得到固定头之后还有多少字节

3. 报文剩余部分
   -> 再调用 transport_getdata() 读取 Remaining Length 字节
```

成功后：

1. `buf` 中保存一整个 MQTT 报文。
2. 返回值是 MQTT 报文类型，例如 `CONNACK`、`SUBACK`、`PUBLISH`，不是读取字节数。
3. 后续可直接使用 `switch (rv)` 进行分发。

`transport_getdata()` 可以被多次调用，是因为 TCP 本身是连续字节流。它不需要知道 MQTT 边界，只需按库请求的数量依次提供字节。

### 8.1 整包缓冲约束

当前库源码明确要求：

```text
the whole message must fit into the caller's buffer
```

因此 `MQTT_PACKET_BUF_SIZE` 必须容纳：

```text
固定头
+ Remaining Length 字段
+ topic 长度字段
+ topic 内容
+ packet identifier（QoS > 0 时）
+ payload
```

固件 chunk 的 payload 为 512 字节时，报文总长度一定大于 512 字节。不能因为业务 payload 上限为 512，就把 MQTT 整包缓冲也设置成 512。

### 8.2 半包超时风险

当前 `MQTTPacket_read()` 是无持久状态的同步解析器。如果它已经从 TCP 环形缓冲消费了固定头和一部分报文体，随后因为超时返回错误，下次重新调用时可能会把剩余报文体误当成新报文头。

第一版可以在网络稳定、单包能在 transport 超时内到齐的前提下使用，但必须保留这一风险记录。后续可选方案包括：

1. 调整 transport 的等待策略，确保一次 `getdata(count)` 要么得到完整 `count` 字节，要么将连接判为异常并重连。
2. 使用库中的 `MQTTPacket_readnb()`，保存跨调用的解析状态。
3. 自己维护增量 MQTT 解帧状态机。

## 9. ACK 报文分发

ACK 类报文只更新当前等待上下文，不进入业务 PUBLISH 队列。

### 9.1 `CONNACK`

通过 `MQTTDeserialize_connack()` 得到：

1. `session_present`
2. `connack_rc`

只有当前正在等待 `CONNACK`，并且 `connack_rc == MQTT_CONNECTION_ACCEPTED`，连接流程才成功。

### 9.2 `SUBACK`

通过 `MQTTDeserialize_suback()` 得到：

1. `submsgid`
2. 返回项数量
3. Broker 授予的 QoS；`0x80` 表示订阅失败

第一版每个 `SUBSCRIBE` 只包含一个 topic，因此返回项也应为一个。`submsgid` 必须与本次请求的 `msgid` 一致。

### 9.3 `PUBACK`

`PUBACK` 是设备 QoS1 上行发布的协议确认。它通过 packet identifier 与本次 `PUBLISH` 匹配，不包含 ThingsBoard 业务响应内容。

### 9.4 `PINGRESP`

`PINGRESP` 用于确认 MQTT 连接仍然存活。它没有 packet identifier，因此串行等待上下文只能按报文类型匹配。

## 10. PUBLISH 解析结果与消息结构来源

库函数签名为：

```c
int MQTTDeserialize_publish(unsigned char *dup,
                            int *qos,
                            unsigned char *retained,
                            unsigned short *packetid,
                            MQTTString *topicName,
                            unsigned char **payload,
                            int *payloadlen,
                            unsigned char *buf,
                            int len);
```

它并非只返回 topic 和 payload，而是通过输出参数同时返回：

| 解析结果 | 可写入 `mqtt_rx_msg_t` 的字段 |
| --- | --- |
| `dup` | `msg.dup` |
| `qos` | `msg.qos` |
| `retained` | `msg.retained` |
| `packetid` | `msg.packet_id` |
| `topicName` | `msg.topic` 和 `msg.topic_len` |
| `payload` | `msg.payload` |
| `payloadlen` | `msg.payload_len` |

接收报文中的 topic 使用 MQTT 的“长度 + 数据”编码。解析后通常表现为：

```c
topicName.lenstring.data
topicName.lenstring.len
```

它不是以 `\0` 结尾的 C 字符串。`payload` 同样只是指向报文缓冲区内部的一段原始字节。

## 11. PUBLISH 消息队列设计

建议定义静态环形队列，避免动态内存分配。消息节点可按以下形式设计：

```c
#define MQTT_RX_TOPIC_MAX   96U
#define MQTT_RX_PAYLOAD_MAX 512U

typedef struct
{
    unsigned short packet_id;
    unsigned short topic_len;
    unsigned short payload_len;
    unsigned char  qos;
    unsigned char  dup;
    unsigned char  retained;
    char           topic[MQTT_RX_TOPIC_MAX + 1U];
    unsigned char  payload[MQTT_RX_PAYLOAD_MAX];
} mqtt_rx_msg_t;
```

上述尺寸只是设计示例，最终值需要结合 ThingsBoard topic 最大长度、固件 chunk 大小和 SRAM 占用确定。

### 11.1 必须深拷贝

`MQTTDeserialize_publish()` 返回的 `topicName.lenstring.data` 和 `payload` 都指向 `s_mqtt_packet_buf` 内部。下一次调用 `MQTTPacket_read()` 时，该缓冲会被覆盖。

因此不能把这两个指针直接存入队列，必须在当前分发函数返回前完成深拷贝：

```text
解析结果中的指针：短生命周期，只在当前报文缓冲有效
队列节点中的数组：长生命周期，直到应用层出队
```

### 11.2 topic 拷贝边界

topic 拷贝必须使用显式长度：

```c
topic_len = topic_name.lenstring.len;
memcpy(node->topic, topic_name.lenstring.data, topic_len);
node->topic[topic_len] = '\0';
```

需要先验证：

1. `topic_len > 0`
2. `topic_len <= MQTT_RX_TOPIC_MAX`
3. `topic_name.lenstring.data != NULL`

额外补 `\0` 只是为了应用层便于打印和比较，MQTT 协议本身仍以 `topic_len` 为准。

### 11.3 payload 拷贝边界

payload 是二进制数据，必须使用：

```c
memcpy(node->payload, payload, payload_len);
node->payload_len = (unsigned short)payload_len;
```

禁止使用 `strlen()`、`strcpy()` 或依赖 `\0`。固件内容中可以合法出现任意数量的 `0x00`。

需要先验证：

1. `payload_len >= 0`
2. `payload_len <= MQTT_RX_PAYLOAD_MAX`
3. `payload_len > 0` 时 `payload != NULL`

零长度 payload 是 MQTT 合法消息，应按正常空消息入队，不能当作解析失败。

### 11.4 其他字段边界

#### 11.4.1 Packet Identifier (包ID) 含义与来源

**Packet ID** 是 MQTT 协议中一个 **16 位无符号整数**（0-65535），用于标识和匹配 MQTT 消息的请求和响应。

##### 来源

Packet ID 来源于 **MQTT 固定头部（Fixed Header）的第 2、3 个字节**：

```
MQTT 固定头部格式:
Bit     7 6 5 4 3 2 1 0
Byte 1  Message Type   | DUP | QoS Level | RETAIN
Byte 2  Remaining Length (高位)
Byte 3  Remaining Length (低位)
--- 如果 QoS > 0 ---
Byte 4  Packet Identifier (高位)
Byte 5  Packet Identifier (低位)
```

只有 QoS1/QoS2 消息才包含这两个字节。在我们的代码中，Packet ID 由 MQTT 库解析后提供。例如在解析 `PUBLISH` 消息时：

```c
unsigned char dup;
int qos;
unsigned char retained;
unsigned short packet_id;  // ← MQTTPacket_read 解析出来

MQTTDeserialize_publish(&dup, &qos, &retained, &packet_id, 
                        &topicName, &payload, &payloadlen_in, 
                        buf, buflen);
```

#### 11.4.2 QoS 级别详解与 Packet ID 语义

**QoS (Quality of Service)** 决定了消息传递的可靠性级别，不同级别对 Packet ID 的需求不同。

| QoS 级别 | 语义 | Packet ID | 确认机制 | 适用场景 |
|---------|------|----------|---------|---------|
| QoS0 | At most once (最多一次) | 无 | 无 | 遥测数据、非关键日志 |
| QoS1 | At least once (至少一次) | 有 | PUBACK | 固件分块、属性请求响应 |
| QoS2 | Exactly once (精确一次) | 有 | PUBREC/PUBREL/PUBCOMP | 金融交易、关键指令（较少用） |

##### QoS0 - "At most once" (最多一次)

**语义**：发送者发送消息后**不等待任何确认**，即"fire and forget"。

**特点**：
- 没有重传机制
- 没有确认流程
- 发送即结束

**为什么不需要 packet_id**：
- 没有需要匹配的响应（没有 SUBACK、PUBACK、PUBREC、PUBREL、PUBCOMP）
- 无法保证可靠传输，标识了也没用
- **协议规范明确 QoS0 消息的固定头部不包含 Packet ID 字段**

**代码处理**：`packet_id` 字段统一填 `0`，因为解析函数不会返回有效值。

##### QoS1 - "At least once" (至少一次)

**语义**：确保消息至少到达一次，可能重复。

**流程**：
```
PUBLISH (QoS1, packet_id=X) → PUBACK (packet_id=X)
```

**特点**：
- 客户端发送 `PUBLISH` 时分配一个 packet_id
- 服务端/客户端收到后回复 `PUBACK`，携带**相同的 packet_id**
- 发送方通过 packet_id 匹配请求和响应
- 如果超时未收到 `PUBACK`，根据 packet_id 重传 `PUBLISH`

**需要 packet_id**：用于区分不同的消息流，匹配请求响应。

**在 ThingsBoard OTA 中**：
- 属性请求和响应（`v1/devices/me/attributes/request/1` → `v1/devices/me/attributes/response/1`）
- 固件块请求和响应（`v2/fw/request/24/chunk/22` → `v2/fw/response/24/chunk/22`）
- 都使用 QoS1 确保可靠传输

##### QoS2 - "Exactly once" (精确一次)

**语义**：确保消息精确到达一次。

**流程**：
```
PUBLISH (QoS2, id=X) → PUBREC (id=X) → PUBREL (id=X) → PUBCOMP (id=X)
```

**特点**：
- 四次握手，每一步都通过 packet_id 匹配
- 完全避免重复消息

**需要 packet_id**：用于可靠的两阶段确认。

**当前项目**：第一版不支持 QoS2，收到 QoS2 报文应明确报错。

#### 11.4.3 在 ThingsBoard OTA 场景中的体现

查看我们之前的日志：

```
INFO: ESP8266 MQTT publish [78] bytes to topic [v1/devices/me/attributes/request/1] ok
INFO: Waiting ThingsBoard firmware info response, request id 1, attempt 1/3
MQTT event popped topic [v1/devices/me/attributes/response/1], 180 bytes, count 0
INFO: ESP8266 MQTT received topic [v1/devices/me/attributes/response/1], 180 bytes
```

这里：
- 我们发布 QoS1 的消息到 `v1/devices/me/attributes/request/1`
- 收到响应 `v1/devices/me/attributes/response/1`（注：这里的 `1` 是 ThingsBoard 业务层的请求 ID，不是 MQTT packet_id）
- 实际上，MQTT 层面会自动分配 packet_id（如 0x0001），并通过匹配 PUBACK 确认

对于固件分块下载：
```
INFO: ESP8266 MQTT publish [3] bytes to topic [v2/fw/request/24/chunk/22] ok
MQTT event popped topic [v2/fw/response/24/chunk/22], 508 bytes, count 0
```

同样，每次请求都有 MQTT packet_id 的确认流程。

#### 11.4.4 业务层 Packet ID 使用规则

在 `mqtt_rx_msg_t` 结构体中：

```c
typedef struct {
    mqtt_msg_type_t type;           // 消息类型
    char topic[MQTT_RX_TOPIC_MAX];  // topic
    uint8_t *payload;               // payload 指针
    uint16_t payload_len;           // payload 长度
    uint16_t packet_id;             // 包 ID (QoS1/2 有效)
} mqtt_rx_msg_t;
```

**`packet_id` 字段的使用规则**：
- **QoS0 PUBLISH**：`packet_id` 填 `0`（因为协议中没有这个字段）
- **QoS1/2 PUBLISH**：`packet_id` 填实际值，用于后续可能的 `PUBACK`/`PUBREC` 处理
- **CONNACK/SUBACK/PUBACK 等**：这些消息类型**没有 packet_id 字段**（或与 PUBLISH 的 packet_id 是不同的概念）

**为什么我们设计中保留这个字段？**
- 虽然当前 ThingsBoard 主要用 QoS1 下发固件，MQTT 库会自动处理 PUBACK
- 保留字段是为了**未来扩展**：
  - 实现 QoS2 支持
  - 实现 MQTT 5.0 的特性
  - 添加消息去重逻辑（QoS1 可能收到重复消息，可通过 packet_id 识别）
- 在 QoS0 场景填 `0`，避免无意义的值。

#### 11.4.5 Packet ID 与 ThingsBoard Request ID 的区别

注意区分两个层面的 ID：

| ID 类型 | 层次 | 来源 | 用途 | 示例 |
|--------|------|------|------|------|
| **MQTT packet_id** | MQTT 协议层 | MQTT 固定头部 | 匹配 PUBLISH/PUBACK/SUBACK 等协议确认 | `0x0001`, `0x0002` |
| **ThingsBoard request_id** | 应用业务层 | topic 路径 | 匹配请求与响应的业务逻辑 | `1`, `24`, `25` |

在 ThingsBoard OTA 中：
- 业务层使用 request_id 追踪固件下载进度
- MQTT 层使用 packet_id 确保每个 QoS1 消息的可靠传输
- 两者正交，各司其职

例如：
```
上行业务 topic: v2/fw/request/24/chunk/22
下行业务 topic: v2/fw/response/24/chunk/22
             ↑        ↑
        request_id  chunk_index

MQTT packet_id: 0x0005 (由 MQTT 库自动分配，与 request_id 无关)
```



1. `packet_id` 只在 QoS1/QoS2 PUBLISH 中有意义；QoS0 节点统一写 0。
2. 第一版只支持 QoS0 和 QoS1；收到 QoS2 时明确报“不支持”，不能按 QoS1 处理。
3. 在把 `int` 长度转换为较小无符号类型前，必须先完成范围检查。
4. topic 或 payload 超限时不能静默截断，否则 topic 会被错误路由，固件块会被永久破坏。

## 12. 队列节点的完整提交顺序

消息入队应采用“先验证和填充，最后提交索引”的方式，避免应用层看到半写入节点：

```text
1. MQTTDeserialize_publish() 解析报文
2. 验证 QoS、topic 指针和长度、payload 指针和长度
3. 检查队列是否有空闲节点
4. 取得当前 write_index 对应节点，但暂不修改队列计数
5. 拷贝 topic、payload 和元数据
6. 完整节点写入成功后，更新 write_index 和 count
7. QoS1 报文成功入队后，再发送 PUBACK
```

核心原则是：

```text
write_index/count 的更新是“提交动作”。
提交之前，该节点对消费者不可见。
```

如果生产和消费都只发生在主循环上下文，队列索引暂时不需要锁。未来若 ISR 或 RTOS 任务也访问该队列，则必须增加临界区或改成明确的单生产者、单消费者并发实现。

### 12.1 队列满时的处理

队列满不能静默覆盖。不同业务对丢包策略的要求并不相同：

| 业务 | 可接受策略 |
| --- | --- |
| 属性状态推送 | 某些场景可考虑丢旧保新 |
| 固件 chunk | 不能丢旧、不能截断、不能乱序覆盖 |

因此当前不把“丢最旧消息”固化到通用 MQTT 层。第一版在队列满时应：

1. 打印明确错误并增加溢出计数。
2. 不提交新节点。
3. 对 QoS1 消息不发送成功 `PUBACK`，让 Broker 保留重传可能。

这里的“不回 `PUBACK`”不是额外功能分支，而是**由正确的提交顺序自然体现出来**：

```text
队列满
-> 本次消息不能成功入队
-> 不执行提交动作
-> 不进入 PUBACK 发送分支
-> Broker 仍认为该 QoS1 下行尚未被可靠接收
```

这样做的目的不是立即实现完整重传机制，而是避免“消息实际上丢了，但协议层已经告诉 Broker 收到了”的更坏情况。

由于第一版固件请求是串行的，正常情况下每请求一块便立即等待、出队和处理，队列不应持续堆积。出现队列满应视为调度或容量设计错误。

### 12.2 PUBACK 的发送时机

QoS1 下行的正确顺序是：

```text
解析成功
-> 边界检查成功
-> 消息成功入队
-> MQTTSerialize_puback()
-> transport_sendPacketBuffer()
```

如果入队前就发送 `PUBACK`，之后又因队列满或长度超限丢弃消息，Broker 会认为消息已可靠交付，不会再重传。

如果消息已入队但 `PUBACK` 发送失败，Broker 可能带 `DUP=1` 再次发送相同 packet identifier。后续业务层需要结合 `packet_id`，以及 ThingsBoard topic 中的 `requestId/chunkIndex` 处理重复消息。

第一版实现策略可先定为：

1. 若已判定为重复包，则不再重复入队。
2. 对重复的 QoS1 下行，补发一次 `PUBACK`。
3. 固件块场景下，业务层仍建议结合 `requestId + chunkIndex` 做二次保护，确保不会重复写块。

## 13. 同步 API 改造后的运行方式

同步 API 的外观可以保留，但内部不能再直接读取指定类型的报文。以单 topic 订阅为例：

```text
mqtt_subscribe_topic(topic, qos, msgid)
-> 建立 wait context：expect=SUBACK, expect_msgid=msgid
-> 序列化并发送 SUBSCRIBE
-> 循环 mqtt_poll_once()
   |- 收到 PUBLISH：入消息队列，继续等待
   |- 收到其他 ACK：记录异常，继续等待
   `- 收到匹配 SUBACK：完成 wait context
-> 检查 granted_qos
-> 清理 wait context
-> 返回成功或失败
```

这解决了当前最核心的问题：同步等待 ACK 时，异步 PUBLISH 不再被丢弃。

### 13.1 `wait_ctx` 生命周期约定

结合当前串行模型，第一版可以把等待上下文的使用规则直接定死：

1. `mqtt_connect()`、`mqtt_subscribe_topic()`、`mqtt_unsubscribe_topic()`、`mqtt_publish(qos=1)` 在发送前建立 `wait_ctx`。
2. 成功收到目标 ACK 后，读取结果并立即清除 `wait_ctx`。
3. 超时后强制失效并清除 `wait_ctx`，上层返回网络或超时错误。
4. 等待某个 ACK 时，若先收到 `PUBLISH`，则入队后继续等待。
5. 等待某个 ACK 时，若先收到其他 ACK，则打印日志，不改当前结果，继续等待目标 ACK。

这里的核心不是“收到了 ACK 就成功”，而是“收到了**类型和标识都匹配**的 ACK 才成功”。

### 13.1.1 `mqtt_wait_result_t` 的语义约定

`wait_ctx` 里的 `result` 字段不建议再配一套额外宏，直接使用枚举值即可。也就是说：

```c
typedef enum
{
    MQTT_WAIT_RESULT_IDLE = 0,
    MQTT_WAIT_RESULT_OK,
    MQTT_WAIT_RESULT_TIMEOUT,
    MQTT_WAIT_RESULT_SEND_FAIL,
    MQTT_WAIT_RESULT_PROTOCOL_ERROR,
    MQTT_WAIT_RESULT_BROKER_REJECT,
    MQTT_WAIT_RESULT_UNEXPECTED_ACK
} mqtt_wait_result_t;
```

这里要注意两点：

1. 枚举本身默认从 `0` 开始递增，不需要另外再定义一套同义整数宏。
2. `result` 只表达“这次等待最终是什么结果”，不负责保存业务数据。

各枚举值的推荐语义如下：

| 枚举值 | 含义 | 使用场景 |
| --- | --- | --- |
| `MQTT_WAIT_RESULT_IDLE` | 当前还没有最终结果 | 刚初始化、刚 reset、等待尚未结束 |
| `MQTT_WAIT_RESULT_OK` | 成功收到目标 ACK，且内容合法 | `CONNACK`、`SUBACK`、`PUBACK` 正常匹配 |
| `MQTT_WAIT_RESULT_TIMEOUT` | 在等待窗口内没有等到目标 ACK | 网络卡住、Broker 未回复、链路异常最终超时 |
| `MQTT_WAIT_RESULT_SEND_FAIL` | 报文在发送阶段就失败 | `transport_sendPacketBuffer()` 或底层发送失败 |
| `MQTT_WAIT_RESULT_PROTOCOL_ERROR` | 收到报文但反序列化失败，或字段不合法 | `SUBACK`/`CONNACK`/`PUBACK` 解析失败 |
| `MQTT_WAIT_RESULT_BROKER_REJECT` | Broker 回复合法，但语义上明确拒绝 | `CONNACK return code != accepted`、`SUBACK qos == 0x80` |
| `MQTT_WAIT_RESULT_UNEXPECTED_ACK` | 先收到了不属于当前等待目标的 ACK | 第一版主要用于日志诊断，通常不直接结束当前等待 |

其中最容易混淆的是下面三组：

```text
SEND_FAIL = 报文都没发出去
TIMEOUT   = 发出去了，但没等到目标 ACK

PROTOCOL_ERROR = 收到的包格式不对
BROKER_REJECT  = 收到的包格式正确，但 Broker 明确拒绝

UNEXPECTED_ACK = 先来了别的 ACK；第一版先记日志，不直接判失败
```

第一版实现时，建议高频使用的结果就是：

1. `IDLE`
2. `OK`
3. `SEND_FAIL`
4. `TIMEOUT`
5. `PROTOCOL_ERROR`
6. `BROKER_REJECT`

而 `UNEXPECTED_ACK` 先作为保留语义，主要用于日志诊断，不强制写入 `result` 终止当前等待。

### 13.2 `packet_id` 分配器口径

第一版 `packet_id` 分配器建议采用“内部计数器可自然回绕，但**不能把 0 分配给 MQTT 报文**”的口径。

要区分两件事：

1. **C 变量层面**：`unsigned short` 自然溢出时，可以从 `0xFFFF` 回绕到 `0`。
2. **MQTT 协议层面**：`packet identifier` 的合法取值范围是 `1 ~ 65535`，`0` 不能作为报文字段使用。

因此这里不是“避免内部变量出现 0”，而是“避免把 0 发到 MQTT 报文里”。推荐写法如下：

```c
static unsigned short mqtt_alloc_packet_id(void)
{
    s_next_packet_id++;

    if (s_next_packet_id == 0U)
    {
        s_next_packet_id = 1U;
    }

    return s_next_packet_id;
}
```

这段代码的语义是：

```text
内部计数器允许自然回绕
-> 若回绕结果为 0
-> 立即跳到 1
-> 返回给协议层的 packet_id 始终是非 0 的合法值
```

### 13.3 `mqtt_poll()` 的第一版调度方式

第一版先不做后台常驻 `poll` 线程或统一调度器，而是采用：

```text
同步等待函数内部自旋调用 mqtt_poll_once()
```

也就是：

1. 连接函数内部等待 `CONNACK`
2. 订阅函数内部等待 `SUBACK`
3. 取消订阅函数内部等待 `UNSUBACK`
4. QoS1 发布函数内部等待 `PUBACK`

这种方式虽然不够“全异步”，但和当前 OTA 流程的串行结构一致，便于调试和控制复杂度。

### 13.3.1 `mqtt_poll_once()` 的分发返回与上层关系

`mqtt_poll_once()` 的核心职责是：

```text
读取一个完整 MQTT 报文
-> 判断报文类型
-> ACK 类尝试投递给 wait_ctx
-> PUBLISH 类尝试入消息队列
```

这里要特别注意：第一版中，上层同步等待函数**并不主要依赖 `mqtt_poll_once()` 的返回值来判断“是否等到了目标 ACK”**。

真正决定等待是否完成的是：

1. `s_wait_ctx.done`
2. `s_wait_ctx.result`

例如在处理 `SUBACK` / `PUBACK` 时，可能出现如下分支：

```c
if (msgid != s_wait_ctx.expect_msgid)
{
    return 0;
}
```

这段代码的语义不是“上层已经知道发生了什么”，而是：

```text
当前这包虽然是 ACK
-> 但不是本次等待目标对应的 msgid
-> 因此不修改 wait_ctx
-> 本次 poll 结束，上层继续等待下一包
```

因此第一版的职责划分是：

| 信息 | 由谁表达 |
| --- | --- |
| 当前等待是否结束 | `s_wait_ctx.done` |
| 当前等待最终是成功还是失败 | `s_wait_ctx.result` |
| 本次 poll 只是处理了一个“无关 ACK”还是“真正匹配 ACK” | 主要通过日志观察 |

这也解释了为什么很多分支里会出现：

```c
if ((s_wait_ctx.active == 0U) ||
    (s_wait_ctx.expect_type != MQTT_WAIT_xxx))
{
    return 0;
}
```

以及：

```c
if (msgid != s_wait_ctx.expect_msgid)
{
    return 0;
}
```

它们的共同目标都是：

```text
不是当前等待目标
-> 不改 wait_ctx
-> 不误判成功
-> 只记日志，继续等待
```

所以第一版里，`mqtt_poll_once()` 的返回值可以保持简单；真正的等待完成条件仍然是 `wait_ctx` 被匹配 ACK 正确更新。

### 13.4 `clean session` 与 `session present`

第一版建议不要依赖 Broker 持久会话，直接采用：

```text
每次连接成功后都主动重新订阅
```

相关字段含义如下：

| 字段 | 含义 | 第一版策略 |
| --- | --- | --- |
| `clean session` | 本次连接是否要求 Broker 使用全新会话 | 不依赖持久会话，重连后重新订阅 |
| `session present` | Broker 在 `CONNACK` 中返回，表示服务端是否已有该客户端旧会话 | 仅打印日志，不参与业务决策 |

这样做的原因是：当前设备不是长期在线，而是“上电连接、检查属性或升级、完成后断开”。在这种模式下，显式重新订阅比依赖旧会话更简单、更稳定。

### 13.5 QoS1 重复包的第一版处理口径

QoS1 的语义是：

```text
至少一次到达（At least once）
```

它不保证“只到一次”，因此重复包是协议允许的正常情况。最典型的场景是：

```text
Broker 发送 QoS1 PUBLISH
-> 设备已经收到并处理
-> 设备发出的 PUBACK 丢失，或 Broker 未收到
-> Broker 重发同一个 PUBLISH
-> 重发包通常会带 DUP=1
```

因此第一版必须接受一个现实：

```text
同一条 QoS1 下行消息，设备可能会收到多次
```

#### 13.5.1 第一版目标

第一版先保证两件事：

1. 不能把重复包再次入队、再次执行业务处理。
2. 对已经识别出来的重复 QoS1 包，要补发 `PUBACK`，让 Broker 停止重传。

也就是：

```text
识别重复
-> 不重复处理
-> 仍然补协议 ACK
```

#### 13.5.2 重复识别的分层

要区分“协议层看到的重复嫌疑”和“业务层确认的重复块”。

协议层在 `MQTTDeserialize_publish()` 后能拿到：

1. `dup`
2. `qos`
3. `packet_id`
4. `topic`
5. `payload`

这里：

- `dup = 1` 表示“这包可能是重传”
- `packet_id` 表示 QoS1 协议层编号

但第一版不建议只靠 `dup = 1` 就直接认定它是重复包。更稳妥的理解是：

```text
DUP=1 只是重复嫌疑
是否真正丢弃，还要结合本地已处理记录
```

#### 13.5.3 固件块场景的幂等键

对于当前 OTA 业务，最可靠的判重键不是单独的 `packet_id`，而是：

```text
requestId + chunkIndex
```

因为固件块响应 topic 本身就是：

```text
v2/fw/response/<requestId>/chunk/<chunkIndex>
```

例如：

```text
v2/fw/response/24/chunk/22
```

只要本地已经成功处理过：

```text
requestId = 24
chunkIndex = 22
```

那后续再次到达相同业务键的块，就应该视为业务重复块，不应再次写 LFS。

因此：

```text
packet_id 适合做协议确认匹配
requestId + chunkIndex 适合做固件块业务幂等
```

#### 13.5.4 第一版处理顺序

**首次到达的 QoS1 固件块：**

```text
解析成功
-> topic/payload 合法
-> 判定为新块
-> 入队或进入业务处理
-> 成功后发送 PUBACK
```

**重复到达的 QoS1 固件块：**

```text
解析成功
-> 识别出 requestId + chunkIndex 已处理过
-> 不再重复入队
-> 不再重复写 LFS
-> 但仍补发 PUBACK
```

这里要强调：

```text
被丢弃的是重复业务处理
不是协议确认
```

#### 13.5.5 为什么重复包仍要回 PUBACK

如果已经处理过某块，但不回 `PUBACK`，Broker 仍会继续重发，设备就会不断看到同一块。  
因此对重复 QoS1 包的正确动作不是“静默丢弃”，而是：

```text
识别为重复
-> 不重复执行业务
-> 回 PUBACK，告诉 Broker 这条已经收到
```

#### 13.5.6 第一版实现口径

第一版可直接定为：

1. QoS1 下行允许重复到达，这是协议正常现象。
2. `dup=1` 只视为重复嫌疑，不单独作为最终判据。
3. 固件块最终按 `requestId + chunkIndex` 做幂等判断。
4. 首次到达的块：正常处理，成功后 `PUBACK`。
5. 已处理过的重复块：不重复处理，但补发 `PUBACK`。
6. 队列满、长度非法、节点非法时：不算成功接收，不发送成功 `PUBACK`。

### 13.6 固件块 `requestId + chunkIndex` 的严格幂等校验

在当前“串行逐块请求”的 OTA 模型里，第一版最稳妥的做法不是引入复杂 bitmap，而是维护一个**当前下载上下文**，只接受“当前会话、当前块号”的固件响应。

建议下载上下文至少包含：

```c
typedef struct
{
    uint8_t  active;
    uint32_t request_id;
    uint32_t expected_chunk_index;
    uint32_t chunk_size;
    uint32_t total_chunks;
    uint32_t fw_size;
    uint32_t received_bytes;
} tb_fw_dl_ctx_t;
```

字段含义如下：

| 字段 | 含义 |
| --- | --- |
| `active` | 当前是否存在一次固件下载会话 |
| `request_id` | 当前下载请求对应的 ThingsBoard `requestId` |
| `expected_chunk_index` | 当前唯一允许接收的块序号 |
| `chunk_size` | 请求块大小，第一版固定为 `512` |
| `total_chunks` | 总块数 |
| `fw_size` | 固件总大小 |
| `received_bytes` | 已成功写入的字节数 |

在这个模型下，当前会话天然形成如下约束：

```text
[0, expected_chunk_index - 1]     已处理过
 expected_chunk_index             当前唯一允许接收的块
> expected_chunk_index            不应在此时出现
```

#### 13.6.1 收到块后的严格校验顺序

业务层收到 `v2/fw/response/...` 后，建议按以下顺序校验：

1. **先解析 topic**
   必须能成功提取：
   - `requestId`
   - `chunkIndex`

   如果 topic 解析失败：

   ```text
   打印日志
   -> 丢弃
   -> 不写文件
   ```

2. **校验 `requestId`**
   收到的 `requestId` 必须等于当前下载上下文中的 `ctx.request_id`。

   若不匹配：

   ```text
   打印日志
   -> 视为无关消息或旧消息
   -> 丢弃
   -> 继续等待当前真正目标块
   ```

3. **校验 `chunkIndex`**
   必须与 `ctx.expected_chunk_index` 比较：

   - `chunkIndex == expected_chunk_index`
     表示当前真正想要的块，可进入后续长度校验。

   - `chunkIndex < expected_chunk_index`
     表示旧块或重复块，第一版按重复块处理：

     ```text
     打印日志
     -> 不重复写 LFS
     -> 丢弃业务处理
     ```

   - `chunkIndex > expected_chunk_index`
     表示乱序块或未来块。在当前“请求一块、等待一块”的串行模型里，这种情况不应出现：

     ```text
     打印日志
     -> 丢弃
     -> 不推进状态
     -> 继续等待当前块或最终超时
     ```

#### 13.6.2 payload 长度也属于幂等校验的一部分

即使 `requestId` 和 `chunkIndex` 都匹配，也不能直接写文件，还必须校验 payload 长度。

期望长度规则如下：

```text
remaining = fw_size - received_bytes
expected_len =
    (remaining >= chunk_size) ? chunk_size : remaining
```

也就是：

1. **非最后一块**
   `payload_len` 必须严格等于 `chunk_size`，第一版即 `512`。

2. **最后一块**
   `payload_len` 必须严格等于剩余字节数，例如剩余 `508` 时就必须收到 `508`。

若长度不匹配：

```text
打印日志
-> 丢弃
-> 不写文件
-> 不推进 expected_chunk_index
```

#### 13.6.3 状态推进必须晚于写文件成功

只有在以下条件全部成立时，才允许真正提交这一块：

1. topic 解析成功
2. `requestId` 匹配当前会话
3. `chunkIndex == expected_chunk_index`
4. `payload_len == expected_len`
5. 写 LFS 成功

只有这五条都满足，才允许推进状态：

```text
received_bytes += payload_len
expected_chunk_index++
```

这里的顺序不能颠倒。必须是：

```text
先写文件成功
-> 再推进 expected_chunk_index
```

否则一旦写文件失败，而状态已经前移，后续整个下载流程就会乱掉。

#### 13.6.4 第一版可直接落地的口径

第一版固件块幂等规则可以直接定为：

1. 当前下载会话只接受一个 `request_id`。
2. 当前时刻只接受一个 `expected_chunk_index`。
3. `requestId` 不匹配：丢弃并继续等待。
4. `chunkIndex < expected`：视为重复块，不重复写。
5. `chunkIndex > expected`：视为乱序块，丢弃并继续等待当前块。
6. 长度不匹配：丢弃并继续等待当前块。
7. 只有“当前会话、当前块号、当前长度全部匹配且写文件成功”时，才推进状态。

这就保证了：

```text
同一个 requestId + chunkIndex
最多只会成功提交一次
```

### 13.7 `transport_getdata(count)` 的半包返回约定

当前接收链路为：

```text
s_tcp_rx_ring
-> esp8266_sock_recv()
-> transport_getdata()
-> MQTTPacket_readnb()
-> mqtt_poll_once()
```

在这条链路中，`MQTTPacket_read()` 是同步整包解析器。它会分阶段调用 `transport_getdata(count)`：

1. 读取固定头第 1 字节
2. 读取 Remaining Length
3. 读取整个报文剩余部分

因此第一版 `transport_getdata(count)` 最好采用一个很硬的约定：

```text
要么按要求拿满 count 个字节并返回 count
要么直接返回错误
```

也就是：

```text
返回 == count    表示成功完整交付
返回 < 0         表示失败
不向上层暴露 0 ~ count-1 的短读结果
```

#### 13.7.1 为什么第一版不能暴露短读

如果 `MQTTPacket_read()` 正在读取某个大 `PUBLISH`，固定头和 Remaining Length 已经消费完毕，而 `transport_getdata(520)` 只读到一半就返回，那么已经被消费掉的那部分字节就无法简单“放回去”。

下一次若重新调用 `MQTTPacket_read()`，它可能会把上次剩余报文体中的某一段误当成新包头，导致 MQTT 流同步丢失。

因此在当前“同步整包解析、无跨调用解帧状态”的第一版中，最稳妥的选择就是：

```text
内部允许循环多次搬运
-> 但对 MQTT 层只表现为“整段成功”或“整体失败”
```

#### 13.7.2 第一版实现口径

第一版建议直接定成：

1. `transport_getdata(count)` 内部可以多次调用 `esp8266_sock_recv()`。
2. 若最终累计拿满 `count` 字节，则返回 `count`。
3. 若在规定时间内始终拿不满，或底层出现错误，则返回负数。
4. 不向 `MQTTPacket_read()` 暴露部分成功的短读结果。
5. 一旦发生半包失败，当前版本统一按网络异常或本轮会话失败处理，不做半包恢复。

可将其概括为：

```text
sock_recv 可以是渐进式拿数据
transport_getdata 必须对 MQTT 层提供原子交付
```

#### 13.7.3 第一版的取舍

这种做法的代价是：

1. 对网络抖动不够友好
2. 某些本可恢复的半包情况会直接判失败
3. 鲁棒性不如显式保存跨调用解帧状态的实现

但它的好处是：

1. 行为简单且确定
2. 不会悄悄破坏 MQTT 字节流边界
3. 非常适合当前“先打通 OTA 主链路”的阶段

因此当前版本的核心口径就是：

```text
在同步整包解析模型下
transport_getdata() 对上层必须表现为“整段交付”接口
而不是“流式短读”接口
```

### 13.8 TCP 环形缓冲、transport 缓冲、MQTT 整包缓冲的尺寸关系

这三层缓冲区不是一回事，不能简单全部设置成 `512`。它们分别对应三种不同职责：

| 缓冲区 | 作用 | 关注点 |
| --- | --- | --- |
| `s_tcp_rx_ring` | 吸收从 `+IPD` 提取出来的 TCP 原始字节流 | 串口接收突发、主循环消费延迟 |
| `transport` 临时缓冲 | 给 `transport_getdata()` 做中间搬运 | 不能成为 `getdata(count)` 的上限瓶颈 |
| MQTT 整包缓冲 | 供 `MQTTPacket_read()` 解析完整 MQTT 报文 | 必须能容纳“完整 MQTT 包” |

#### 13.8.1 三者的本质区别

1. **`s_tcp_rx_ring`**
   它不是只保存“当前一包”，而是用来吸收 UART 接收和上层解析之间的速度差。因此它应该大于单个 MQTT 整包大小。

2. **transport 临时缓冲**
   它本质是搬运层，不一定要保存完整 MQTT 包，但不能因为它太小而让 `transport_getdata(count)` 永远无法交付大于它本身长度的数据。

3. **MQTT 整包缓冲**
   它必须按“完整 MQTT 报文”来算，而不是只按业务 payload 算。完整报文至少包含：

   ```text
   固定头
   + Remaining Length
   + topic length field
   + topic
   + packet_id (QoS1)
   + payload
   ```

#### 13.8.2 为什么不能都按 512 设计

当前固件块业务 payload 虽然是 `512` 字节，但 MQTT 整包并不等于 payload：

```text
payload 512
!= MQTT packet buffer 512
!= TCP ring 512
```

因为：

1. topic 本身也要占空间
2. QoS1 还要包含 `packet_id`
3. 固定头和 Remaining Length 也要占空间
4. TCP 环形缓冲还要承受“上层尚未及时消费”的积压

#### 13.8.3 第一版建议口径

按固件块响应 `payload <= 512`、topic 预留约 `96` 字节估算，MQTT 整包缓冲建议至少覆盖：

```text
固定头           1~5
topic 长度字段    2
topic            <= 96
packet_id         2
payload         <= 512
```

因此第一版建议：

1. `MQTT_PACKET_BUF_SIZE` 按完整包估算，建议级别约 `768`
2. `MQTT_RX_TOPIC_MAX` 建议级别约 `96`
3. `MQTT_RX_PAYLOAD_MAX` 第一版仍为 `512`
4. `s_tcp_rx_ring` 应大于 MQTT 整包缓冲，建议级别约 `1536` 或 `2048`
5. transport 私有缓冲可以保留较小值，但实现上必须允许循环搬运

#### 13.8.4 当前代码现状与目标方案的差异

当前代码中已经观察到：

1. `transport` 层私有缓冲 `TRANSPORT_SOCK_BUF_SIZE = 512`
2. `core_mqtt.c` 中 `CORE_MQTT_BUF_SIZE = 256`
3. 当前 `transport_getdata()` 仍会向上层暴露短读结果

这说明：

```text
当前实现
!= 目标中的“整段交付 + 够大的 MQTT 整包缓冲”
```

后续若要完全落地新方案，需要继续调整：

1. MQTT 整包缓冲改为模块级、尺寸按完整包计算
2. `transport_getdata()` 改为不向 MQTT 层暴露短读
3. transport 临时缓冲只作为内部搬运缓冲，不再成为上限约束

#### 13.8.5 RAM 紧张时的优先级

如果后续 RAM 仍然吃紧，建议按下面顺序保容量：

1. **优先保证 MQTT 整包缓冲够大**
2. **其次保证 TCP 环形缓冲不容易溢出**
3. **最后再考虑缩小 transport 私有缓冲**

原因是：

```text
MQTT 整包缓冲不够 -> 协议层直接无法稳定工作
TCP ring 太小      -> 串口突发时可能丢字节
transport 缓冲偏小 -> 只要支持循环搬运，仍有优化空间
```

### 13.9 `MQTTTransport` 的初始化与 reset 规则

若后续切换到 `MQTTPacket_readnb()`，则 MQTT 半包续收状态将保存在 `MQTTTransport` 结构体中。  
因此 `MQTTTransport` 应视为 **`core_mqtt` 层的协议状态对象**，而不是 `transport` 层或业务层对象。

建议在 `core_mqtt.c` 中保留模块级对象：

```c
static MQTTTransport s_mqtt_trp;
static unsigned char s_mqtt_packet_buf[MQTT_PACKET_BUF_SIZE];
```

#### 13.9.1 为什么它应该属于 `core_mqtt`

`MQTTTransport` 保存的是“当前 MQTT 解帧进度”，包括：

1. 当前读到哪一个阶段
2. Remaining Length 还差多少
3. 当前包已经累计多少字节

这些都属于 MQTT 协议层状态，不应由 `transport.c` 或 ThingsBoard/OTA 业务层直接管理。

#### 13.9.2 推荐初始化方式

建议提供统一函数，例如：

```c
static void mqtt_transport_state_reset(void)
{
    memset(&s_mqtt_trp, 0, sizeof(s_mqtt_trp));
    s_mqtt_trp.getfn = mqtt_transport_getfn_nb;
    s_mqtt_trp.sck   = NULL;
}
```

其含义是：

1. `state = 0`，从读取新包头开始
2. `len = 0`，当前包累计长度清零
3. `rem_len = 0`，当前剩余长度清零
4. `multiplier = 0`
5. 重新绑定 non-blocking `getfn`

#### 13.9.3 什么时候必须 reset

第一版可以把 reset 时机明确分成两类：

**1. 新 MQTT 会话开始前**

例如：

```text
transport_open()
-> transport_clearBuf()
-> mqtt_transport_state_reset()
-> 开始 CONNECT
```

这样可以保证新会话不会继承上一轮的半包状态。

**2. 当前未完成包已经不可信时**

包括：

1. 总等待超时
2. socket 断开
3. `transport_getdata()` 返回 `<0`
4. `MQTTPacket_readnb()` 返回 `<0`
5. MQTT 解析错误，并且当前会话决定作废

这些场景都意味着：

```text
当前 MQTT 包不再可信
-> 必须 reset MQTTTransport
```

#### 13.9.4 什么时候不能 reset

这一点和旧的“同步整包 + 半包即失败”模型不同。

若 `MQTTPacket_readnb()` 返回 `0`，含义是：

```text
当前包还没收齐
-> 继续等待下一批字节
```

这时**绝不能 reset**，否则就等于把“正常半包续收”自己打断了。

所以第一版必须明确：

```text
readnb() 返回 0
= 正常等待
≠ 失败
≠ reset
```

#### 13.9.5 reset 的职责归属

建议职责固定为：

1. `core_mqtt` 层负责 reset `MQTTTransport`
2. `transport` 层只负责清自己的私有缓存
3. OTA/ThingsBoard 业务层不直接操作 `MQTTTransport`

可进一步封装为两个层次：

**`mqtt_abort_current_wait(reason)`**

只负责把当前 `wait_ctx` 标为失败完成。

**`mqtt_session_reset()`**

负责统一清理：

1. `wait_ctx`
2. MQTT 消息队列
3. `MQTTTransport`
4. `transport` 私有缓存
5. TCP 原始接收环形缓冲（需要底层提供清空接口）

#### 13.9.6 第一版建议口径

可以直接定为：

1. `MQTTTransport` 是 `core_mqtt` 的模块级解帧状态对象。
2. 每次新 MQTT 会话开始前 reset 一次。
3. 若当前会话的未完成包已不可信，则 reset。
4. 若只是“当前没收齐，`readnb()` 返回 0”，则不 reset，继续等待。
5. reset 由 `core_mqtt` 层统一触发，不向业务层外泄。

### 13.10 版本决策、传输校验与 OTA 校验的边界

当前方案中，这三层职责需要明确分开，不能混用：

#### 13.10.1 ThingsBoard 属性阶段负责“要不要升级”

在拉取固件属性后，业务层已经拿到：

1. `fw_version`
2. `fw_size`
3. `fw_checksum`
4. `fw_checksum_algorithm`

这一阶段的职责是：

```text
比较平台版本与本地当前版本
-> 决定是否需要下载
```

因此：

```text
是否要升级
= ThingsBoard 属性阶段决定
```

这一步属于业务决策，不属于 OTA 校验态的主职责。

#### 13.10.2 传输校验只负责“文件有没有传坏”

下载阶段的校验边界应明确为：

1. `received_bytes == fw_size`
2. `requestId + chunkIndex` 顺序正确
3. 整体 checksum 与平台下发值一致
4. 每个块的长度符合当前期望

这层只回答：

```text
从平台拉下来的字节流
是否与平台声称要发的文件一致
```

也就是：

```text
传输校验
= 文件有没有传坏
```

#### 13.10.3 OTA 校验态只负责“包能不能安装”

当传输校验已经通过后，才把文件交给 OTA 校验态。  
OTA 校验态的主职责是：

1. 包头 / magic 校验
2. 包结构合法性
3. 包内 hash / 签名校验
4. 镜像格式与可启动性检查
5. 目标槽位写入前的镜像合法性检查

因此：

```text
OTA 校验
= 这个文件是不是合法可安装升级包
```

这里不再承担“是否要升级”的主决策。

#### 13.10.4 版本信息在 OTA 校验态中的位置

第一版推荐口径是：

```text
版本是否需要升级
-> 由 ThingsBoard 属性阶段决定

OTA 校验态
-> 不做主版本决策
```

若后续 OTA 包头中也带版本号，则 OTA 校验态最多做**防御性一致性复核**，例如：

1. 包头版本字段存在且可解析
2. 包头版本与属性阶段预期版本一致

但这只是增强项，不是第一版主流程。

### 13.11 临时文件清理与命名回收策略

当前下载流程已经变成：

```text
ThingsBoard 下载
-> 写入 LFS 文件
-> 传输校验
-> 交给 OTA 校验态与安装流程
```

因此必须区分：

```text
文件已写入 LFS
!= 文件已被证明可升级
```

第一版建议把下载文件始终视为**临时文件**。

#### 13.11.1 第一版命名策略

第一版最简单稳妥的做法是：

```text
使用单一固定临时文件名
```

例如：

```text
tb_fw.tmp
```

这样可以避免：

1. LFS 中残留过多历史文件
2. 多个 requestId 临时文件并存带来的清理复杂度
3. 旧文件与新文件命名冲突处理复杂化

#### 13.11.2 文件何时才算“可交付给 OTA”

只有在以下条件全部满足后，临时文件才允许交给 OTA 状态机：

1. 所有 chunk 下载完成
2. `received_bytes == fw_size`
3. 传输 checksum 校验通过

到这里为止，它只是：

```text
传输正确的临时文件
```

接下来再交由 OTA 校验态判断：

1. 包头是否合法
2. 包结构是否合法
3. 签名 / 镜像是否可接受

因此第一版要明确：

```text
下载完成
!= OTA 合法
```

#### 13.11.3 第一版清理时机

建议把清理时机直接定死：

1. **新一轮下载开始前**
   - 若旧临时文件存在，先删除

2. **下载过程中失败**
   - chunk 超时
   - 长度错误
   - LFS 写失败
   - 会话断开  
   以上都应删除临时文件

3. **传输校验失败**
   - checksum 不一致
   - 总长度不一致  
   删除临时文件

4. **OTA 校验态失败**
   - 包头错误
   - 包结构错误
   - 签名失败
   - 镜像非法  
   删除临时文件

5. **OTA 安装流程失败**
   - 读取临时文件失败
   - 解包失败
   - 写槽位失败  
   第一版也建议删除临时文件

6. **OTA 安装成功**
   - 安装完成后删除临时文件

#### 13.11.4 第一版不必引入“正式文件名”

当前 OTA 模型下，LFS 文件只是 OTA 安装输入，而不是长期资产。  
因此第一版不必走：

```text
临时文件 -> 改名 -> 正式文件
```

更简单的做法是：

```text
始终写固定临时文件
-> 校验通过后直接交给 OTA 安装流程读取
-> 安装成功或失败后都删除
```

#### 13.11.5 第 9 项仍需注意的边界问题

除了基本清理时机外，第 9 项还有这些边界点需要后续编码时明确：

1. **文件打开失败后的回收**
   - 创建临时文件失败时是否需要先尝试删除旧文件再重建

2. **写到一半掉电**
   - 下次上电进入下载前，必须先检查并删除残留临时文件

3. **校验通过但 OTA 尚未开始前复位**
   - 若系统在“传输校验通过、尚未进入 OTA 校验态”之间复位，启动时如何识别并清理旧临时文件

4. **OTA 失败样本是否保留**
   - 当前建议默认删除
   - 若后续需要调试保留样本，可加调试开关，而不是改默认产品逻辑

5. **临时文件与当前 requestId 的绑定**
   - 第一版虽然用固定文件名，但日志中仍建议打印当前 `requestId`，方便定位是哪一轮下载生成的临时文件
   - 这里的 `requestId` 只是**本轮下载事务内的日志关联号**，不是跨重启、跨长期运行的持久身份标识
   - 它的价值在于把“请求 chunk、收到 chunk、checksum 失败、删除临时文件”这些日志串成同一轮过程

6. **清理失败时的二次处理**
   - 若 `lfs_remove()` 本身失败，是否记录错误并阻止下一轮下载，还是继续尝试覆盖创建

7. **正常不升级场景**
   - 若版本比较后决定“不升级”，应保证不会误复用上一次残留临时文件

8. **多阶段失败的优先日志**
   - 若“传输校验失败”和“临时文件删除失败”同时出现，日志应先报告主失败原因，再补充清理失败

第 9 项可以用一句话概括：

```text
临时文件的生命周期
必须严格绑定当前这一次下载会话
```

#### 13.11.6 `requestId` 在日志中的作用边界

需要明确：

```text
requestId 递增
!= 它没有价值
```

第一版里，`requestId` 的作用不是做“持久文件身份号”，而是做：

```text
单轮下载过程里的日志关联号
```

例如：

```text
TB: request chunk requestId=24 chunk=0 file=tb_fw.tmp
TB: request chunk requestId=24 chunk=1 file=tb_fw.tmp
TB: checksum failed requestId=24 file=tb_fw.tmp
TB: remove temp file requestId=24 file=tb_fw.tmp
```

这样可以看出：

1. 当前 `tb_fw.tmp` 属于哪一轮下载流程
2. checksum 失败和删除动作属于同一轮事务
3. 若后续又出现 `requestId=25`，说明已经切到下一轮下载

但也必须明确它的限制：

```text
requestId 只适合做本轮日志关联
不适合做跨重启的持久身份恢复
```

因此：

1. **日志层面**可以打印 `requestId + file`
2. **系统恢复层面**不能依赖 `requestId` 日志来恢复文件归属

#### 13.11.7 “按平台下发命名”和“本地临时文件命名”的边界

这里确实容易和前面讨论过的“按拉取下来的信息命名”混在一起，第一版建议把两者明确区分：

**1. 传输阶段 / 下载阶段**

```text
本地统一使用固定临时文件名
```

例如：

```text
tb_fw.tmp
```

原因是：

1. 下载阶段的文件本质上仍是临时文件
2. 此时它还没有通过传输校验和 OTA 合法性校验
3. 固定临时名最便于失败清理和命名回收

**2. 平台下发的 `fw_title` / `fw_version` / 其他命名信息**

这些信息更适合用于：

1. 日志打印
2. 界面显示
3. 业务决策
4. 后续若需要扩展“正式归档文件名”时再使用

而不是第一版就直接作为下载落盘文件名。

所以当前第一版口径可以定为：

```text
平台命名信息
-> 用于日志和业务展示

本地下载文件名
-> 固定临时名
```

#### 13.11.8 两者是否矛盾

第一版里这两件事并不矛盾，因为它们发生在不同阶段：

```text
平台命名 = 业务属性
本地临时文件名 = 下载落盘策略
```

只有当后续你想做“校验通过后，把临时文件转成带版本号/标题的正式归档文件”时，平台命名才会真正参与文件命名。

但当前方案里：

```text
下载文件始终是临时文件
-> 校验通过后直接交给 OTA 安装
-> 安装后删除
```

所以第一版不需要把平台命名直接映射成 LFS 文件名。

### 13.11.9 第一版升级策略

第一版不考虑复杂恢复策略，直接采用最保守、最容易保证一致性的升级策略：

```text
上电
-> 若发现残留临时文件，直接删除
-> 重新联网
-> 重新请求固件属性
-> 重新判断是否需要升级
-> 若需要，则从头开始一次新的下载
```

这意味着第一版明确放弃以下能力：

1. 不复用上次掉电前未完成的下载半成品
2. 不复用“传输校验已通过但尚未进入 OTA 校验态”的完整临时文件
3. 不实现断点续传
4. 不实现掉电后继续安装上次下载包

第一版这样设计的原因是：

1. 不需要 EEPROM 额外保存中间下载状态
2. 不需要区分“残留文件是半成品还是完整品”
3. 不需要恢复 `requestId`、`chunkIndex`、`received_bytes` 等上下文
4. 能最大限度避免“中间态判断错误”带来的歧义和脏状态

因此，第一版可以用一句话概括为：

```text
所有中间态一律作废
重新开始一轮完整升级判断
```

### 13.11.10 这里真正要解决的问题是什么

之所以要单独讨论升级策略，是因为设备在下载阶段和安装阶段之间可能出现多种“中间态”：

1. **下载未完成时掉电**
   - LFS 中留下半个临时文件
   - 当前下载到了哪个 chunk 已丢失

2. **下载完成但传输校验未完成时掉电**
   - 文件可能完整，也可能尚未完成最终 checksum 判断

3. **传输校验通过但尚未进入 OTA 校验态时掉电**
   - 文件可能已经是“传输正确的完整文件”
   - 但系统没有记录“它是否已经被确认可以继续安装”

4. **OTA 校验态中掉电**
   - 文件存在，但还不确定是否已经完成包合法性校验

5. **OTA 安装过程中掉电**
   - 下载文件是否保留
   - 当前槽位状态是否已改变

这些问题的共同点是：

```text
文件可能还在
但设备已经失去了它处于哪一个阶段的上下文记忆
```

第一版选择的策略就是：

```text
不尝试恢复这些中间态
只要发现残留临时文件，就删除
```

这并不是说这些问题不存在，而是说：

```text
第一版先通过“丢弃中间态”来避免处理中间态
```

### 13.11.11 后续如果要实现断点续传，需要解决什么问题

后续若要实现断点续传，就不能再简单地“删文件重来”，而必须解决一整套状态恢复问题。

断点续传并不只是“文件还在就继续下”，至少要解决下面这些问题：

#### 1. 如何确认残留文件属于当前这轮升级

需要知道：

1. 这个文件对应哪一次下载任务
2. 它对应的平台版本是什么
3. 它对应的 `requestId`、`fw_version`、`fw_size`、`fw_checksum` 是否与当前平台信息一致

否则设备可能把“旧版本残留文件”误当成当前版本继续下载。

#### 2. 如何确认文件已经下载到哪里

至少要恢复：

1. 已完成的 `chunkIndex`
2. `received_bytes`
3. 当前文件长度
4. 最好还有“哪些块已完成”的记录

否则就无法知道应该从哪一块继续请求。

#### 3. 如何保证已写入部分没有损坏

如果设备掉电时正在写文件，恢复后需要判断：

1. 文件长度是否可信
2. 已写入部分是否需要重新计算局部校验
3. 是否存在“最后一块写了一半”的情况

否则断点续传可能建立在一个已损坏的临时文件之上。

#### 4. 如何处理 `requestId` 变化

ThingsBoard 当前流程里，chunk 请求使用的是：

```text
v2/fw/request/<requestId>/chunk/<chunkIndex>
```

掉电重连后，新一轮请求很可能会使用新的 `requestId`。  
因此断点续传不能简单依赖旧 `requestId`，而应把它看作：

```text
本轮会话级标识
而不是断点续传的持久身份号
```

真正持久的身份应更多依赖：

1. `fw_version`
2. `fw_size`
3. `fw_checksum`
4. 本地记录的下载进度

#### 5. 如何协调“断点续传”和“传输校验”

断点续传不是只把剩余块下完就结束，最终仍必须重新确认：

1. 总字节数是否等于 `fw_size`
2. 整体 checksum 是否与平台一致

也就是说：

```text
断点续传恢复的是下载过程
最终校验仍必须对完整文件重新做一次
```

#### 6. 如何处理掉电后“文件已完整但未安装”的情况

这和“继续下载”不同，它属于：

```text
复用完整残留文件
继续进入 OTA 校验或安装流程
```

若后续要支持这一能力，还要再解决：

1. 如何标记“下载已完成”
2. 如何标记“传输校验已通过”
3. 上电后如何区分“完整未安装文件”和“普通残留半成品”

#### 7. 需要哪些持久化状态

如果要真正做断点续传，通常需要 EEPROM 或其他掉电保持区保存至少以下状态：

1. 当前是否存在有效下载会话
2. 对应 `fw_version`
3. 对应 `fw_size`
4. 对应 `fw_checksum`
5. 已完成到哪个 chunk
6. 已写入多少字节
7. 临时文件是否已通过传输校验

没有这些持久状态，就很难安全地做掉电恢复。

#### 8. 断点续传不只是传输问题，也是升级策略问题

需要明确区分：

1. **重新下载**
   - 删除旧文件
   - 从头开始

2. **断点续传**
   - 文件未完成
   - 从上次中断位置继续下

3. **复用完整残留文件**
   - 文件已完整下载
   - 不继续下，而是继续校验或安装

这三者属于不同策略，后续实现时必须明确选择，而不能混在一起。

### 13.11.12 第一版与后续增强版的边界

因此可以把当前方案和后续增强方向明确划分为：

**第一版：**

```text
不恢复中间态
不做断点续传
不复用残留临时文件
一律删除后重新开始升级判断
```

**后续增强版：**

```text
增加持久状态记录
支持断点续传
支持复用完整残留文件
支持掉电后继续进入 OTA 校验或安装流程
```

## 20. 第一版最终实施清单

结合前文所有讨论，第一版实际落地时可以直接按下面这份清单执行。

截至 2026-07-23，以下协议层内容已经实现：

1. `transport_getdata()` 已改为 non-blocking / short-read，删除内部 1500 ms 超时和 512 字节中转缓存。
2. `core_mqtt` 已切换到 `MQTTPacket_readnb()`，半包状态由模块级 `MQTTTransport` 保存。
3. `mqtt_poll_once()` 已统一处理 `CONNACK/SUBACK/UNSUBACK/PUBACK/PUBLISH/PINGRESP`。
4. `connect/subscribe/unsubscribe/QoS1 publish` 已接入单一 ACK 等待上下文。
5. PUBLISH 已按二进制 payload 深拷贝到两节点环形队列，入队成功后才对 QoS1 回复 `PUBACK`。
6. `mqtt_message_available()` 和 `mqtt_message_pop()` 已提供给业务层。
7. `main.c` 已切换为固件属性主动请求 smoke test：订阅 `attributes/response/+`，以 QoS1 发布 `attributes/request/1`，并从队列匹配 `response/1`。
8. smoke test 已扩展为完整固件下载：解析 `fw_size`，订阅 `v2/fw/response/+/chunk/+`，逐块请求512字节，按响应 topic 和预期长度校验后写入 `mqtt_fw.tmp`，最后执行 LFS sync 和文件大小核对。最后一块仍请求512字节，但只按平台实际返回的剩余长度写入。

尚未实现的是 ThingsBoard 业务层切换：属性响应 topic 路由、固件 `requestId/chunkIndex` 幂等检查、LFS 写入和新 MQTT 后端重新接入 OTA 状态机。

### 20.1 MQTT 下行接收主链路

1. 保留 `ESP8266` 层 `+IPD -> s_tcp_rx_ring` 的纯 TCP 字节流提取方案。
2. 保留 `transport` 层，但改成 **non-blocking / short-read** 风格：
   - 有数据返回 `>0`
   - 当前没数据返回 `0`
   - 错误返回 `<0`
   - 不在 `transport_getdata()` 内部做超时等待
3. `core_mqtt` 从 `MQTTPacket_read()` 切换到 `MQTTPacket_readnb()`。
4. 在 `core_mqtt` 中引入模块级：
   - `MQTTTransport s_mqtt_trp`
   - MQTT 整包缓冲 `s_mqtt_packet_buf`
5. MQTT 半包续收状态统一由 `MQTTTransport` 保存，不在业务层做半包拼接。

### 20.2 MQTTTransport 与等待机制

1. 增加 `mqtt_transport_state_reset()`，负责 reset `MQTTTransport` 状态。
2. 每次新 MQTT 会话开始前 reset 一次。
3. `MQTTPacket_readnb()` 返回 `0` 时，视为“还没收齐”，继续等待，不 reset。
4. 超时、断线、`readnb()` 返回 `<0`、协议反序列化失败时，视为当前未完成包不可信，执行 reset。
5. `wait_ctx` 继续作为同步等待控制块，只负责 ACK：
   - `CONNECT -> CONNACK`
   - `SUBSCRIBE -> SUBACK`
   - `UNSUBSCRIBE -> UNSUBACK`
   - `QoS1 PUBLISH -> PUBACK`
6. 上层同步等待函数不依赖 `mqtt_poll_once()` 返回值判断成功，而是依赖：
   - `s_wait_ctx.done`
   - `s_wait_ctx.result`

### 20.3 `packet_id`、ACK 与消息队列

1. `packet_id` 分配器内部允许无符号回绕，但**不能把 0 分配给 MQTT 报文**。
2. `ACK` 类报文统一进入 `wait_ctx`。
3. `PUBLISH` 类报文统一进入 MQTT 下行消息队列。
4. 队列节点采用“先写完整节点，最后提交索引”的方式入队。
5. 入队前清零节点，出队后也清零节点，保持防御式编程。
6. 队列满时：
   - 打印错误
   - 不提交新节点
   - 对 QoS1 下行不发送成功 `PUBACK`

### 20.4 QoS1、重复包与幂等

1. QoS1 下行允许重复到达，这是协议正常现象。
2. `dup=1` 只作为重复嫌疑，不作为唯一判据。
3. 固件块业务最终按：

```text
requestId + chunkIndex
```

做幂等判断。

4. 首次到达的块：
   - 正常处理
   - 成功后发送 `PUBACK`

5. 已处理过的重复块：
   - 不重复写 LFS
   - 不重复执行业务
   - 但补发 `PUBACK`

### 20.5 ThingsBoard 固件块接收规则

1. 当前下载会话只接受一个 `request_id`。
2. 当前时刻只接受一个 `expected_chunk_index`。
3. 收到固件块后按顺序校验：
   - topic 是否能解析出 `requestId/chunkIndex`
   - `requestId` 是否匹配当前会话
   - `chunkIndex` 是否等于当前期望块
   - `payload_len` 是否等于当前期望长度
4. 只有“当前会话、当前块号、当前长度都匹配且写文件成功”时，才推进：

```text
received_bytes += payload_len
expected_chunk_index++
```

### 20.6 版本决策、传输校验与 OTA 校验边界

第一版边界明确如下：

1. **是否要升级**
   - 在 ThingsBoard 属性阶段决定
   - 比较平台版本与本地当前版本

2. **传输校验**
   - 只负责文件是否传坏
   - 例如：
     - `received_bytes == fw_size`
     - chunk 顺序正确
     - checksum 匹配

3. **OTA 校验态**
   - 只负责包是否合法可安装
   - 例如：
     - 包头
     - 包结构
     - 签名
     - 镜像合法性

### 20.7 临时文件策略

1. 第一版下载文件统一使用固定临时文件名。
2. 平台下发的 `fw_title/fw_version` 用于日志和业务展示，不直接作为第一版落盘文件名。
3. 下载文件在传输校验通过前，始终视为临时文件。
4. OTA 校验或安装失败后，也删除临时文件。
5. 新一轮下载开始前，先删除残留临时文件。

### 20.8 第一版升级策略

第一版不实现：

1. 断点续传
2. 复用残留临时文件
3. 掉电后继续安装上次下载文件
4. EEPROM 中间状态恢复

第一版统一策略为：

```text
上电发现残留临时文件
-> 直接删除
-> 重新联网
-> 重新请求属性
-> 重新判断是否升级
-> 若需要，再从头开始一次新下载
```

### 20.9 日志与错误处理

1. 协议层日志使用 `MQTT:` 前缀。
2. ThingsBoard/固件业务层日志使用 `TB:` 前缀。
3. 日志区分：
   - 协议错误
   - topic 路由错误
   - JSON 字段错误
   - 业务校验错误
4. 主失败原因优先打印，清理失败作为补充日志。
5. 固定临时文件名场景下，日志中可打印：

```text
requestId + file
```

其作用只是帮助串联**本轮下载事务内**的日志，不作为跨重启持久身份标识。

### 20.10 第一版暂不做的增强项

以下内容明确留到后续增强版，不进入第一版实现范围：

1. 自动重连
2. 持久 MQTT 会话恢复
3. 多请求并发等待表
4. 断点续传
5. 复用完整残留临时文件
6. EEPROM 持久化下载进度
7. 更复杂的队列溢出恢复策略
8. MQTT 半包恢复后的跨重启继续解帧

## 21. 后续编码顺序建议

为了降低改动风险，建议实际代码按下面顺序推进：

1. 先把 `transport_getdata()` 改成 non-blocking / short-read
2. 再把 `core_mqtt` 从 `MQTTPacket_read()` 切到 `MQTTPacket_readnb()`
3. 引入 `MQTTTransport` 模块级状态和 reset 逻辑
4. 实现统一 `mqtt_poll_once()` 分发
5. 接入 `wait_ctx` 与 ACK 匹配
6. 实现 PUBLISH 消息队列与 QoS1 `PUBACK`
7. 接入 ThingsBoard 固件块幂等校验
8. 接入固定临时文件与清理策略
9. 最后再把完整链路接回 OTA 状态机

建议第一阶段提供以下接口：

```c
int mqtt_poll_once(void);
int mqtt_poll(uint32_t timeout_ms);
int mqtt_message_available(void);
int mqtt_message_pop(mqtt_rx_msg_t *msg);
```

其中：

1. `mqtt_poll_once()` 读取并分发一个 MQTT 报文。
2. `mqtt_poll()` 在给定时间窗口持续处理报文。
3. `mqtt_message_available()` 查询 PUBLISH 队列是否有消息。
4. `mqtt_message_pop()` 将队首消息复制给调用者并推进读索引。

考虑到单个消息节点可能超过 600 字节，应用层不宜在深层调用栈中临时创建多个 `mqtt_rx_msg_t`。可以由业务模块提供静态接收对象，或后续增加 `peek/release` 接口减少一次整节点复制。

## 14. ThingsBoard OTA 在新模型中的数据流

新模型下，ThingsBoard OTA 流程如下：

```text
1. mqtt_connect()
   CONNECT -> 等 CONNACK

2. mqtt_subscribe_topic("v1/devices/me/attributes/response/+", ...)
   SUBSCRIBE -> 等匹配 SUBACK

3. mqtt_subscribe_topic("v1/devices/me/attributes", ...)
   SUBSCRIBE -> 等匹配 SUBACK

4. 发布固件属性请求
   PUBLISH(v1/devices/me/attributes/request/<requestId>)
   -> QoS1 时先等 PUBACK
   -> 持续 poll
   -> 从消息队列取得 attributes/response/<requestId>

5. 比较远端版本和本地已确认版本

6. 需要升级时订阅
   v2/fw/response/+/chunk/+

7. 串行请求每个固件块
   PUBLISH(v2/fw/request/<requestId>/chunk/<chunkIndex>, "512")
   -> 等 PUBACK
   -> 等对应 response PUBLISH 入队
   -> 校验 requestId 和 chunkIndex
   -> 写入 LFS
   -> 再请求下一块

8. 完成整文件 checksum、包签名和 OTA 状态机处理
```

Broker 负责按照设备已提交的订阅过滤器决定哪些消息可以发给当前会话。设备收到的 PUBLISH topic 是具体 topic，例如：

```text
订阅：v2/fw/response/+/chunk/+
收到：v2/fw/response/24/chunk/22
```

`core_mqtt` 不需要再次实现 MQTT `+` 通配规则；应用层只需解析收到的具体 topic，并检查其中的 `requestId` 和 `chunkIndex` 是否属于当前业务请求。

## 15. 缓冲区与 SRAM 约束

当前相关缓冲区为：

| 缓冲区 | 当前大小 | 作用 | 方案要求 |
| --- | ---: | --- | --- |
| `s_tcp_rx_ring` | 1024 | 保存从 `+IPD` 提取的纯 TCP 字节 | 能承受串口接收与上层消费速度差 |
| transport 中转缓存 | 0 | `transport_getdata()` 直接短读 TCP 环形缓冲 | 不再重复缓存和搬运 |
| `s_mqtt_rx_buf` | 672 | 模块级 MQTT 完整报文缓冲 | 可容纳 128 字节 topic、512 字节 payload 和 MQTT 头部 |
| `s_mqtt_tx_buf` | 256 | 模块级 MQTT 上行序列化缓冲 | 当前 ThingsBoard 请求和 telemetry 足够使用 |
| PUBLISH 队列 | 2 × 650 | 深拷贝 topic、二进制 payload 和 QoS 元数据 | 队列满时报错，QoS1 不回成功 `PUBACK` |

这里必须区分三种不同容量：

1. TCP 环形缓冲容量：用于吸收串口和主循环之间的速度波动。
2. MQTT 原始报文缓冲容量：必须容纳一整个编码后的 MQTT 报文。
3. PUBLISH 节点 payload 容量：只需容纳业务 payload，例如 512 字节固件块。

它们不能简单地全部设成 512。

第一阶段已经完成以下调整：

1. 将 MQTT 原始报文缓冲改为模块级静态缓冲，避免扩大线程栈占用。
2. 按“最长 topic + 512 字节 payload + MQTT 头部”计算 `MQTT_PACKET_BUF_SIZE`。
3. 修改或扩大 transport 缓冲，确保 `transport_getdata(count)` 能满足大于 512 字节的读取请求。
4. 根据实际 SRAM 余量选择 PUBLISH 队列深度；串行固件下载通常不需要很深的队列。

原始报文缓冲和队列节点在消息入队瞬间会同时存在，这是深拷贝换取解耦所需的短时 RAM 成本。后续如需继续压缩 SRAM，可考虑回调式流处理或按业务类型分配不同节点，但这会提高耦合度，暂不作为第一版目标。

当前 Keil map 中相关静态对象为：`s_mqtt_rx_buf=672`、`s_mqtt_tx_buf=256`、`s_message_queue=1300`。`main.c` 的下行 smoke test 额外使用一个 650 字节静态出队对象；正式接入 ThingsBoard 业务后，可将该对象迁移到业务模块，或改为 `peek/release` 接口避免再次复制完整节点。

本次“固件属性 + 完整分块下载到 LittleFS”smoke test 的构建结果为 `Code=58448、RO-data=2028、RW-data=504、ZI-data=10440`。这是 `ESP8266_MQTT_BACKEND_AT_ENABLE=0` 的构建结果，此时完整 OTA 状态机和 40KB `elf_buf` 没有被链接进最终映像。因此该数值不能直接代表“新 MQTT + 完整 OTA”同时启用后的最终 SRAM 占用。业务层接回 OTA 状态机时必须重新查看 map，重点确认 `elf_buf`、MQTT 队列、业务出队对象和主栈的总和不超过 SRAM1；若空间不足，优先考虑去掉 smoke-test 出队副本、引入 `peek/release`，再评估队列深度。

## 16. 实施顺序

建议按以下顺序落地：

1. 在 `core_mqtt.h` 中定义等待上下文结果、PUBLISH 消息节点和队列接口。
2. 在 `core_mqtt.c` 中增加模块级 MQTT 报文缓冲和静态消息环形队列。
3. 实现 `mqtt_poll_once()` 及 `CONNACK/SUBACK/UNSUBACK/PUBACK/PUBLISH/PINGRESP` 分支。
4. 实现 PUBLISH 深拷贝、队列提交和 QoS1 `PUBACK`。
5. 将 `mqtt_connect()`、`mqtt_subscribe_topic()`、`mqtt_unsubscribe_topic()` 改为等待上下文模式。
6. 为 QoS1 `mqtt_publish()` 增加递增 packet identifier 和 `PUBACK` 等待。
7. 增加 `mqtt_message_available()` 与 `mqtt_message_pop()`。
8. 在 ThingsBoard 业务层按具体 topic 消费属性响应和固件响应。
9. 最后接入逐块 OTA 下载，不在协议层改造期间同时调整 OTA 状态机。

## 17. 验证场景

实现完成后至少验证以下情况：

1. `CONNECT` 后能够正确解析 `CONNACK`。
2. 两个 topic 串行订阅，第一个成功、第二个超时时能准确定位失败项。
3. 等待 `SUBACK` 期间插入一条 PUBLISH，PUBLISH 仍可从队列取出。
4. QoS1 下行成功入队后发送正确 packet identifier 的 `PUBACK`。
5. topic 恰好达到上限时能正确补 `\0`，超过上限时明确拒绝。
6. payload 中包含 `0x00` 时仍按完整长度保存。
7. 512 字节固件块加 MQTT 头部后能够完整收包。
8. 队列满时不覆盖旧固件块，并输出可定位的错误。
9. `PUBACK` 发送失败后收到 `DUP=1` 重传时不会重复写入固件。
10. 超时或断连后清理等待上下文、transport 缓存和 MQTT 会话状态。

## 18. 已确认决策与待确认项

### 18.1 已确认的第一版实现口径

除前文设计外，当前又进一步确认了以下口径：

1. 队列先按主循环单线程模型实现，暂不考虑 ISR/RTOS 并发访问。
2. 队列节点出队后清零，入队写入前也先清零，走防御式编程风格。
3. 队列满时打印错误；若当前消息为 QoS1，则由于未成功入队，不进入 `PUBACK` 发送分支。
4. `wait_ctx` 在 `connect / subscribe / unsubscribe / publish(qos=1)` 中建立，收到目标 ACK 或超时后清除。
5. 等待目标 ACK 时，若收到其他 ACK，则打印日志并继续等待，不误判成功。
6. `packet_id` 分配器内部允许无符号回绕，但返回给 MQTT 报文的值必须跳过 `0`。
7. 当前先不实现自动重连；若后续发生重连，则重新执行订阅。
8. `mqtt_poll()` 第一版放在同步等待函数内部自旋调用，不单独抽后台驱动。
9. 文件块出队后再写 LFS；写失败时终止本轮下载并删除临时文件。
10. 设备工作节奏按“上电连接 -> 请求属性 -> 判断是否升级 -> 完成后断开”理解，OTA 状态机位于最上层。

### 18.2 仍未完全展开的技术细节

虽然主方案已经清晰，但还有一些实现细节尚未逐条展开或编码：

1. `wait_ctx` 中 `result`、`ack_msgid`、`granted_qos`、`connack_rc` 的最终错误码约定。
2. `mqtt_poll_once()` 各分支的具体反序列化顺序与日志格式。
3. QoS1 重复包的判定粒度：仅靠 `packet_id`，还是结合 topic 与业务字段。
4. 固件块业务层对 `requestId + chunkIndex` 的严格幂等校验流程。
5. `transport_getdata(count)` 在“只收到半包”时的精确返回约定和上层错误映射。
6. 大报文接收时，TCP 环形缓冲、transport 缓冲、MQTT 整包缓冲之间的最终尺寸表。
7. 断线、超时、半包失败之后，transport 与 MQTT 会话状态的复位顺序。
8. topic 解析失败、JSON 缺字段、固件属性不完整时的统一日志与错误码体系。
9. 写 LFS 成功但后续流程失败时，临时文件清理策略与命名回收策略。
10. 后续若引入自动重连，`clean session`、重订阅、未完成下载恢复的协作方式。

已确认的第一版决策：

1. ESP8266 只处理 WiFi、TCP 和 `+IPD` 提取。
2. 所有 MQTT 下行统一由 `mqtt_poll_once()` 读取。
3. 需要 ACK 的控制请求串行执行，一个 SUBSCRIBE 只订阅一个 topic。
4. ACK 进入当前等待上下文，PUBLISH 进入独立消息队列。
5. topic 和 payload 必须深拷贝，payload 始终按二进制处理。
6. 超限报文不能静默截断。
7. QoS1 PUBLISH 成功入队后由 `core_mqtt` 自动回复 `PUBACK`。

仍需在编码前最终确定：

1. `MQTT_PACKET_BUF_SIZE` 的具体值。
2. `MQTT_RX_TOPIC_MAX` 和 PUBLISH 队列深度。
3. transport 大包读取采用扩容还是改为更直接的分段搬运。
4. 半包超时第一版采用断线重连，还是直接引入非阻塞增量解析。
5. 重复 QoS1 PUBLISH 的统一去重策略。

## 19. 结论

本次升级的核心不是再增加一个“接收 PUBLISH 的函数”，而是改变 MQTT 数据的所有权：

```text
旧模型：调用者为了等自己的 ACK，直接读取并决定是否保留下一包。

新模型：统一收包器拥有所有下行报文，先按协议类型分发，
        同步调用者只观察等待上下文，业务层只消费 PUBLISH 队列。
```

这样可以同时保证：

1. ACK 与当前请求通过类型和 packet identifier 正确关联。
2. 等待 ACK 期间到达的异步 PUBLISH 不会丢失。
3. TCP、MQTT 与 ThingsBoard 业务边界清晰。
4. 后续属性同步和固件 OTA 可以共用同一套稳定的 MQTT 下行机制。
