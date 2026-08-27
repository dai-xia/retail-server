# RetailServer

无人零售自助收银系统的服务端。基于 C/S 架构，管理多台收银终端，负责会员、商品、订单、库存、OTA 等核心业务。

## 技术亮点

### 自研 C 网络框架：epoll / io_uring 双后端

服务端没有使用现成的网络库，而是自己实现了一套高性能 C 网络框架，核心是**主/子 Reactor 架构**，并通过 `USE_IOURING` 宏在编译期切换 epoll / io_uring 两个后端，业务代码完全一致：

```
                    ┌─────────────────┐
                    │   主 Reactor     │
                    │  accept 线程     │
                    │ epoll/io_uring  │
                    └────────┬────────┘
                             │ 新连接分发
              ┌──────────────┼──────────────┐
              ▼              ▼              ▼
     ┌────────────┐  ┌────────────┐  ┌────────────┐
     │ 子 Reactor 0│  │ 子 Reactor 1│  │ 子 Reactor 3│
     │epoll/uring │  │epoll/uring │  │epoll/uring │
     └──────┬─────┘  └──────┬─────┘  └──────┬─────┘
            └───────────────┼───────────────┘
                            ▼ 可读事件
                    ┌───────▼────────┐
                    │   线程池(8线程) │
                    │   业务任务处理   │
                    └────────────────┘
```

**两个后端（编译期二选一）**

- **epoll 后端**（`nf_epoll.c`）：经典的主/子 Reactor + 边缘触发，单次 `epoll_wait` 最多取出 1024 个事件批量处理。
- **io_uring 后端**（`nf_iouring.c`）：把 accept / read / write 全部改成异步 SQE 提交 + CQE 收割，进一步减少系统调用与用户态/内核态切换。

### io_uring 后端的关键设计

io_uring 后端不是简单地把 epoll 换成 io_uring，而是针对高性能网络做了以下优化：

- **SQPOLL 内核轮询**：每个子 Reactor 用 `io_uring_queue_init(1024, &ring, IORING_SETUP_SQPOLL)` 创建 1024 项 SQ 环，由内核线程自动收割 SQ，避免每次提交都进入内核；内核不支持 SQPOLL 时自动降级为普通模式（`flags=0`）。
- **主 Reactor 专用环**：主线程用独立的 256 项 ring（`io_uring_queue_init(256, ..., 0)`），只做 accept，不启用 SQPOLL。
- **固定缓冲区零拷贝**：预分配 512 个 64KB 读缓冲，通过 `io_uring_register_buffers` 注册，读请求用 `io_uring_prep_read_fixed` 直接写入固定缓冲，省去每次 read 的缓冲分配；注册失败时自动回退到逐连接缓冲。
- **eventfd 跨线程唤醒**：主线程把新连接交给子 Reactor、或外部请求关闭连接时，通过 eventfd（`UR_OP_EVENT`）唤醒子线程。
- **CQE user_data 操作标签**：借用 64 位虚拟地址高 16 位中的高 2 位编码操作类型（READ / WRITE / EVENT / CANCEL），低 62 位存连接指针，一次 CQE 收割即可路由到对应处理。
- **SQE 耗尽重试队列**：SQ 环满导致 `submit_read` 提交失败时，连接进入 256 项重试队列，下一轮重新提交，不丢事件。
- **写重复抑制**：用 `write_sqe_pending` 标志避免同一 fd 同时存在多个写 SQE。
- **批量 CQE 收割**：`io_uring_for_each_cqe` + `io_uring_cq_advance` 一次处理多个完成事件，减少 syscall 次数。
- **关闭语义**：关闭带在途读请求的连接时，用 `io_uring_prep_cancel`（`UR_OP_CANCEL`）取消在途读，避免 fd 复用后收到脏数据。

- **关键参数**：epoll 单次最多 1024 事件；io_uring 子 Reactor SQ 环 1024 项、主 Reactor 256 项、固定缓冲池 512×64KB；读缓冲 64KB、4 个子 Reactor、8 个工作线程、任务队列 256。
- 主线程只负责 accept，读写全部下沉到子 Reactor，业务处理交给线程池，避免单线程阻塞。

### C++ 与 C 的桥接

Qt 界面层是 C++，网络框架是 C。`CFrameworkAdapter` 把 C 回调（`on_accept` / `on_recv` / `on_close`）转成 Qt 信号，业务层用信号槽处理，处理完再通过桥接发回 C 框架。

### 按 client_id 的数据隔离

服务端用 MySQL 存全局数据，但商品、库存按 `client_id` 隔离——每个收银终端只看到自己的数据。连接层的 `fd` 和业务层的 `client_id` 通过映射表解耦，断线重连不影响业务标识。

## 目录结构

```
server/          Qt 服务端
  ui/            管理界面（会员/商品/订单/客户端监控/OTA 面板）
  core/          核心层
    net_framework.c/h     C 网络框架（主/子 Reactor）
    nf_epoll.c/h          epoll 后端
    nf_iouring.c/h        io_uring 后端
    iouring_engine.c/h    io_uring 引擎
    thread_pool.c/h       C 线程池
    servermanager.cpp/h   fd <-> client_id 映射
    businessmanager.cpp/h 业务逻辑（命令分发）
    cframeworkadapter.cpp/h  C 框架 <-> Qt 信号桥接
    databasemanager.cpp/h  MySQL（连接池）
    stream_receiver.cpp/h  视频流接收
common/         共享库（AES 加密 / 日志 / cJSON / 公共类型）
3rdparty/liburing/   内置 liburing 头文件
scripts/setup_liburing.sh   安装 liburing
```

## 通信协议

数据帧 = `4 字节长度头（网络序） + JSON 体`。

主要命令（与客户端保持一致）：

| 命令 | 方向 | 说明 |
|------|------|------|
| `client_register` / `heartbeat` | C→S | 注册 / 心跳 |
| `goods_add` / `goods_update` / `goods_delete` | C→S | 商品管理 |
| `goods_sync_report` / `goods_sync_request` | C→S | 库存同步 |
| `member_register` / `member_query` / `member_recharge` / `member_update` | C→S | 会员管理 |
| `member_verify_password` | C→S | 密码验证 |
| `order_create` / `order_query` | C→S | 下单 / 查单 |
| `stock_deduct` / `balance_update` | C→S | 扣库存 / 改余额 |
| `face_verify` | C→S | 人脸验证 |

响应统一格式：`{"cmd":"...","code":0,"msg":"...","data":{}}`，`code=0` 表示成功。

## 编译运行

```bash
# 依赖
sudo apt install qt5-default libqt5sql5-mysql libmysqlclient-dev libcjson-dev
./scripts/setup_liburing.sh     # io_uring 后端需要

# qmake 编译
cd server && qmake RetailServer.pro && make -j$(nproc)

# 或 CMake
cmake -B build && cmake --build build -j

# 运行
./RetailServer
```

## License

MIT，详见 [LICENSE](LICENSE)。
