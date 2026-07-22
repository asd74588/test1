# MQTT 下行升级方案讨论

## 1. 当前背景

目前底层链路已经调整为：

```text
UART RX
-> s_rx_ring
-> esp_at_process()
-> s_tcp_rx_ring
-> esp8266_sock_recv()
-> transport_getdata()
-> MQTTPacket_read()
```

也就是说，`ESP8266` 驱动层已经逐步完成了职责收敛：

1. 负责 WiFi 入网。
2. 负责 TCP socket 收发。
3. 负责把 `+IPD` 提取成纯 TCP payload。
4. 不再继续承担 MQTT 业务分发职责。

## 2. 当前 `core_mqtt` 的定位

当前 `core_mqtt` 更像是一个“同步请求式 MQTT 封装层”，主要特点是：

1. 发送一个 MQTT 请求。
2. 阻塞等待某一个指定类型的返回包。
3. 当前函数只关心本次请求对应的 ACK。

典型流程：

1. `mqtt_connect()`：发 `CONNECT`，等 `CONNACK`
2. `mqtt_subscribe_topic()`：发 `SUBSCRIBE`，等 `SUBACK`
3. `mqtt_unsubscribe_topic()`：发 `UNSUBSCRIBE`，等 `UNSUBACK`

这个结构适合：

1. 最小链路验证。
2. 先打通 MQTT 上行。
3. 快速完成 smoke test。

## 3. 当前结构的核心局限

问题不在于“没有下行解析函数”，因为当前 MQTT 库已经提供：

```text
MQTTPacket_read()
MQTTDeserialize_publish()
```

真正的问题在于：当前 `core_mqtt` 的读取模型还是“等某一种包”。

这会导致：

1. 如果此时正在等 `SUBACK`
2. Broker 却先发来一条 `PUBLISH`
3. 这条 `PUBLISH` 会被底层读走
4. 但当前逻辑没有专门保存它
5. 业务上就相当于消息丢失

因此当前局限可以概括为：

```text
当前结构只能稳定处理同步 ACK，
不能稳定处理异步下行 PUBLISH。
```

## 4. ThingsBoard 场景下为什么必须升级

在 ThingsBoard 里，真正重要的下行大多数都是 `PUBLISH`：

```text
v1/devices/me/attributes
v1/devices/me/attributes/response/<requestId>
v2/fw/response/<requestId>/chunk/<chunkIndex>
```

也就是说：

1. 属性读取响应是 `PUBLISH`
2. 属性变更推送是 `PUBLISH`
3. 固件块下发响应也是 `PUBLISH`

如果 `core_mqtt` 仍维持当前结构，那么后续 OTA、属性同步、配置更新都不够稳。

## 5. 升级目标

下一步不再是“给当前同步结构补一个收消息函数”，而是把 `core_mqtt` 升级为：

```text
统一收包 + 按类型分发 的 MQTT 会话层
```

目标数据流：

```text
transport_getdata()
-> MQTTPacket_read()
-> mqtt_dispatch_packet()
   |- ACK 状态区
   |- PUBLISH 消息队列
   |- keepalive 状态
```

## 6. 计划中的模块职责

### 6.1 驱动层

`ESP8266` 层只负责：

1. WiFi 与 TCP。
2. 提供纯净的 socket 风格收发接口。

### 6.2 传输层

`transport` 层只负责：

1. 调用 `esp8266_sock_send()`
2. 调用 `esp8266_sock_recv()`
3. 维持少量本地 TCP 拼包缓存

### 6.3 MQTT 核心层

`core_mqtt` 层负责：

1. 统一读取 MQTT 包。
2. 识别包类型。
3. 分发 ACK 与 PUBLISH。
4. 提供给应用层统一的轮询与取消息接口。

## 7. 第一阶段建议接口

建议先补齐以下接口，不急着一步到位：

```c
int mqtt_poll_once(void);
int mqtt_poll(uint32_t timeout_ms);
int mqtt_message_available(void);
int mqtt_message_pop(mqtt_rx_msg_t *msg);
```

其中：

1. `mqtt_poll_once()`：读一个完整 MQTT 包并分发。
2. `mqtt_poll()`：在给定时间窗口内循环调用 `mqtt_poll_once()`。
3. `mqtt_message_available()`：判断是否有业务下行消息。
4. `mqtt_message_pop()`：从消息队列中取出一条 `PUBLISH`。

## 8. 第二阶段建议状态

升级后至少需要两类状态：

### 8.1 ACK 状态区

用于保存：

1. `CONNACK`
2. `SUBACK`
3. `UNSUBACK`
4. `PUBACK`
5. `PINGRESP`

### 8.2 PUBLISH 消息队列

用于保存：

1. topic
2. payload
3. payload_len
4. qos
5. retained
6. packet_id

## 9. 讨论重点

接下来讨论新方案时，建议重点围绕下面几个问题展开：

1. ACK 状态区是做“最近一次状态”还是做“小型队列”？
2. `PUBLISH` 消息队列长度定多少合适？
3. topic 匹配是做精确匹配，还是支持 `+` 通配？
4. QoS1 下行时，`PUBACK` 由 `core_mqtt` 自动回复，还是交给上层决定？
5. `mqtt_connect()` / `mqtt_subscribe_topic()` 这类同步 API，是保留外观还是也改成更显式的异步风格？

## 10. 当前结论

当前底层方向已经基本正确，后续主战场不再是 `ESP8266` 驱动，而是 `core_mqtt` 的收包与分发模型。

因此下一步的代码设计重点应放在：

1. 统一收包入口
2. ACK 状态管理
3. PUBLISH 队列设计
4. 应用层如何按 topic 消费消息
