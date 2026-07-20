# ESP8266/ESP32-C3 AT 调试问题记录

## 现象

复位后执行 WiFi 入网测试，串口日志中出现：

```text
Start send AT command: AT+CWJAP="labtop","12345678"
WIFI DISCONNECT
WIFI CONNECTED
INFO: ESP8266 connect to 'labtop' ok

Start send AT command: AT+CIPSTA_CUR?
busy p...
...
WIFI DISCONNECT
+CWJAP:4
ERROR
```

同时复位阶段出现：

```text
AT+RST
OK
Start send AT command: AT
busy p...
...
ready
```

## 原因

当前模块启动日志显示：

```text
arch:ESP32C3
firmware_version:2.2.0
```

因此实际使用的是 ESP32-C3 系列 AT 固件，虽然大部分 AT 指令与 ESP8266 类似，但查询命令和返回时序需要按 ESP-AT 行为处理。

本次问题主要有两点：

1. `AT+RST` 收到 `OK` 不代表模块已经复位完成。模块后面还会输出 boot log 和 `ready`，如果收到 `OK` 后立刻发 `AT`，容易得到 `busy p...`。
2. `AT+CWJAP` 中的 `WIFI CONNECTED` 只表示连接流程中的中间状态，不代表命令完成。成功入网通常还需要等待 `WIFI GOT IP` 和最终 `OK`。原代码等待 `"CONNECTED"`，会过早进入 `AT+CIPSTA_CUR?` 查询，导致模块仍忙。

`+CWJAP:4` 表示本次入网最终失败。常见诱因包括热点不是 2.4 GHz、SSID/密码错误、认证方式不兼容、信号不稳定、电脑热点未正确开启或 DHCP 未能分配地址。

## 本次修正

已在 `Core/interface/ESP8266/esp8266.c` 中调整：

1. `AT+RST` 改为等待 `"ready"`，避免复位未完成就发送下一条 AT 命令。
2. `AT+CWJAP` 改为等待最终 `"OK\r\n"`，超时时间拉长到 30 秒。
3. 初始化阶段发送 `AT+CWAUTOCONN=0` 和 `AT+CWQAP`，减少历史自动连接状态对测试的干扰。
4. IP 查询增加兼容处理：优先尝试 ESP32-C3 固件支持的 `AT+CIPSTA?`，失败后再尝试旧版 `AT+CIPSTA_CUR?`。

## 当前验证结论

修正后复位阶段已经可以等待到 `ready`，`AT+CWJAP` 也不再因为匹配 `"CONNECTED"` 而提前返回，日志中 `busy p...` 已消失。

新的日志显示：

```text
AT+CWJAP="labtop","12345678"
WIFI CONNECTED
WIFI DISCONNECT
+CWJAP:4
ERROR
```

这说明当前软件时序问题已缓解，剩余问题转为实际热点连接失败。`AT+CIPSTA?` 返回 `0.0.0.0` 也确认模块未获得 DHCP 地址。

后续将目标热点从电脑热点 `labtop` 切换到手机热点 `first` 后，连接成功：

```text
WIFI CONNECTED
WIFI GOT IP
+CIPSTA:ip:"192.168.10.12"
+CIPSTA:gateway:"192.168.10.38"
+PING:13
+PING:143
```

因此可以确认：

1. USART2 接收、AT 命令等待、AP 扫描、入网、DHCP、Ping 流程均已跑通。
2. ESP32-C3 模块和当前驱动逻辑基本可用。
3. 电脑热点 `labtop` 的失败更可能来自 Windows 移动热点兼容性、DHCP/ICS 共享、网卡驱动或热点认证配置，而不是固件主流程。
4. 手机热点可以作为后续 WiFi OTA 接收开发的稳定基准环境。

## 推荐测试步骤

电脑开启移动热点：

```text
SSID: ESP8266_TEST
Password: 12345678
Band: 2.4 GHz
```

固件测试顺序：

```c
esp8266_module_init();
esp8266_scan_ap("ESP8266_TEST");
esp8266_join_network("ESP8266_TEST", "12345678");
esp8266_get_ipaddr(ip, gateway, sizeof(ip));
esp8266_ping_test("192.168.137.1");
```

测试代码中应按返回值逐步执行：初始化失败则停止，扫描不到目标 AP 则停止，入网失败则停止，获取不到 IP 则停止。先 ping 电脑热点网关 `192.168.137.1`，确认局域网链路正常后，再测试外网域名，例如 `www.baidu.com`。

## 后续注意

1. 第一次测试建议 SSID 和密码只使用英文、数字，避免编码问题干扰 AT 指令。
2. 如果 `AT+CWLAP` 搜不到电脑热点，优先检查电脑热点是否工作在 2.4 GHz。
3. 如果能 `WIFI CONNECTED` 但没有 `WIFI GOT IP`，重点检查电脑热点 DHCP 和防火墙设置。
4. 如果后续要做 OTA 接收，不建议继续用字符串函数直接处理完整 `+IPD` payload，因为固件数据可能包含 `0x00`。

## 电脑热点替代软件

Windows 自带移动热点在部分网卡/驱动/ICS 共享组合下可能出现 ESP 模块能扫描、短暂连接但无法获得 IP 的问题。可选方案：

1. 猎豹免费 WiFi：国产热点共享工具，适合作为 Windows 自带热点失败时的对照测试。
2. WiFi 共享大师：国产免费 WiFi 热点软件，主打利用电脑无线网卡共享热点。
3. MyPublicWiFi：非国产，但有中文页面，功能更偏完整热点/防火墙/带宽管理。

注意：这类工具可能包含广告或附带组件，应优先从官网、微软商店或可信软件中心下载。若后续长期做 WiFi OTA 调试，更推荐使用独立 2.4 GHz 小路由器或随身路由器，稳定性通常高于电脑虚拟热点。

## 电脑 TCP/ThingsBoard 联调拓扑

电脑不一定需要自己开启热点。只要电脑和 ESP32-C3 在同一个局域网内，ESP32-C3 就可以访问电脑上的 TCP 服务或 ThingsBoard。

推荐拓扑：

```text
手机热点 / 小路由器
    ├── 电脑：连接热点，运行 TCP 服务或 ThingsBoard
    └── ESP32-C3：连接同一个热点
```

本次手机热点测试中：

```text
电脑 IP: 192.168.10.11
ESP IP: 192.168.10.38
```

因此 ESP32-C3 需要主动连接电脑：

```c
esp8266_sock_connect("192.168.10.11", 8080);
esp8266_sock_send((unsigned char *)"hello from esp32\r\n", 18);
esp8266_sock_disconnect();
```

当前 `Core/Src/main.c` 的临时测试流程已加入 TCP 发送测试：

```c
char *tcp_server_ip = "192.168.10.11";
int tcp_server_port = 8080;
unsigned char tcp_msg[] = "hello from esp32\r\n";
```

如果电脑 TCP 服务端口不是 `8080`，修改 `tcp_server_port` 即可。

电脑端 TCP 服务必须监听在：

```text
0.0.0.0:<port>
```

或：

```text
192.168.10.11:<port>
```

如果只监听 `127.0.0.1:<port>`，ESP32-C3 无法连接。

Windows 防火墙需要允许对应端口入站，例如：

```powershell
New-NetFirewallRule -DisplayName "ESP TCP Test 8080" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8080
```

ThingsBoard 常用端口：

```text
MQTT: 1883
HTTP/API/UI: 8080 或 Docker 映射端口 9090
CoAP: 5683/UDP
```

在接入 ThingsBoard 前，建议先用普通 TCP 服务验证 `ESP -> 电脑` 链路。链路成功后，再替换为 ThingsBoard 的 MQTT/HTTP 协议。

## ThingsBoard MQTT 接入方案

TCP 连通后，接入 ThingsBoard 不建议继续使用自定义裸 TCP 协议。ThingsBoard 设备接入更适合使用 MQTT，其中设备访问令牌作为 MQTT username，常用端口为 `1883`。

当前项目采用 ESP-AT 固件自带 MQTT 指令，避免在 STM32 侧实现完整 MQTT 协议栈，减少 Flash 和 RAM 占用。

代码修改位置：

```text
Core/interface/ESP8266/esp8266.h
Core/interface/ESP8266/esp8266.c
Core/Src/main.c
```

新增 ESP-AT MQTT 封装接口：

```c
int esp8266_mqtt_connect(char *host, int port, char *access_token);
int esp8266_mqtt_disconnect(void);
int esp8266_mqtt_publish_raw(char *topic, unsigned char *data, int bytes);
int esp8266_thingsboard_publish_telemetry(unsigned char *json, int bytes);
```

底层 AT 指令流程：

```text
AT+MQTTCLEAN=0
AT+MQTTUSERCFG=0,1,"stm32_ota","<ACCESS_TOKEN>","",0,0,""
AT+MQTTCONN=0,"192.168.10.11",1883,0
AT+MQTTPUBRAW=0,"v1/devices/me/telemetry",<len>,1,0
> JSON payload
```

使用 `AT+MQTTPUBRAW` 而不是 `AT+MQTTPUB`，是为了避免 JSON 字符串中的双引号需要额外转义。

`main.c` 中当前测试参数：

```c
char *thingsboard_host = "192.168.10.11";
int thingsboard_mqtt_port = 1883;
char *thingsboard_access_token = "PUT_DEVICE_ACCESS_TOKEN_HERE";
unsigned char thingsboard_telemetry[] = "{\"fw_test\":1,\"wifi_rssi\":-34}";
```

正式测试前需要将 `thingsboard_access_token` 替换为 ThingsBoard 设备详情中的 Access Token。

ThingsBoard telemetry topic 固定为：

```text
v1/devices/me/telemetry
```

电脑端需要确认：

1. ThingsBoard 已启动。
2. MQTT 端口 `1883` 已监听。
3. Windows 防火墙允许 `1883/TCP` 入站。
4. 如果 ThingsBoard 运行在 Docker 中，需要确认 `1883:1883` 端口映射存在。

防火墙放行示例：

```powershell
New-NetFirewallRule -DisplayName "ThingsBoard MQTT 1883" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 1883
```

## ThingsBoard OTA 元信息请求阶段

当前阶段只实现“询问平台是否给设备分配了固件”，不下载固件内容。设备端通过 MQTT 订阅 ThingsBoard attributes topic，并主动请求 OTA shared attributes。

新增接口：

```c
int esp8266_mqtt_subscribe(char *topic, int qos);
int esp8266_thingsboard_request_firmware_info(void);
```

设备端订阅：

```text
v1/devices/me/attributes/response/+
v1/devices/me/attributes
```

设备端请求：

```text
topic: v1/devices/me/attributes/request/1
payload:
{"sharedKeys":"fw_title,fw_version,fw_size,fw_checksum,fw_checksum_algorithm"}
```

如果平台已经给设备分配固件，期望收到的响应中应包含：

```text
fw_title
fw_version
fw_size
fw_checksum
fw_checksum_algorithm
```

当前实现只打印收到的 MQTT attributes 响应，并根据是否包含 `fw_title` 判断是否发现固件信息。它不会请求：

```text
v2/fw/request/<requestId>/chunk/<chunkIndex>
```

因此不会下载固件包，也不会写入 Flash 或 LittleFS。

后续阶段再实现固件 chunk 下载、写入、校验和 OTA 状态机切换。

### Design Rationale: 使用 ESP-AT MQTT 指令而非移植 MQTT 协议库

该设计决策源于 WiFi OTA 接入 ThingsBoard 时的协议栈分工问题。当前硬件链路为：

```text
STM32
    -> USART2 AT 指令
    -> ESP32-C3 AT 固件
    -> MQTT 协议栈
    -> TCP/IP
    -> WiFi
    -> ThingsBoard
```

在此架构下，MQTT CONNECT、SUBSCRIBE、PUBLISH、PINGREQ/PINGRESP、QoS 状态维护等协议细节均由 ESP32-C3 AT 固件内部完成。STM32 只需要发送 ESP-AT 提供的 MQTT 指令：

```text
AT+MQTTUSERCFG
AT+MQTTCONN
AT+MQTTPUBRAW
AT+MQTTSUB
AT+MQTTCLEAN
```

因此，当前阶段不需要在 STM32 侧移植独立 MQTT 协议库。

项目比较了两种实现路线：

| 方案 | 实现思路 | 取舍 |
| --- | --- | --- |
| 使用 ESP-AT MQTT 指令 | STM32 通过 AT 指令调用 ESP32-C3 固件内部 MQTT 能力 | Flash/RAM 占用小，实现快；受 ESP-AT 指令能力和返回格式约束 |
| STM32 移植 MQTT 协议库 | STM32 自己构造 MQTT 报文，通过 TCP socket 或透传 WiFi 模块发送 | 协议控制力强，跨模块能力好；需要额外 Flash/RAM，并维护 MQTT 状态机 |

当前项目采用第一种方案，原因是：

1. 目标 MCU Flash 和 RAM 已经比较紧张。
2. ESP32-C3 AT 固件已经验证支持 MQTT 连接、Telemetry 上报、Topic 订阅和 attributes 请求。
3. ThingsBoard OTA 的下一阶段可以继续使用 `AT+MQTTSUB` 和 `AT+MQTTPUBRAW` 请求固件 chunk，无需先引入 MQTT 协议库。

MQTT 协议库仍然有适用场景。如果后续改为以下架构，则需要重新考虑移植 MQTT 库：

```text
STM32 + W5500 以太网
STM32 + 4G 模块 TCP 透传
STM32 + ESP8266/ESP32 透明传输模式
STM32 + lwIP socket
```

这些场景中，外部模块只提供 TCP 通道，不再替 STM32 完成 MQTT 报文封装，主控就必须自己维护 MQTT CONNECT、SUBSCRIBE、PUBLISH、keepalive、QoS 和重连流程。

当前方案仍需注意一个边界：不需要 MQTT 协议库，并不代表可以把 MQTT 收到的数据都当作字符串处理。ThingsBoard OTA 固件 chunk 是二进制数据，可能包含 `0x00`。后续下载固件时，必须基于 ESP-AT 返回的 `+MQTTSUBRECV:<LinkID>,"<topic>",<data_length>,<data>` 中的 `<data_length>` 做长度解析和拷贝，不能使用 `strlen`、`strstr` 等字符串函数处理固件 payload。

### Problem and Handling: AT 命令清空缓冲区可能丢失异步 MQTT 数据

Problem:
当前 `send_atcmd()` 在发送每条 AT 命令前会调用 `clear_atcmd_buf()` 清空 `g_wifi_rxbuf`。该处理适合 `AT`、`AT+CWJAP`、`AT+PING` 等同步命令，但在 MQTT 订阅场景下存在风险。ThingsBoard 可以在任意时刻通过订阅 topic 下发 `+MQTTSUBRECV`，如果该数据已经进入接收缓冲区但尚未被业务层处理，下一次 AT 命令前的清空操作会直接丢弃这条消息。

Cause analysis:
当前接收模型将 AT 命令响应和 MQTT 异步消息混放在同一个全局缓冲区 `g_wifi_rxbuf` 中。同步命令依赖“清空缓冲区 -> 发送命令 -> 等待指定字符串”的流程判断成功与失败；而 MQTT 下发消息不一定与当前命令严格同步，可能与 `+MQTTPUB:OK`、`OK` 等响应交错到达。本项目在请求 ThingsBoard OTA 元信息时已经观察到 `+MQTTSUBRECV` 可能早于 `+MQTTPUB:OK` 出现在接收缓冲区中。

Handling:
当前阶段只请求 OTA 元信息，不下载固件。代码已经重构为“USART2 中断接收 -> ring buffer -> ESP-AT 解析器 -> MQTT 事件队列”的模型。`send_atcmd()` 仍可清空 `g_wifi_rxbuf` 用于显示和同步响应记录，但不再直接清除 USART2 ring buffer 中尚未解析的数据；`+MQTTSUBRECV` 会被解析成独立事件，等待函数再从事件队列中取出。

Effect:
该处理避免了 AT 同步命令响应和 MQTT 异步下行数据混用同一个缓冲区的问题。当前 ThingsBoard telemetry 上报和 OTA 固件元信息请求已经验证可用。

Remaining risk:
固件 chunk 是二进制数据，后续必须继续基于 ESP-AT 返回的长度字段做解析，不能依赖字符串搜索或 `strlen`。

### Problem and Handling: `+MQTTSUBRECV` payload 边界解析偶发失败

Problem:
在 ThingsBoard attributes 请求调试中，同一份程序偶尔能够成功收到固件元信息，偶尔只打印：

```text
MQTT subrecv header topic [v1/devices/me/attributes/response/1], payload_len 180
```

随后没有 `MQTT subrecv payload done`，上层也无法从事件队列中取到固件信息。该现象说明 MQTT 下行 topic 和 payload 长度已经被识别，但 payload 数据没有完整进入事件队列。

Cause analysis:
ESP-AT 的 MQTT 下行格式为：

```text
+MQTTSUBRECV:<LinkID>,"<topic>",<data_length>,<data>
```

旧解析逻辑在识别到 `<data_length>` 后面的逗号时，立即认为 header 结束，并将 `s_line_buf` 清空，然后从后续 UART 字节开始按 `<data_length>` 收 payload。这个逻辑依赖处理边界刚好落在 header 和 payload 之间。如果主循环处理稍晚，逗号后的部分 payload 已经进入 `s_line_buf`，清空 line buffer 就可能丢掉这部分 payload，导致后续按长度等待时无法凑满完整数据。

Handling:
代码将 payload 追加过程抽象为 `mqtt_payload_append_byte()`。在解析到 `+MQTTSUBRECV` header 后，不再直接丢弃逗号后已经缓存的内容，而是先把 `s_line_buf` 中 header 后面的已有数据作为 payload 追加，再切换到 `ESP_PARSE_MQTT_PAYLOAD` 状态继续接收剩余字节。

后续多次连续运行测试时，又观察到一次成功、下一次失败的现象。该现象说明除了 payload 边界外，驱动内部状态也需要在模块复位前同步复位。因此代码增加 `esp8266_driver_reset_state()`，在 `esp8266_module_init()` 中执行 `HAL_UART_AbortReceive()` 后清理 ring buffer、line buffer、命令上下文、MQTT 事件队列和 payload 解析状态，避免上一次测试残留影响下一轮。

同时，解析到 `+MQTTSUBRECV` header 后不再立即打印 header 日志。header 与 payload 之间属于串口突发接收敏感区，阻塞式 `printf` 虽然走 USART1，但仍会增加主循环处理延迟。当前只在 payload 完整接收并入队后打印确认信息。

Effect:
修正后日志中可以稳定看到：

```text
MQTT subrecv payload done topic [v1/devices/me/attributes/response/1], 180 bytes
MQTT event queued topic [v1/devices/me/attributes/response/1], 180 bytes
MQTT event popped topic [v1/devices/me/attributes/response/1], 180 bytes
INFO: ThingsBoard firmware info found
```

这说明 USART2 ring buffer、ESP-AT 下行解析、MQTT 事件队列和 ThingsBoard 固件元信息判断链路已经打通。

Remaining risk:
当前验证对象是 JSON 格式的 OTA 元信息。后续下载固件 chunk 时，payload 可能是二进制数据，仍需要保证解析器完全按 `<data_length>` 处理，避免使用 `strstr`、`strlen` 等字符串逻辑处理固件内容。

### Problem and Handling: attributes 请求发布成功后未立即看到响应

Problem:
在一次连续测试中，设备完成 WiFi 连接、MQTT 连接、telemetry 上报和 attributes 请求发布，日志停在：

```text
INFO: ESP8266 MQTT publish [78] bytes to topic [v1/devices/me/attributes/request/1] ok
```

后续日志中暂未出现 `+MQTTSUBRECV`、`ThingsBoard firmware info found` 或 `ThingsBoard firmware info request timeout`。

Cause analysis:
该现象与前面的 payload 边界问题不同。前面的边界问题至少已经看到 `+MQTTSUBRECV` header；本次日志中连 `+MQTTSUBRECV` 都没有出现，说明当前截取的日志只证明 attributes 请求已经发布成功，尚不能证明平台响应已经到达设备。若继续等待超过 `esp8266_mqtt_wait_subrecv()` 的 8000 ms 超时时间，代码应打印超时信息。

连续调试时固定使用 `v1/devices/me/attributes/request/1` 虽然协议上可以表达请求，但不利于区分多次请求与响应，也容易在日志分析中混淆。ThingsBoard 的 requestId 本质上是设备侧用于匹配 response topic 的编号。

Handling:
代码将 ThingsBoard attributes 请求 topic 从固定 `request/1` 改为递增 requestId：

```text
v1/devices/me/attributes/request/<requestId>
v1/devices/me/attributes/response/<requestId>
```

设备仍订阅 `v1/devices/me/attributes/response/+`，因此不需要每次为不同 requestId 重新订阅。发布请求后增加等待提示日志，用于区分“请求已经发出，正在等待平台响应”和“解析器已经收到响应但 payload 未完成”两类问题。

后续调试中发现，设备已经使用唯一 MQTT clientId，且 attributes request 发布成功后，平台响应仍可能没有立即下发。为提高网络和平台侧偶发延迟下的稳定性，代码增加以下策略：

1. MQTT 连接成功后延迟 200 ms，再进入订阅和请求流程。
2. attributes response 订阅完成后延迟 200 ms，再发布固件元信息请求。
3. 固件元信息请求最多重试 3 次。
4. 每次重试使用新的 requestId。
5. 每次等待平台响应的超时时间调整为 8000 ms。

Effect:
递增 requestId 后，连续多次测试时可以通过 topic 末尾编号确认响应属于哪一次请求，降低日志分析歧义。

Validation:
在加入唯一 MQTT clientId、连接/订阅后的短延迟以及请求重试机制后，设备完成了一次完整验证。日志显示：

```text
INFO: ESP8266 MQTT client id [stm32_ota_0024003A4156500820373539_7803]
INFO: ESP8266 MQTT connect to [47.97.214.156:1883] ok
INFO: ESP8266 MQTT publish [29] bytes to topic [v1/devices/me/telemetry] ok
INFO: ESP8266 MQTT subscribe [v1/devices/me/attributes/response/+] ok
INFO: ESP8266 MQTT subscribe [v1/devices/me/attributes] ok
MQTT subrecv payload done topic [v1/devices/me/attributes/response/1], 180 bytes
MQTT event queued topic [v1/devices/me/attributes/response/1], 180 bytes, count 1
INFO: Waiting ThingsBoard firmware info response, request id 1, attempt 1/3
MQTT event popped topic [v1/devices/me/attributes/response/1], 180 bytes, count 0
INFO: ThingsBoard firmware info found
ThingsBoard firmware info received
```

该验证说明当前链路已经覆盖：WiFi 联网、MQTT 建链、telemetry 上报、attributes response 订阅、固件元信息请求、`+MQTTSUBRECV` payload 长度解析、MQTT 事件入队/出队和固件信息字段识别。第一次请求 `attempt 1/3` 即成功，重试机制作为后续偶发超时的兜底处理。

Remaining risk:
如果长时间没有 `+MQTTSUBRECV`，需要从 ThingsBoard 设备 token、shared attributes 是否已分配、MQTT 连接状态、服务器访问稳定性和平台日志几个方向继续排查。

### Technical Route Adjustment: 接收固件 chunk 到 LittleFS

在固件元信息请求已经验证成功后，下一步开始验证 ThingsBoard 固件块下载。当前阶段已经从“只下载第 0 个 chunk”推进到“根据 `fw_size` 循环下载完整固件并写入 LittleFS 文件”。

实现边界如下：

```text
firmware size: 11772 bytes
chunk size   : 512 bytes
chunk count  : 23
last chunk   : 508 bytes
LFS path     : a.elf
```

设备端新增流程：

```text
订阅 v2/fw/response/+/chunk/+
发布 v2/fw/request/<requestId>/chunk/<chunkIndex>
payload 为期望 chunk 大小，例如 "512"
等待 v2/fw/response/<requestId>/chunk/<chunkIndex>
按 +MQTTSUBRECV 的 data_length 接收二进制 payload
按顺序写入 LittleFS 文件 a.elf
```

该阶段的重点是验证三件事：

1. ThingsBoard 能按 OTA MQTT topic 返回固件块。
2. ESP-AT 驱动能够接收二进制 `+MQTTSUBRECV` payload，而不把内容当字符串打印。
3. STM32 能将收到的 chunk 写入 LittleFS 文件系统。

代码中新增 `esp8266_thingsboard_request_firmware_chunk()`，用于请求单个固件 chunk 到调用方 buffer。`main.c` 中新增完整下载流程：先通过 `esp8266_thingsboard_get_firmware_info()` 获取 `fw_size`，再循环请求所有 chunk，并将数据按顺序写入 `a.elf`。

调试中观察到设备在发送 chunk 请求 payload `"512"` 后停住：

```text
Start AT command send [3] bytes raw data
```

该问题与前面的 JSON attributes 响应不同。固件 chunk 是二进制 payload，可能包含 `0x00`、控制字符或终端流控字符。如果 MQTT payload 被追加进 `g_wifi_rxbuf`，后续 `wifi_dbg("<<<< AT command reply:\r\n%s", g_wifi_rxbuf)` 会按字符串打印二进制数据，可能导致串口终端显示异常甚至看起来像“卡死”。

因此，解析器调整为：当状态机已经进入 `ESP_PARSE_MQTT_PAYLOAD` 时，payload 字节只写入 `mqtt_sub_event_t.payload`，不再追加到 `g_wifi_rxbuf` 调试回显缓冲。这样 AT 命令响应仍可打印，但固件二进制内容不会被 `%s` 路径输出。

继续实测后发现，设备仍会停在发送 `"512"` 后。进一步通过 Keil 编译、ST-LINK 烧录和串口日志复现，确认真正的卡点与栈空间有关。`mqtt_sub_event_t` 内部包含约 1 KB payload 缓冲，chunk 请求函数中又定义了一个局部 `mqtt_sub_event_t evt`，叠加 MQTT publish 调用链后会明显增加栈压力，表现为发送 raw payload 后不再进入正常超时或响应打印。

处理方式是将较大的 `mqtt_sub_event_t` 临时对象从函数栈移动到静态存储区：

```text
esp8266_mqtt_wait_subrecv_event(): static mqtt_sub_event_t evt
esp8266_thingsboard_request_firmware_chunk(): static mqtt_sub_event_t evt
```

该修改降低了 chunk 下载路径的栈占用，符合当前 STM32L431 RAM 紧张的约束。

修正后使用 Keil 编译通过：

```text
"test\test.axf" - 0 Error(s), 0 Warning(s).
Program Size: Code=48912 RO-data=1936 RW-data=460 ZI-data=18604
```

随后通过 STM32CubeProgrammer/ST-LINK 烧录并复位验证，chunk 0 成功写入 LittleFS：

```text
Start send AT command: AT+MQTTPUBRAW=0,"v2/fw/request/2/chunk/0",3,1,0
Start AT command send [3] bytes raw data
MQTT subrecv payload done topic [v2/fw/response/2/chunk/0], 512 bytes
MQTT event queued topic [v2/fw/response/2/chunk/0], 512 bytes, count 1
AT command matched expect '+MQTTPUB:OK'
INFO: ESP8266 MQTT publish [3] bytes to topic [v2/fw/request/2/chunk/0] ok
INFO: Waiting ThingsBoard firmware chunk 0, request id 2, size 512
MQTT event popped topic [v2/fw/response/2/chunk/0], 512 bytes, count 0
INFO: ESP8266 MQTT received topic [v2/fw/response/2/chunk/0], 512 bytes
INFO: ThingsBoard firmware chunk 0 received, 512 bytes
ThingsBoard firmware chunk 0 saved to tb_chunk.bin, 512 bytes
ThingsBoard firmware chunk LFS test ok
```

当前完整下载已经验证通过。Keil 编译通过后，使用 STM32CubeProgrammer/ST-LINK 烧录并抓取串口日志，完整固件写入结果如下：

```text
ThingsBoard firmware download progress: 11264/11772 bytes
Start send AT command: AT+MQTTPUBRAW=0,"v2/fw/request/24/chunk/22",3,1,0
MQTT subrecv payload done topic [v2/fw/response/24/chunk/22], 508 bytes
INFO: ThingsBoard firmware chunk 22 received, 508 bytes
ThingsBoard firmware download progress: 11772/11772 bytes
ThingsBoard firmware saved to a.elf, 11772 bytes, chunks 23
ThingsBoard firmware LFS download ok
```

main 测试流程结束后进入 shell，并注入 LittleFS 命令上下文。串口发送 `ls /` 后验证文件系统中存在完整固件：

```text
ls /
--- / ---
[FILE] a.elf                    11 KB
[FILE] tb_chunk.bin             512 B
@dev::
```

后续仍需要补充 SHA256/checksum 校验，确认 `a.elf` 与 ThingsBoard 元信息中的 `fw_checksum` 完全一致，再进入刷写或验证流程。

### Design Rationale: MQTT clientId 使用设备 UID 与启动 tick

在连续 RESET 调试中，设备每次启动后都使用固定 MQTT clientId：

```text
stm32_ota
```

固定 clientId 便于识别设备，但在快速 RESET 场景下存在一个问题：旧的 MQTT TCP 会话在服务器侧可能尚未完全释放，新会话又使用同一个 clientId 连接。MQTT broker 会断开旧会话，但在切换窗口期，订阅关系和下行响应可能出现短暂竞态，表现为 publish 成功但 attributes response 偶尔没有下发到当前连接。

因此，代码将 clientId 改为：

```text
stm32_ota_<设备UID>_<本次启动tick>
```

其中设备 UID 来自 STM32 唯一 ID，启动 tick 来自 `HAL_GetTick()`。这样同一块板子的不同启动会话具有不同 clientId，可以降低快速 RESET 后旧会话与新会话抢占同一 clientId 的概率。

该修改不影响 ThingsBoard 设备身份认证。ThingsBoard MQTT 接入仍然使用 access token 作为 username，clientId 只用于 MQTT 会话标识。

### Design Rationale: MQTT Topic 结构归属

本项目使用的 topic：

```text
v1/devices/me/telemetry
v1/devices/me/attributes/request/1
v1/devices/me/attributes/response/1
v2/fw/request/<requestId>/chunk/<chunkIndex>
```

这些 topic 不是 MQTT 标准规定的，而是 ThingsBoard Device MQTT API 规定的主题结构。

MQTT 标准只定义消息传输机制，包括：

```text
CONNECT
PUBLISH
SUBSCRIBE
topic
payload
QoS
retain
keepalive
```

MQTT 不规定 topic 的业务含义。`v1/devices/me/attributes/request/1` 的含义是 ThingsBoard 定义的：设备向平台请求 shared attributes，其中末尾的 `1` 是请求 ID，用于匹配响应 topic `v1/devices/me/attributes/response/1`。

因此，本项目中的职责划分为：

```text
MQTT：负责消息发布、订阅和传输
ThingsBoard：定义 topic 名称、payload 格式和 OTA 流程
ESP-AT：提供 AT+MQTTPUBRAW、AT+MQTTSUB 等 MQTT 操作接口
STM32：组织业务流程并解析 ThingsBoard 返回的数据
```

### Core Code Analysis: ESP-AT MQTT 与 ThingsBoard OTA 通信逻辑

当前 WiFi OTA 阶段使用 ESP32-C3 的 ESP-AT MQTT 能力。STM32 不直接构造 MQTT 二进制协议帧，而是通过 USART2 发送 AT 指令，由 ESP-AT 固件完成 MQTT CONNECT、PUBLISH、SUBSCRIBE、QoS 确认和 TCP/IP 传输。

#### AT 指令职责与期望回复

当前代码中 ESP-AT 指令主要分为 WiFi 初始化、TCP 测试和 MQTT 接入三类：

| 指令 | 作用 | 期望回复或判断条件 |
| --- | --- | --- |
| `AT+RST` | 复位 ESP-AT 模块 | `ready` |
| `AT` | 检查 AT 通信是否正常 | `OK` |
| `AT+CWMODE=1` | 设置 Station 模式 | `OK` |
| `AT+CWDHCP=1,1` | Station 模式启用 DHCP | `OK` |
| `AT+CWAUTOCONN=0` | 关闭自动重连，减少 reset 后旧状态干扰 | `OK` |
| `AT+CWQAP` | 断开当前 AP | `OK` |
| `AT+CWLAP` | 扫描附近 AP | `OK`，并从回复中查找目标 SSID |
| `AT+CWJAP="<ssid>","<pwd>"` | 连接目标 WiFi | `OK` |
| `AT+CIPSTA?` / `AT+CIPSTA_CUR?` | 查询本机 IP、网关和子网掩码 | 查找 `255.` 判断已获取 netmask |
| `AT+PING="<host>"` | 测试网关、域名或服务器连通性 | `OK` |
| `AT+CIPMUX=0` | 设置 TCP 单连接模式 | `OK` |
| `AT+CIPSTART="TCP","<ip>",<port>` | 建立裸 TCP 连接 | `CONNECT`，并等待最终 `OK` |
| `AT+CIPSEND=<len>` | 发送裸 TCP 数据前申请发送窗口 | `>` |
| `AT+CIPCLOSE` | 关闭 TCP 连接 | `OK` |
| `AT+MQTTCLEAN=0` | 清理 ESP-AT MQTT 客户端状态 | `OK`，当前允许失败 |
| `AT+MQTTUSERCFG=0,1,"<clientId>","<token>","",0,0,""` | 配置 MQTT 客户端身份 | `OK` |
| `AT+MQTTCONN=0,"<host>",1883,0` | 连接 ThingsBoard MQTT Broker | `+MQTTCONNECTED` |
| `AT+MQTTPUBRAW=0,"<topic>",<len>,1,0` | 发布原始 MQTT payload | 先等待 `>`，发送 payload 后等待 `+MQTTPUB:OK` |
| `AT+MQTTSUB=0,"<topic>",1` | 订阅 MQTT topic | `OK` |

其中 `MQTTUSERCFG` 是 MQTT 连接前的身份配置。当前代码中的 clientId 使用：

```text
stm32_ota_<STM32 UID>_<本次启动 tick>
```

该 clientId 主要用于 MQTT 会话标识，避免快速 reset 后新旧连接使用同一个 clientId 造成 broker 侧会话冲突。ThingsBoard 识别设备身份主要依赖 access token，即 `MQTTUSERCFG` 中的 username 字段，而不是 STM32 UID、ESP MAC 或 clientId。

`MQTTCONN` 只负责建立 MQTT 连接。固件元信息查询、telemetry 上报和固件 chunk 下载都不是由 `MQTTCONN` 直接完成，而是在连接建立后通过 `MQTTSUB` 与 `MQTTPUBRAW` 完成。

#### MQTT 发布参数含义

以 telemetry 上报为例：

```text
AT+MQTTPUBRAW=0,"v1/devices/me/telemetry",29,1,0
```

参数含义如下：

```text
0                              link_id，ESP-AT 内部 MQTT 连接编号
"v1/devices/me/telemetry"      topic，消息主题
29                             payload 字节长度
1                              QoS，当前表示至少送达一次
0                              retain，不要求 broker 保留该消息
```

当前 telemetry payload 为：

```json
{"fw_test":1,"wifi_rssi":-34}
```

该 JSON 字符串本体长度为 29 字节。C 语言数组中还包含字符串结尾的 `\0`，因此代码使用：

```c
sizeof(thingsboard_telemetry) - 1U
```

作为 `AT+MQTTPUBRAW` 的 payload 长度。这个长度必须和随后 STM32 实际发送给 ESP-AT 的 payload 字节数一致，否则 ESP-AT 可能等待更多字节、发送失败或造成后续串口数据错位。

`link_id` 在裸 TCP 多连接和 MQTT 多客户端场景中用于区分连接。当前项目只使用一个 MQTT 客户端，因此固定使用 `0`。虽然 ESP-AT 指令格式保留了多个 MQTT link_id 的可能性，但当前 OTA 场景不需要多 MQTT 连接，一个 MQTT 连接可以同时发布 telemetry、请求 attributes、订阅 attributes response，并逐块拉取固件。保持单连接更节省 RAM，也更利于稳定调试。

#### MQTT 协议帧与 ESP-AT 返回格式

MQTT 底层是二进制协议，典型报文由固定头、可变头和 payload 组成：

```text
Fixed Header
Variable Header
Payload
```

以 MQTT `PUBLISH` 报文为例，固定头中包含报文类型、QoS、retain 和剩余长度；可变头中包含 topic 和 QoS 相关的 packet identifier；payload 中才是 JSON 或固件二进制数据。

当前 STM32 没有直接解析这些 MQTT 二进制帧。对于上行消息，STM32 发送 `AT+MQTTPUBRAW`，ESP-AT 固件负责封装 MQTT `PUBLISH` 帧。对于下行消息，ESP-AT 固件收到 MQTT `PUBLISH` 后，会通过串口转换成：

```text
+MQTTSUBRECV:<link_id>,"<topic>",<payload_len>,<payload>
```

因此当前驱动需要解析的是 ESP-AT 的 `+MQTTSUBRECV` 格式。由于固件 chunk 是二进制 payload，解析器必须依据 `<payload_len>` 做长度接收，不能用 `strlen()`、`strstr()` 或 `%s` 打印固件内容。

#### ThingsBoard topic 规则

ThingsBoard 的 topic 是应用层协议规定，不属于 MQTT 标准本身。MQTT 只规定 publish、subscribe、topic、payload、QoS、retain 等通用机制；ThingsBoard 规定哪些 topic 代表 telemetry、attributes、RPC 或 firmware OTA。

当前项目使用的 ThingsBoard topic 如下：

| Topic | 方向 | 含义 |
| --- | --- | --- |
| `v1/devices/me/telemetry` | 设备到平台 | 上报遥测数据，例如 RSSI、测试标志、运行状态 |
| `v1/devices/me/attributes/request/<requestId>` | 设备到平台 | 主动请求属性 |
| `v1/devices/me/attributes/response/<requestId>` | 平台到设备 | 回复设备属性请求 |
| `v1/devices/me/attributes/response/+` | 设备订阅 | 订阅所有属性请求响应 |
| `v1/devices/me/attributes` | 平台到设备 | shared attributes 变化时的主动推送 |
| `v2/fw/request/<requestId>/chunk/<chunkIndex>` | 设备到平台 | 请求某个固件分块 |
| `v2/fw/response/<requestId>/chunk/<chunkIndex>` | 平台到设备 | 返回对应固件分块 |
| `v2/fw/response/+/chunk/+` | 设备订阅 | 订阅所有固件分块响应 |

`v1/devices/me/...` 中的 `me` 表示当前 access token 对应的设备。设备不需要在 topic 中写 ThingsBoard 设备 ID，因为 MQTT 登录时的 username/access token 已经完成设备身份绑定。

`v1/devices/me/attributes/request/<requestId>` 只是“属性请求入口”，请求哪类属性由 payload 决定。当前 OTA 元信息请求 payload 为：

```json
{"sharedKeys":"fw_title,fw_version,fw_size,fw_checksum,fw_checksum_algorithm"}
```

因此请求的是 ThingsBoard 保存的 shared attributes。平台回复中会包含：

```json
{
  "shared": {
    "fw_title": "v1.0",
    "fw_version": "v1.0",
    "fw_size": 11772,
    "fw_checksum": "...",
    "fw_checksum_algorithm": "SHA256"
  }
}
```

固件块请求中的 `<requestId>` 由设备端自己生成。当前代码使用 `s_tb_request_id` 从 1 开始递增，每次请求 attributes 或 firmware chunk 时取一个新编号。ThingsBoard 回复时会把该编号带回 response topic，用于设备侧区分本次响应属于哪一次请求。当前实现已经能完成完整下载，后续为了更稳，应进一步严格校验 response topic 中的 requestId 与 chunkIndex 是否和当前请求一致。

#### MQTTSUB 的作用

`MQTTSUB` 不会主动拉取数据，它的作用是向 MQTT Broker 登记接收规则。可以将它理解为当前 MQTT 连接上的“接收白名单”或“关注列表”：

```text
凡是 broker 收到匹配这些 topic 的消息，就通过当前 MQTT 连接转发给设备。
```

因此，订阅成功的 `OK` 只表示 broker 接受了订阅规则，不表示平台已经发送了业务数据。真正的数据到达时，ESP-AT 会通过串口输出：

```text
+MQTTSUBRECV:0,"<topic>",<payload_len>,<payload>
```

例如设备订阅：

```text
v1/devices/me/attributes
```

当 ThingsBoard 平台修改该设备的 shared attributes 后，平台会向该 topic 发布一条消息。如果设备当时在线且已经订阅，ESP-AT 会输出类似：

```text
+MQTTSUBRECV:0,"v1/devices/me/attributes",36,{"fw_version":"v1.1","fw_size":20480}
```

如果设备没有订阅对应 topic，即使平台发布了该消息，broker 也不会转发给该设备连接。

#### 离线设备与 OTA 主动检查策略

低功耗 OTA 场景中，WiFi 模块不可能长期在线，MQTT 连接也不应长期保持。因此 OTA 不能依赖“平台属性变化时实时推送”作为唯一触发路径。设备离线期间如果 ThingsBoard 修改了固件 shared attributes，实时推送会错过，但属性值本身仍保存在 ThingsBoard 数据库中。

因此当前项目应采用主动检查策略：

```text
周期性唤醒或开机
    -> 初始化 ESP-AT
    -> 连接 WiFi
    -> 连接 MQTT
    -> 重新订阅 response/chunk topic
    -> 主动请求 fw_* shared attributes
    -> 比较平台 fw_version 与本地固件版本
    -> 不需要升级则断开并休眠
    -> 需要升级则主动逐块请求固件
    -> 写入 LittleFS
    -> 校验 checksum
    -> 执行 OTA 切换
```

订阅规则通常属于当前 MQTT 连接或 MQTT 会话，不应假设 ThingsBoard 会按设备身份永久保存订阅规则。结合当前代码每次启动都会生成新的 clientId，更应认为每次 MQTT 连接成功后都需要重新执行必要的 `MQTTSUB`。这也是低功耗设备 OTA 状态机需要自己完成的内容。

项目结论是：ThingsBoard 负责保存设备属性和固件包；MQTT Broker 负责在设备在线且订阅匹配时转发消息；设备端负责周期性上线、重新订阅、主动查询固件元信息、判断版本差异并按需拉取固件块。

#### `esp_at_process()` 的解析核心

在当前驱动里，USART2 中断只负责把收到的字节放进环形缓冲区，真正的协议解析在 `esp_at_process()` 中完成。其核心代码如下：

```c
static void esp_at_process(void)
{
    uint8_t ch;

    while( esp_ring_pop(&ch) )
    {
        if( s_parse_state == ESP_PARSE_MQTT_PAYLOAD )
        {
            mqtt_payload_append_byte(ch);
            continue;
        }

        esp_debug_append(ch);

        if( ch == '>' )
        {
            at_cmd_handle_text(">");
            s_line_len = 0U;
            continue;
        }

        if( s_line_len < (ESP_LINE_BUF_SIZE - 1U) )
        {
            s_line_buf[s_line_len++] = (char)ch;
        }
        else
        {
            s_line_len = 0U;
        }

        if( try_parse_mqtt_subrecv_header() )
        {
            continue;
        }

        if( ch == '\n' )
        {
            esp_parse_line();
        }
    }
}
```

这段逻辑解决的是“同一条串口流里同时存在三类边界”的问题：

1. 普通 AT 文本回复，按 `\n` 作为一行结束。
2. `>` 发送提示符，不等换行，出现后要立刻通知上层发送 payload。
3. `+MQTTSUBRECV` 的 MQTT 下行 payload，按长度接收，不按字符串或换行处理。

你在讨论里提出的疑惑可以归纳为：`>` 既然也能交给 `at_cmd_handle_text()`，为什么不直接等到 `\n` 再统一匹配。这里的答案是，`>` 的语义不是“这一行回复结束”，而是“发送窗口已经打开”。如果仍然按照 `\n` 处理，STM32 会晚一步拿到发送许可，`AT+MQTTPUBRAW` / `AT+CIPSEND` 的 payload 发送就会变得不及时。

因此当前实现把两类事件分开处理：

```text
普通文本回复 -> 攒成行，等 \n 再处理
> 提示符     -> 单字符立即处理
MQTT payload  -> 按长度逐字节接收
```

这也解释了 `esp_debug_append()` 的作用：它只是把文本流复制到调试缓冲区，方便最后打印 `<<<< AT command reply:`，并不参与真正的协议判定；真正的判定是由 `at_cmd_handle_text()` 更新 `s_cmd_ctx.state` 完成的。

你可以把当前解析器理解为一个状态机：

```text
ESP_PARSE_LINE
  -> 普通 AT 文本按行解析
  -> 遇到 '>' 立即触发发送事件
  -> 识别到 +MQTTSUBRECV 后切换到 payload 状态

ESP_PARSE_MQTT_PAYLOAD
  -> 按 payload_len 收满后切回 ESP_PARSE_LINE
```

这个设计的价值在于，它允许 AT 回复、发送提示符和 MQTT 二进制 payload 共用同一条 UART 输入流，而不会互相干扰。

## TCP 连接验证结果

使用手机热点 `first` 建立局域网后，ESP32-C3 与电脑 TCP 服务连接成功：

```text
ESP IP: 192.168.10.12
Gateway: 192.168.10.38
Computer IP: 192.168.10.11
TCP Port: 8080
```

验证日志：

```text
AT+PING="192.168.10.11"
+PING:330
OK

AT+CIPSTART="TCP","192.168.10.11",8080
CONNECT
OK

AT+CIPSEND=18
>
Recv 18 bytes
SEND OK

TCP sent 18 bytes to 192.168.10.11:8080
```

结论：

1. ESP32-C3 到电脑的局域网链路已打通。
2. TCP 建链与发送流程可用。
3. 当前 WiFi 底层传输可以作为后续 ThingsBoard/MQTT/HTTP OTA 的基础。
4. 后续重点从“能否联网”转为“如何按 ThingsBoard 协议发送/接收固件数据”。

双向通信测试时，`main.c` 会在 TCP 连接成功后保持连接，并在主循环中轮询 `esp8266_sock_recv()`。电脑 TCP 服务端发送文本后，ESP32-C3 侧会通过串口打印：

```text
TCP recv <len> bytes: <payload>
```

## 局域网通信中的 ARP、交换机与路由器职责

本项目在 WiFi TCP 联调时涉及 ESP32-C3、电脑、手机热点或路由器三类节点。为了理解数据是否经过手机/路由器，以及为什么设备仍然需要 ARP，需要区分 ARP 表、交换机 MAC 表和路由表。

### Design Rationale: ARP 与二层交换的职责划分

该问题的核心在于：应用层只指定目标 IP，例如 `192.168.10.11`，但以太网/WiFi 链路层真正发送的是二层帧。二层帧在离开发送设备前必须填入目标 MAC 地址：

```text
目标 MAC
源 MAC
类型：IPv4
IP 包：目标 IP = 192.168.10.11
TCP 数据
```

因此，ESP32-C3 在向同一局域网内的电脑发送 TCP 数据前，需要先知道 `192.168.10.11` 对应的 MAC 地址。这个 `IP -> MAC` 的解析由 ARP 完成，解析结果缓存在主机或 ESP32-C3 内部 TCP/IP 协议栈中。

二层交换机维护的不是 `IP -> MAC`，而是：

```text
MAC 地址 -> 交换机端口
```

交换机收到已经封装好的二层帧后，只根据目标 MAC 查找对应端口并转发。它并不负责替主机把目标 IP 转换成 MAC。也就是说：

```text
ARP 表：IP -> MAC，由主机/协议栈维护
MAC 表：MAC -> 端口，由二层交换机/AP 维护
```

在本项目当前的 AT 模块方案中，STM32 应用层不需要自己维护 ARP 表。STM32 只通过 AT 指令指定目标 IP 和端口：

```c
esp8266_sock_connect("192.168.10.11", 8080);
```

实际的 ARP 查询、TCP/IP 封装、WiFi 帧发送均由 ESP32-C3 AT 固件内部完成。

### 二层交换机与路由器的区别

二层交换机工作在数据链路层，核心转发依据是：

```text
目标 MAC -> 出口端口
```

路由器或三层交换设备工作在网络层，核心转发依据是：

```text
目标 IP -> 路由表 -> 下一跳/出口接口
```

当 ESP32-C3 与电脑处在同一网段，例如：

```text
ESP32-C3: 192.168.10.12
电脑:     192.168.10.11
掩码:     255.255.255.0
```

ESP32-C3 会对电脑 IP 做 ARP，得到电脑 MAC，然后把 TCP 数据作为局域网二层帧发送。手机热点或路由器/AP 负责在无线局域网内转发这些帧，数据不会进入公网。

当 ESP32-C3 访问外网，例如 `www.baidu.com` 或 `8.8.8.8`，目标 IP 不在本地网段内。此时 ESP32-C3 不会 ARP 查询外网 IP 的 MAC，而是查询默认网关的 MAC：

```text
目标 IP: 外网服务器
二层目标 MAC: 默认网关 MAC
```

网关收到后拆掉原来的二层头，查看 IP 目标地址，查路由表，必要时执行 NAT，再重新封装新的二层帧发往下一跳。

### 项目结论

在当前 OTA WiFi 接收方案中：

1. STM32 不需要实现 ARP，也不需要维护 `IP -> MAC` 表。
2. ESP32-C3 AT 固件内部会处理 ARP、TCP 重传、IP 封装和 WiFi 链路细节。
3. 手机热点或路由器在局域网内承担 AP/转发节点角色，局域网通信会经过它，但不会经过公网。
4. 后续如果改为 STM32 直接运行 lwIP 并驱动网卡，则需要重新考虑 ARP 缓存、超时刷新和链路层发送逻辑。

## 当前重构后的 ThingsBoard OTA 调用链

本次重构把 ThingsBoard OTA 流程收拢成了几个独立阶段，主入口只保留“调度”，驱动层只负责 AT/MQTT 收发和事件解析：

1. `main.c` 进入 `thingsboard_ota_check_and_download()`
2. `thingsboard_wifi_prepare()`
   - `esp8266_module_init()`
   - `esp8266_scan_ap()`
   - `esp8266_join_network()`
   - `esp8266_get_ipaddr()`
   - `esp8266_ping_test()`
3. `thingsboard_mqtt_prepare()`
   - `esp8266_mqtt_connect()`
   - `esp8266_thingsboard_publish_telemetry()`
   - `esp8266_thingsboard_request_firmware_info()`
4. `thingsboard_firmware_should_download()`
   - 对比远端 `fw_version` 与本地 `sys_get_version()`
   - 检查 `fw_checksum_algorithm`
5. `thingsboard_firmware_download_to_lfs()`
   - 先订阅 `v2/fw/response/+/chunk/+`
   - 再逐块请求 `v2/fw/request/<requestId>/chunk/<chunkIndex>`
   - 收到 `+MQTTSUBRECV` 后写入 LittleFS 文件 `a.elf`
6. 结束后统一断开 MQTT / WiFi，并进入 shell 让用户手动执行 `ota start a.elf`

### 这次拆分的关键点

1. 固件元信息请求和 chunk 下载分开，避免一个函数又“订阅”又“请求”又“写文件”。
2. chunk topic 改成按 `requestId + chunkIndex` 精确匹配，减少异步消息串台。
3. chunk 订阅提前到下载前一次完成，和 ThingsBoard 的 OTA 语义更一致。
4. WiFi 失败后补了断开清理，避免后续测试残留连接状态。

## ThingsBoard 固件传输后的 checksum 校验

ThingsBoard 固件元信息中包含：

```json
{
  "fw_size": 11772,
  "fw_checksum_algorithm": "SHA256",
  "fw_checksum": "448655b298b5387abeae7175890b8b7655960489262d50b6603b616e5b747603"
}
```

当前代码把校验拆成两层：

1. **传输完整性**：下载每个 chunk 时检查收到的 payload 长度是否等于请求长度，全部写入后检查总大小是否等于 `fw_size`。
2. **整文件 checksum**：对 LittleFS 中保存的 `a.elf` 计算 SHA256，并与 ThingsBoard 下发的 `fw_checksum` 做字节级比较。

注意：checksum 校验只能证明“下载到本地的文件内容与平台元信息声明的文件一致”，不能单独证明固件来源可信。如果后续需要防止固件被伪造，仍然需要继续执行固件包内部的签名验证，例如现有 `verify_firmware()` 中的 ECDSA 验签逻辑。

当前接入点：

```text
thingsboard_firmware_download_to_lfs()
  ↓
thingsboard_firmware_verify_download()
  ↓
verify_lfs_file_sha256()
  ↓
校验通过才允许保留 a.elf 并提示 ota start a.elf
```

如果 SHA256 或文件大小不匹配，代码会删除 `a.elf`，并阻止后续手动 `ota start` 使用错误固件。

## ThingsBoard 下载流程接入 OTA 状态机

本次接入没有新增 OTA 状态，而是复用原状态机的传输抽象：

```c
typedef int (*receive_callback_t)(void *user_ctx, void *file_info_out);
```

原来 `UPGRADING` 状态通过 `Proto_Start_Receive()` 从 Xmodem/Ymodem 接收固件；现在 ThingsBoard OTA 模式下把 `receive_cb` 替换为 `thingsboard_ota_receive_cb()`：

```text
main()
  ↓
ota_context_init_thingsboard()
  ↓
transfer_cfg.receive_cb = thingsboard_ota_receive_cb
  ↓
ota_run()
  ↓
handle_upgrading()
  ↓
ctx->transfer_cfg->receive_cb(...)
```

这样状态机仍然只关心“有没有收到一个完整固件包”，不直接关心固件来自串口、Xmodem、WiFi 还是 ThingsBoard。

当前 ThingsBoard 模式的 `UPGRADING` 行为：

1. `thingsboard_ota_receive_cb()` 连接 WiFi 和 ThingsBoard MQTT。
2. 请求固件元信息。
3. 如果远端版本不高于本地版本，返回 `0`，状态机认为“没有新固件”，回到 `BOOT`。
4. 如果需要升级，则下载固件 chunk 到 LittleFS 的 `a.elf`。
5. 下载完成后做 ThingsBoard `fw_checksum` 整文件 SHA256 校验。
6. 返回固件大小，状态机继续执行原来的 `normalize_received_package()` 和 `verify_firmware()`。
7. 校验和签名通过后进入 `VERIFYING`，由原来的 `bootloader_load_target()` 完成 ELF 装载、写入目标分区、提交 active slot。

### 关键边界修正

1. 删除了 `UPGRADING` 开头的提前擦除目标分区。现在真正写 Flash 仍由 `bootloader_load_target()` 完成，避免只是检查云端更新时就擦掉备用分区。
2. WiFi 状态机模式下不再编译进 shell 入口和 Xmodem 初始化引用，避免无关模块把 Flash/RAM 撑爆。
3. MQTT 异步事件队列从 4 个槽降到 2 个槽。当前固件元信息和 chunk 都是串行请求-响应模型，2 个槽足够兜住一个当前事件和一个短时间交错事件，同时节省 RAM。
