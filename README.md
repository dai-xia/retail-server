# RetailServer

无人零售自助收银系统的服务端。基于 C/S 架构，管理多台收银终端，负责会员、商品、订单、库存、OTA 等核心业务。

## 技术亮点

### 自研 C 网络框架：epoll / io_uring 双后端

服务端没有使用现成的网络库，而是自己实现了一套高性能 C 网络框架，核心是**主/子 Reactor 架构**：

```
                    ┌─────────────────┐
                    │   主 Reactor     │
                    │  accept 线程     │
                    │  epoll/io_uring │
                    └────────┬────────┘
                             │ 新连接分发
              ┌──────────────┼──────────────┐
              ▼              ▼              ▼
     ┌────────────┐  ┌────────────┐  ┌────────────┐
     │ 子 Reactor 0│  │ 子 Reactor 1│  │ 子 Reactor N│
     │ epoll_wait │  │ epoll_wait │  │ epoll_wait │
     └──────┬─────┘  └──────┬─────┘  └──────┬─────┘
            └───────────────┼───────────────┘
                            ▼ 可读事件
                    ┌───────▼────────┐
                    │   线程池(8线程) │
                    │   业务任务处理   │
                    └────────────────┘
```

- **编译期切换后端**：`USE_IOURING` 宏控制用 epoll 还是 io_uring，同一套业务代码不改。
- **关键参数**：单次 epoll_wait 最大 1024 事件、读缓冲 64KB、4 个子 Reactor、8 个工作线程、任务队列 256。
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
