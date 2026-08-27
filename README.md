# RetailServer

无人零售自助收银系统的服务端。基于 C/S 架构，管理多台收银终端，负责会员、商品、订单、库存、OTA 等核心业务。

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

## 网络框架

没用现成网络库，自己写了一套 C 网络框架，核心是主/子 Reactor 多线程模型，通过 `USE_IOURING` 宏在编译期切 epoll 或 io_uring，业务代码不动。

```
                    ┌─────────────────┐
                    │   主 Reactor     │
                    │  accept 线程     │
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

### 双 IO 引擎

- **epoll 后端**（`nf_epoll.c`）：边缘触发，单次 `epoll_wait` 最多取 1024 个事件批量处理。
- **io_uring 后端**（`nf_iouring.c`）：把 accept / read / write 都改成异步 SQE 提交 + CQE 收割，减少系统调用和用户态/内核态切换。

两个后端的长连接 ping 纯 IO 压测（200 并发）对比：io_uring 后端 QPS 是 epoll 的约 3.15 倍，P99 延迟约降为 epoll 的 1/2.8。

### io_uring 内核交互

- 每个子 Reactor 用 `io_uring_queue_init(1024, ..., IORING_SETUP_SQPOLL)` 建 1024 项 SQ 环，由内核线程自动收割 SQ；不支持 SQPOLL 时降级普通模式。
- 子 Reactor 预注册 512 个 64KB 固定读缓冲（`io_uring_register_buffers`），读请求用 `io_uring_prep_read_fixed` 直接写入固定缓冲，省去每次 read 的地址校验和数据拷贝；注册失败回退到逐连接缓冲。
- CQE 的 `user_data` 高位 2 位编码操作类型（READ/WRITE/EVENT/CANCEL），低 62 位放连接指针，内核原样透传，收割时不用查表就能定位业务对象。
- 用 `io_uring_for_each_cqe` + `io_uring_cq_advance` 批量收割完成事件。

### 连接生命周期

- 连接对象用原子引用计数（`__sync_fetch_and_add/sub`）管理，业务线程和 IO 线程都能安全持有。
- 关闭的连接进 `closed_conns` 缓冲，统一在子线程事件循环里回收，从架构上避免多线程 use-after-free。
- 关闭带在途读的连接时用 `io_uring_prep_cancel` 取消飞行中的 SQE，避免 fd 复用后收到脏数据。
- SQ 环满导致提交失败时，连接进 `retry_conns` 重试队列，下一轮无损重试。

### 协议与调度

- 应用层协议：`4 字节大端长度头 + JSON`。连接级残留缓冲区处理粘包/半包，一个完整帧才触发 `on_recv`。
- 写路径用链表组织的异步写队列，`eventfd` 跨线程唤醒子 Reactor，避免多线程并发写同一连接。
- 线程池任务带 `func + cleanup` 双回调，异常路径（线程池销毁/任务丢弃）也能正确释放连接引用，不泄漏。

## C++ 与 C 的桥接

Qt 界面层是 C++，网络框架是 C。`CFrameworkAdapter` 把 C 回调（`on_accept` / `on_recv` / `on_close`）转成 Qt 信号，业务层用信号槽处理，处理完再通过桥接发回 C 框架。

## 按 client_id 的数据隔离

服务端用 MySQL 存全局数据，但商品、库存按 `client_id` 隔离——每个收银终端只看到自己的数据。连接层的 `fd` 和业务层的 `client_id` 通过映射表解耦，断线重连不影响业务标识。

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
