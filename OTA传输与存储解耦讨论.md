# OTA传输与存储解耦讨论

## 1. 背景

当前工程已经打通以下 OTA 主链路：

- `ESP8266` 通过 `AT` 指令接入 WiFi 和 ThingsBoard MQTT。
- 设备从平台请求固件属性和固件分块。
- 固件分块写入 `LittleFS` 文件系统。
- 下载完成后执行 `SHA256` 校验、`ECDSA` 验签、`ELF` 装载和状态机提交。

在这一链路中，`ThingsBoard` 下载逻辑已经能够稳定工作，但代码结构上存在一个明显问题：传输过程与 `LittleFS` 写入过程耦合较深，导致后续若要替换存储目标，或统一串口 OTA 与 WiFi OTA 的抽象层，会比较困难。

本讨论文档用于记录当前耦合点、可选解法、完整抽象方案，以及本阶段暂不实施该抽象的原因。

## 2. 当前代码中的耦合位置

当前入口绑定函数位于 `Core/interface/ESP8266/thingsboard_ota.c`，函数名为：

- `thingsboard_ota_bind_transport()`

该函数本身只负责将 OTA 状态机与 `ThingsBoard` 接收回调绑定起来，其动作较轻：

- 清空 `transfer_cfg_t`
- 清空 `ota_ctx_t`
- 将 `receive_cb` 绑定为 `thingsboard_ota_receive_cb()`
- 将 `ctx->transfer_cfg` 和 `ctx->resource_ctx` 分别指向传输配置与文件系统上下文

真正的耦合点不在绑定函数本身，而在 `thingsboard_ota_receive_cb()` 内部。

### 2.1 当前回调实际承担的职责

`thingsboard_ota_receive_cb()` 当前并不是一个狭义的“接收一段数据并交给上层”的回调，而是一整个 `ThingsBoard OTA` 事务调度器。其内部依次完成：

1. 从 `user_ctx` 中取出 `ota_ctx`
2. 再从 `ota_ctx->resource_ctx` 中取出 `lfs_ctx`
3. 检查网络配置有效性
4. 初始化并连接 `ESP8266`
5. 连接 `MQTT`
6. 请求固件属性
7. 判断是否需要升级
8. 生成下载路径
9. 逐块下载固件
10. 直接写入 `LittleFS`
11. 下载完成后执行整包 `SHA256` 校验
12. 回填版本号和文件信息给 OTA 状态机
13. 断开 `MQTT` 与 `WiFi`

因此，该回调同时依赖以下几类对象：

- OTA 状态机上下文
- `LittleFS` 文件系统上下文
- 网络配置
- `ESP8266` / `MQTT` / `ThingsBoard` 协议细节
- 固件校验逻辑

### 2.2 当前耦合的具体表现

当前耦合主要表现为以下几种：

- 传输层直接知道存储介质是 `LittleFS`
- 传输层直接调用 `lfs_file_open`、`lfs_file_write`、`lfs_file_sync`、`lfs_file_close`
- 传输层知道固件文件命名规则
- 传输层在下载失败时直接执行文件删除
- OTA 状态机通过 `write_cb == NULL` 这种隐式方式判断当前不是串口流式接收，而是 WiFi 精确长度接收

这意味着后续如果希望将固件改写到：

- RAM 缓冲区
- 内部 Flash
- 其他文件系统
- 双写缓存目标

则必须直接修改 `ThingsBoard` 下载逻辑，而不是仅替换一个底层写入实现。

## 3. 讨论出的两种解法

### 3.1 方案一：只解开 `ThingsBoard` 与 `LittleFS`

该方案属于最小改动方案，目标是：

- 保留 OTA 状态机现有接口
- 保留 `transfer_cfg_t` 的现状
- 保留 `receive_cb` 的用法
- 只把 `LittleFS` 文件写入部分从 `ThingsBoard` 下载循环中拆出来

可引入一个轻量级写入器接口，例如：

```c
typedef struct
{
    int (*open)(void *ctx, const char *path);
    int (*write)(void *ctx, const uint8_t *buf, uint32_t len);
    int (*sync)(void *ctx);
    int (*close)(void *ctx);
    int (*remove)(void *ctx, const char *path);
    void *ctx;
} firmware_sink_t;
```

然后：

- `ThingsBoard` 下载循环只负责请求 chunk 和检查长度
- 每收到一块数据后调用 `sink->write()`
- `LittleFS` 提供一套 `sink` 实现

这种做法的优点是：

- 改动小
- 风险低
- 能先把“下载逻辑”和“写入逻辑”分离

其缺点是：

- `Xmodem/Ymodem` 与 `ThingsBoard` 的传输抽象仍不统一
- `transfer_cfg_t` 中 `write_cb` 和 `receive_cb` 继续维持两套模型

### 3.2 方案二：统一 OTA 传输抽象

该方案属于完整抽象方案，目标是把所有 OTA 输入源统一到同一套接口下。

核心思想是明确分出两个角色：

- `transport`：负责“从哪里拿固件”
- `sink`：负责“拿到以后写到哪里”

建议的抽象方向为：

```c
typedef struct
{
    char     path[64];
    uint32_t size;
    char     version[32];
    char     checksum[65];
    char     checksum_algorithm[16];
    uint8_t  has_remote_metadata;
} ota_artifact_info_t;
```

```c
typedef struct
{
    int (*open)(void *ctx, const ota_artifact_info_t *info);
    int (*write)(void *ctx, const uint8_t *buf, uint32_t len);
    int (*sync)(void *ctx);
    int (*close)(void *ctx);
    int (*remove)(void *ctx, const char *path);
    void *ctx;
} ota_sink_t;
```

```c
typedef enum
{
    OTA_TRANSPORT_STREAM_PADDED,
    OTA_TRANSPORT_STREAM_EXACT,
} ota_transport_mode_t;

typedef struct
{
    int (*fetch)(void *ctx, ota_sink_t *sink, ota_artifact_info_t *info);
    void                 *ctx;
    ota_transport_mode_t  mode;
} ota_transport_t;
```

在这个模型下：

- `Xmodem/Ymodem` 不再直接写 Flash 或 `LittleFS`
- `ThingsBoard` 也不再直接写 `LittleFS`
- 所有传输方式最终都统一调用 `sink->write()`
- OTA 状态机只调用 `transport->fetch()`

### 3.3 统一抽象方案下的关键变化

若采用方案二，则需要同步修改以下逻辑：

1. `transfer_cfg_t` 的职责需要被 `ota_transport_t` 替代或显著重构
2. `ota_ctx_t` 中需要显式持有 `transport` 与 `sink`
3. OTA 状态机不能再依赖 `write_cb == NULL` 判断传输类型
4. 必须显式增加 `transport mode`
5. `Proto_Start_Receive()` 需要增加桥接层，把流式数据转交给统一 `sink`
6. `ThingsBoard` 下载器需要改为只负责获取 chunk，再写入统一 `sink`

这一步的意义在于：

- 串口 OTA 与 WiFi OTA 的抽象真正统一
- 上层状态机不再知道底层使用的是 `Xmodem`、`MQTT` 还是其他来源
- 存储目标可以从 `LittleFS` 切换为其他实现，而无需修改传输层

## 4. 当前结论：完整抽象方案先暂缓

经过讨论，当前决定为：

- 暂不立刻实施方案二
- 先把该思路记录为后续重构方案

原因如下：

### 4.1 当前改动面过大

方案二会影响：

- `transfer_cfg_t`
- `ota_ctx_t`
- OTA 状态机 `UPGRADING` 逻辑
- `Proto_Start_Receive()`
- `ThingsBoard` 下载路径
- `main` 中的绑定流程

这属于架构级重构，不是局部修补。

### 4.2 当前主线任务仍是 OTA 业务稳定

目前项目主线重点仍然是：

- WiFi / MQTT / ThingsBoard OTA 主链路稳定
- 固件写入 `LittleFS` 后的校验与装载稳定
- OTA 状态机与版本管理逻辑继续收敛

在此阶段贸然做大抽象，容易把“业务验证问题”和“架构重构问题”混在一起。

### 4.3 当前抽象目标已经明确，不必急于立即落地

虽然暂时不改，但讨论已经明确了后续重构方向：

- 上层统一抽象成 `transport + sink`
- 传输层不直接操作 `LittleFS`
- 状态机不通过 `write_cb == NULL` 做隐式类型判断
- `mode` 应作为显式属性存在

因此，本次讨论的价值在于“先把正确方向定清楚”，而不是立即把所有代码一次性重构。

## 5. 后续可执行的收敛顺序

若未来重新启动该重构，建议按以下顺序推进：

1. 先定义 `ota_sink_t`
2. 先让 `ThingsBoard` 下载逻辑通过 `sink` 写入
3. 再为 `Xmodem/Ymodem` 添加桥接层，使其也写统一 `sink`
4. 最后再重构 OTA 状态机与 `transfer_cfg_t`

这样可以避免一次性重构过深，降低联调风险。

## 6. 本文档结论

本次讨论形成的最终结论是：

- 当前 `ThingsBoard OTA` 的主要结构问题，不在于绑定函数命名，而在于 `thingsboard_ota_receive_cb()` 承担了过多跨层职责。
- 若只做局部优化，可以先把 `LittleFS` 写入从下载循环中抽离。
- 若要从架构上彻底统一 OTA 传输模型，则应采用 `transport + sink` 的双角色抽象。
- 该统一抽象方案已经具备清晰方向，但当前项目阶段应先记录方案，暂不立即实施。

因此，本方案在当前阶段属于“已讨论、已澄清边界、暂缓实现”的架构备忘录。
