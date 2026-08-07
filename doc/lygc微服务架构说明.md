# lygc 微服务架构说明

## 1. 概述

lygc 是一个基于 C++ 的微服务框架，构建在 `zbf` 网络库之上。框架分层如下：

```
业务层 (Business Logic)
    |
lygc 层 (协议 + 服务抽象)
    |
zbf 层 (TCP Socket + 线程模型)
```

框架支持三种服务角色：

| 角色 | 基类 | 用途 |
|------|------|------|
| **普通微服务** | `NetServer` | 处理业务请求，通过 `UserHandler` 回调与客户连接 |
| **网关服务** | `GateServer` | 在普通服务基础上增加 RSA 握手 + AES 会话加密 |
| **DB 代理服务** | `DBProxyBase` | 将 DB 客户端请求转发到连接池，异步回包 |

每种服务角色对应一个独立的进程实例。

---

## 2. 类继承关系

### 2.1 服务端类图

```
zbf::tcpsock_server                    # 框架层：IO线程/监控线程/工作线程池/泳道路由
├── lygc::NetServer                    # 业务服务基类
│   │   - UserHandler 注册与分发
│   │   - ReqContext 异步请求匹配 (16分片)
│   │   - NetClient HA 出站客户端管理
│   │   - async_client_manager 出站连接 epoll 管理
│   │
│   └── lygc::GateServer              # 网关服务
│       - RSA 加载 + 握手处理
│       - 会话加密类型配置
│
└── lygc::DBProxyBase                  # DB代理基类（独立于 NetServer）
    │   - DBCltPoolCtx 分片挂起请求 (64槽 x 16分片)
    │   - onResponseBase 异步回包匹配
    │   - 纯虚钩子: initDriver / initPools / cleanupDriver / cleanupPools
    │
    ├── lygc::DBProxyMySQL   (+ mysql_client_pool_listener)
    ├── lygc::DBProxyMongo   (+ mongo_client_pool_listener)
    └── lygc::DBProxyRedis   (+ redis_client_pool_listener)
```

**设计要点**：`NetServer` 和 `DBProxyBase` 都直接继承 `tcpsock_server`，不共享 lygc 层基类。原因是二者的请求生命周期完全不同：
- `NetServer` 通过 `UserHandler` 回调模式处理业务逻辑
- `DBProxyBase` 通过 `pool_listener` 接口处理数据库回包

### 2.2 用户连接类图

```
zbf::tcpsock_user                          # 框架层：fd/收发缓存/写队列/心跳/连接状态
├── lygc::NetUser                          # 协议解包 -> onRequest 分发
│   │   - 重写的虚函数: onConnect / onRecvMsg / onDisconnect
│   │   - 虚函数: onRequest(header, body)
│   │   - 虚函数: createResponse(header, body)
│   │
│   └── lygc::GateUser                     # 握手 + AES 加解密
│       - onRequest 重写：握手/解密后委托 NetUser
│       - createResponse 重写：加密后委托 NetUser
│
└── lygc::DBProxyUserBase                  # 委托 onRecvMsg 到子类
    ├── lygc::DBProxyMySQLUser              # 反序列化 MySQL 请求 -> submit
    ├── lygc::DBProxyMongoUser              # 反序列化 MongoDB 请求 -> submit
    └── lygc::DBProxyRedisUser              # 反序列化 Redis 请求 -> submit
```

### 2.3 客户端类图

```
zbf::tcpsock_client                        # 框架层：阻塞式 send/recv
└── zbf::tcpsock_asynclt                   # 非阻塞：epoll + post 队列

zbf::tcpsock_ha_asynclt                    # HA 异步客户端（多连接轮询）
└── lygc::NetClient                        # per-serial Handler 回调映射
    - request(header, data, handler)       # 出站请求 + 回调注册
    - onResponse(msg)                      # 回包分发：per-serial > Responser

lygc::DBClientMySQL                        # 独立适配器（非继承）
lygc::DBClientMongo                        # 包装 NetClient，类型安全序列化
lygc::DBClientRedis
```

### 2.4 抽象接口

```
lygc::UserHandler                          # 业务处理接口
    onRequest(reqHeader, reqData, syncRespData) -> request_id_t
    # 返回 SYNC_RESPONSE(0): 同步回包
    # 返回 NO_RESPONSE: 不回包
    # 返回 >= ASYNC_RESPONSE(101): 异步，通过 NetServer::response() 回包

lygc::Responser                            # 默认异步回包接口
    onResponse(respHeader, respData)

wjp::*_client_pool_listener                # DB 连接池回包接口
    onResponse(req, resp)
```

---

## 3. 消息协议

### 3.1 消息头结构 (`lymsg_header`)

```
偏移  大小  字段      说明
0     2     origin    源服务标识 (uint16_t)
2     2     reserve   保留字段 (uint16_t)
4     4     type      消息类型标志位 (uint32_t)
8     8     serial    请求序列号，用于异步匹配 (uint64_t)
16    4     size      消息体长度 (uint32_t)
───────────────────────
总计 20 字节 (紧凑对齐, #pragma pack(1))
```

### 3.2 类型标志位

| 常量 | 值 | 说明 |
|------|-----|------|
| `LYMSG_TYPE_REQ` | `0x00000000` | 请求标志 |
| `LYMSG_TYPE_RESP` | `0x80000000` | 响应标志 (最高位) |
| `LYMSG_TYPE_ENC` | `0x40000000` | 加密标志 (次高位) |
| `LYMSG_TYPE_HANDSHAKE` | `0x00000011` | 握手消息 |
| `LYMSG_TYPE_DB_MYSQL` | `0x00000021` | MySQL 请求 |
| `LYMSG_TYPE_DB_MONGO` | `0x00000022` | MongoDB 请求 |
| `LYMSG_TYPE_DB_REDIS` | `0x00000023` | Redis 请求 |

通过位运算判断消息性质：
- `type & LYMSG_TYPE_RESP` → 是响应消息
- `type & LYMSG_TYPE_ENC` → 消息体已加密

### 3.3 协议解析器

`lymsg_protocol` 实现了 `tcp_message_protocol` 的全部虚函数：

| 方法 | 作用 |
|------|------|
| `headerSize()` | 返回 `sizeof(lymsg_header)` = 20 |
| `bodySize(msg)` | 从头中提取 `size` 字段 |
| `type(msg)` | 从头中提取 `type` 字段，用于泳道路由 |
| `genHeartbeat()` | 生成 `type=Heartbeat` 的心跳包 |
| `isHeartbeat(msg)` | 判断 `type == Heartbeat` |

### 3.4 编解码工具

`lymsg_helper` 提供静态方法：

```
packMsg(header, data) -> tcp_message*
    创建完整消息：拷贝 header + 设置 size + 拷贝 body

unpackMsg(msg, header, data) -> 0/-1
    从原始消息中提取 header 和 body 数据
```

---

## 4. 消息流转详解

### 4.1 入站流程：TCP 接收 → 业务处理

```
TCP Socket (epoll 可读事件)
    |
    v
>> tcpsock_server::ioprocessor (单 IO 线程, epoll_wait)

1. recv(fd, buffer, bufsize)
   - 返回 >0: 读取到的字节数
   - 返回 -2: EAGAIN, 忽略
   - 返回 -1/0: 连接错误, 调用 onIOError

2. user->onRecv(&chunk)
   将收到的数据追加到 tcpsock_user 的 _recvCache (memchunk)
   检查缓存大小，超过 MaxPendingSize (10MB) 则断开连接

3. while (msg = user->getRecvMsg())
   循环调用 _PopMsg() 尝试从缓存中解析完整消息：
     a. 检查 _recvCache 数据 >= headerSize (20字节)
     b. 读取 header 得到 bodySize
     c. 检查 bodySize <= MaxBodySize
     d. 检查 _recvCache 数据 >= headerSize + bodySize
     e. 从缓存中 pop 出完整消息数据，构造 tcp_message

4. dispatchMsg(msg, user)
   根据 msgType 泳道路由到对应 worker 线程的队列：
     a. 取出 msgType = protocol->type(msg)
     b. 查找 _msgTyp2WkIdGrp[msgType] 的 WorkerIdGroup
     c. 轮询取出 workerId，计算 workerIndex
     d. _recvQueues[workerIndex]->push(TcpMsgPtr(msg, user))

    |  工作线程队列
    v
>> tcpsock_server::worker (多工作线程, 默认8个)

5. queue->pop_timeout(ptr, 100ms)
   - SQ_POP_SUCCESS: 取到消息，继续
   - SQ_EXIT: 线程退出

6. user_sp->onRecvMsg(msg)   【虚函数，分支在此】
```

**onRecvMsg 在各类 User 中的分支：**

```
┌─ NetUser::onRecvMsg(msg)
│   ├─ lymsg_helper::unpackMsg → header + body
│   └─ onRequest(&header, body)
│       ├─ 查找 UserHandler = _baseServer->getUserHandler(origin)
│       └─ handler->onRequest(header, body, respData) → reqId
│           ├─ reqId == SYNC_RESPONSE(0):
│           │   立即 post(createResponse(header, respData))
│           ├─ reqId == NO_RESPONSE:
│           │   立即 post(createResponse(header, ""))
│           └─ reqId >= ASYNC_RESPONSE(101):
│               saveReqContext(reqId, this, header, body)
│               存入 _baseServer->_reqShards[分片].map
│
├─ GateUser::onRecvMsg(msg) → NetUser::onRecvMsg(msg)
│   └─ onRequest(header, body)  【重写 NetUser】
│       ├─ type == HANDSHAKE:
│       │   _gateServer->onHandshake(body) → shareKey
│       │   成功: setHandshakeSuccess → post 握手响应
│       │   失败: setHandshakeFail → post 空响应
│       ├─ type & ENC:  解密 → NetUser::onRequest(header, decrypted)
│       └─ 其他:        直接 → NetUser::onRequest(header, body)
│
└─ DBProxyMySQLUser::onRecvMsg(msg)
    ├─ unpackMsg → header + body
    ├─ type == LYMSG_TYPE_DB_MYSQL
    │   ├─ mysql_client_req::deserialize(body)
    │   └─ _proxy->submit(req, &header, this)
    │       ├─ pool->submit(req) → dbReqId
    │       └─ savePenddingReq(dbReqId, header, user, slot)
    │           存入 _poolCtx[slot]->shards[分片].penddingReqs.map
    └─ 其他 type: 日志报错
```

### 4.2 出站流程：业务 post → TCP 发送

写事件采用**主动激活机制**（不再由 Monitor 线程轮询），`post()` 加入消息时激活写监听，`sendData()` 队列清空时取消写监听，减少不必要的 epoll 唤醒。

```
>> 业务层/User 调用 post(unique_ptr<tcp_message>)

1. user->post(std::move(msg))
   将消息追加到 tcpsock_user 的 _wrQueue (list)
   队列上限 MaxPendingMsg (256), 超限返回错误
        |
        v
   enableWrite(true)  【原子 CAS, 仅在状态变化时调用 sp_enable】
   向 epoll 注册该 fd 的 EPOLLOUT 事件（激活写监听）

    |  epoll 可写事件
    v
>> tcpsock_server::ioprocessor (IO线程)

2. user->sendData()
   = _SendData(fd, _wrQueue, _lockWRQ)
     a. 从队列前端 pop 一条消息
     b. send() 发送消息数据
     c. 部分发送则将剩余部分放回队列头
     d. EAGAIN 则将消息放回队列头
     e. 发送出错返回 SEND_FAIL
   - rc == SEND_FAIL: onIOError 断开连接
   - rc == QUEUE_EMPTY: enableWrite(false) 取消写监听（双检锁防竞态）
       - _SendData 返回 QUEUE_EMPTY 后再次加锁检查队列
       - 若队列仍为空 → enableWrite(false) 移除 EPOLLOUT
       - 若中途有新消息入队 → 保持监听，下次可写事件继续发送
```

### 4.3 异步回包流程（两种模式）

#### 模式 A：NetServer 业务异步

```
业务逻辑完成
    |
    v
NetServer::response(reqId, respData)
    |  1. 在 _reqShards[shardIdx(reqId)] 中查找 ReqContext
    |  2. 取出 ctx (含 userWeak + reqHeader)
    |  3. user = ctx->userWeak.lock() → NetUser
    |  4. user->createResponse(&ctx->reqHeader, respData)
    |     - 设置 respHeader.origin = _baseOrigin
    |     - 设置 respHeader.type = reqHeader.type | LYMSG_TYPE_RESP
    |     - 设置 respHeader.serial = reqHeader.serial
    |     - lymsg_helper::packMsg → tcp_message
    |  5. user->postMsg(resp) → 进入出站流程
    |
    v
TCP → 客户端
```

#### 模式 B：DBProxy 数据库异步

```
数据库回包
    |
    v
mysql_client_pool::worker → 处理完成 → listener->onResponse(req, resp)
    |
    v
DBProxyMySQL::onResponse(req, resp)
    |  1. onResponseBase(req->req_id, resp->serialize(), req->db_slot)
    |     a. 在 _poolCtx[slot]->shards[分片] 中查找 PenddingReq
    |     b. user = pendReq->userWeak.lock() → DBProxyMySQLUser
    |     c. user->createResponse(&pendReq->reqHeader, respData)
    |        - 创建 lymsg_header: RESP 位 + 原 serial
    |        - lymsg_helper::packMsg → tcp_message
    |     d. user->post(resp) → 进入出站流程
    |
    v
TCP → 调用方服务 (NetServer)
    |
    v  (回到模式 A 或直接到业务层)
NetClient::onResponse(msg)
    |  1. unpackMsg → respHeader + respData
    |  2. 查找 _msgHandlers[serial] (per-serial回调)
    |  3. 或调用 _defaultResponser->onResponse()
    |
    v
业务层处理数据库结果
```

### 4.4 网关握手与加解密流程

```
客户端 → GateServer (握手阶段)
    |
    v
GateUser::onRequest(header, body)
    |  type == LYMSG_TYPE_HANDSHAKE
    |    |
    |    v
    |  GateServer::onHandshake(body, shareKey)
    |    |  1. RSA 解密客户端的 HandshakeReq
    |    |  2. 打印握手请求日志
    |    |  3. 创建 HandshakeResp (含服务端 DH/RSA 公钥)
    |    |  4. 用客户端公钥计算共享密钥 shareKey
    |    |  5. 用临时对称密钥加密 HandshakeResp
    |    |  6. 返回加密后的握手响应
    |    |
    |    v
    |  setHandshakeSuccess(shareKey, sessionType)
    |    |  - _secret = shareKey[0:16]  (AES 密钥)
    |    |  - _ivec   = shareKey[16:32] (初始向量)
    |    |  - _handshakeFin = true
    |    |
    |    v
    |  post(createResponse(header, 加密握手响应))
    |
    v
客户端收到握手响应 → 计算相同共享密钥 → 后续通信加密

--- 后续加密通信 ---

GateUser::onRequest(header, body)
    |  type & LYMSG_TYPE_ENC
    |    |
    |    v
    |  if (!isHandshakeDone()): 拒绝，日志报错
    |  decrypt(body, decrypted) → AES 解密
    |  NetUser::onRequest(header, decrypted) → 正常业务流

GateUser::createResponse(header, body)
    |  type & LYMSG_TYPE_ENC
    |    |
    |    v
    |  encrypt(body, encrypted) → AES 加密
    |  加密失败 → 返回空 body (通知客户端重新握手)
    |  NetUser::createResponse(header, encrypted) → 打包
```

### 4.5 DB 客户端请求流程

```
业务层 → DBClientMySQL::request(header, req, callback)
    |
    v
NetClient::request(header, serializedReq, handler)
    |  1. 设置 header->origin = _localOrigin
    |  2. lymsg_helper::packMsg → tcp_message
    |  3. tcpsock_ha_asynclt::post(msg)
    |     - round-robin 选择一个 tcpsock_asynclt
    |     - 加入该 client 的 _wrQueue
    |  4. 保存 handler 到 _msgHandlers[header->serial]
    |
    v
async_client_manager::monitor → enableWrite → ioprocessor → sendData → TCP

--- 回包路径 ---

TCP → async_client_manager::ioprocessor
    |  recv → asynclt->onRecv → getRecvMsg
    |
    v
async_client_manager::worker
    |  notifyRecv → ha_clt->onResponse(msg)
    |
    v
NetClient::onResponse(msg)
    |  1. unpackMsg → respHeader + respData
    |  2. 查找 _msgHandlers[respHeader.serial]
    |  3. 如果找到: handler(respHeader, respData) → 业务回调
    |  4. 否则: _defaultResponser->onResponse(respHeader, respData)
```

---

## 5. 三种服务详解

### 5.1 NetServer（普通微服务）

**定位**：标准业务微服务，通过 `UserHandler` 回调处理客户端连接上的请求。

**核心组件**：

| 组件 | 说明 |
|------|------|
| `_userHandlers` | `map<origin, UserHandler*>`，按 origin 注册业务处理器 |
| `_reqShards[16]` | 分片 ReqContext 存储，用于异步请求匹配 |
| `_reqIdCreator` | 原子 request_id 生成器，起始值 `ASYNC_RESPONSE (101)` |
| `_asynCltMgr` | `async_client_manager`，管理出站 HA 客户端 |
| `_timerHelper` | 定时器，每 5 分钟打印状态 + 内存统计 |

**使用方式**：

```cpp
// 1. 创建服务
ServerConfig conf("MyService", 1001, "0.0.0.0", 9000);
NetServer server(conf);

// 2. 注册业务处理器
server.registerUserHandler(1001, new MyHandler());

// 3. 添加出站客户端（如需调用其他服务）
NetClient* dbClient = server.addAsynClient(dbConf);

// 4. 启动
server.open();            // 绑定端口
server.start(8);          // 8个工作线程, IO线程, 监控线程
server.serveUtilStop();   // 等待结束
```

**异步处理模式**：

```
MyHandler::onRequest(header, reqData, syncRespData)
    → 发起异步操作（如调用 DB）
    → 返回 reqId (>=101)
    ...
异步操作完成
    → server->response(reqId, respData)
    → 框架自动找到对应连接 → post 响应
```

### 5.2 GateServer（网关服务）

**定位**：在 NetServer 基础上增加传输层安全。客户端必须先完成 RSA 握手建立 AES 会话密钥，后续通信使用对称加密。

**核心组件**：

| 组件 | 说明 |
|------|------|
| `_serverSide` | `handshake_helper`，服务端握手逻辑 |
| `_session_symsec_type` | 会话加密算法（默认 AES_CFB） |

**握手流程**：

```
1. 客户端发送 HandshakeReq (RSA 加密)
   含客户端公钥、临时对称密钥

2. GateServer 处理:
   - RSA 解密 HandshakeReq
   - 计算共享密钥 (32字节) = _secret [0:16] + _ivec [16:32]
   - 创建 HandshakeResp (含服务端公钥)
   - 用临时对称密钥加密 HandshakeResp

3. GateServer 返回 HandshakeResp (临时对称密钥加密)

4. 客户端计算相同共享密钥 → AES 加密通信开始
```

**关键设计**：

- `GateUser::onRequest()` 是三路分支：握手 → 解密 → 明文委托
- `GateUser::createResponse()` 中加密失败时返回空 body，而非崩溃，避免客户端挂超时
- 握手未完成前拒绝加密消息，防止未授权访问

### 5.3 DBProxy 数据库代理服务

**定位**：独立进程，接收来自 NetServer 的数据库请求，通过连接池转发到实际数据库，异步回包。

**核心组件**：

| 组件 | 说明 |
|------|------|
| `_poolCtx[64]` | 64 个 DB 槽位，每个槽位有独立的 `DBCltPoolCtx`（16 分片 PenddingReq 映射） |
| `_pools[64]` | 每个槽位对应一个连接池 |
| `PenddingReq` | 挂起请求上下文：`userWeak + reqHeader` |
| `DBSlotNo` | `uint8_t`，请求中的目标数据库槽位 |

**架构特点**：

```
NetServer (业务进程)
   DBClientMySQL
     → NetClient::request()
       → lymsg_helper::packMsg
         → TCP ──────────────────→

                                    DBProxyMySQL (独立进程)
                                      DBProxyMySQLUser::onRecvMsg
                                        → deserialize
                                        → submit(pool, req)
                                      mysql_client_pool
                                        → 实际 MySQL 连接
                                      pool_listener::onResponse
                                        → onResponseBase
                                    TCP ←── post 响应 ←──

NetServer (业务进程)
   ← NetClient::onResponse ←──
   ← 业务回调
```

**DB 客户端包装器（用于业务层调用 DB 代理）**：

```cpp
// DBClientMySQL 封装了序列化/反序列化
DBClientMySQL db(dbNetClient);
db.request(&header, &mysqlReq, [](auto* rh, auto* resp) {
    // resp 已反序列化为 mysql_client_resp
});
```

**槽位机制**：每个 DB 代理可管理最多 64 个数据库连接池，通过 `DBSlotNo` 路由到不同库。

---

## 6. 线程模型

### 6.1 tcpsock_server 线程模型

```
IO Thread (1)
  epoll_wait 处理所有 fd 的读写事件
    |
    +-- 可读事件 --> recv() 解析消息
    |                    |
    |                    v
    |               dispatchMsg()
    |               按 msgType 路由
    |                    |
    |     +--------------+--------------+
    |     |              |              |
    |     v              v              v
    |  Worker 1      Worker 2 ...  Worker N
    |  recvQueue[0]  recvQueue[1]   recvQueue[N-1]
    |  pop → onRecvMsg
    |
    +-- 可写事件 --> sendData() 从写队列取

Monitor Thread (1)
  定时 20ms 扫描
  → 检查超时/关闭 → 触发 onDisconnect
  (不再轮询写队列，写激活由 post()/sendData() 主动触发)
```

**泳道路由机制**：`configSwimlanes(lanes)` 可将特定 `msgType` 的消息路由到指定的 worker 线程子集，实现资源隔离。

### 6.2 async_client_manager 线程模型

```
IO Thread (1)
  epoll_wait 管理所有出站 fd
  recv + write
    |
    +-- recv 事件 → 唤醒 worker
         |
         +--------------+--------------+
         |                             |
         v                             v
     Worker 1 ...                Worker N
     按 workerIndex 分片           notifyRecv →
     处理 client                  ha_clt->onResponse

Monitor Thread (1)
  定时扫描
  → 自动重连/新连接 → addMonitored
  → 心跳检测 → post heartbeat
  (写激活由 tcpsock_asynclt::post()/sendData() 主动触发，同 4.2 节)
```

---

## 7. 文件索引

| 文件 | 内容 |
|------|------|
| `zbf/socket_tcp_v5.hpp` | 框架层：tcpsock_server / tcpsock_user / tcpsock_client / 线程模型 |
| `zbf/socket_poll.hpp` | epoll 封装 |
| `zbf/tcp_message.hpp` | tcp_message 数据结构 + tcp_message_protocol 抽象协议 |
| `lygc/lymsg_protocol.hpp` | lymsg_header 定义 + lymsg_protocol 协议实现 + lymsg_helper 编解码 |
| `lygc/lyserver_config.hpp` | ServerConfig / ServerGroupMap 配置管理 |
| `lygc/net_server.hpp` | NetUser / NetClient / NetServer / ReqContext |
| `lygc/gate_server.hpp` | GateUser / GateServer (握手 + 加密) |
| `lygc/dbproxy_base.hpp` | DBProxyUserBase / DBProxyBase / PenddingReq / DBCltPoolCtx |
| `lygc/dbproxy_mysql.hpp` | DBProxyMySQLUser / DBProxyMySQL |
| `lygc/dbproxy_mongo.hpp` | DBProxyMongoUser / DBProxyMongo |
| `lygc/dbproxy_redis.hpp` | DBProxyRedisUser / DBProxyRedis |
| `lygc/dbclient_mysql.hpp` | DBClientMySQL (业务层适配器) |
| `lygc/dbclient_mongo.hpp` | DBClientMongo (业务层适配器) |
| `lygc/dbclient_redis.hpp` | DBClientRedis (业务层适配器) |
| `wjp/mysql_client.hpp` | MySQL 连接池 |
| `wjp/mongo_client.hpp` | MongoDB 连接池 (mongoc driver) |
| `wjp/redis_client.hpp` | Redis 连接池 |
