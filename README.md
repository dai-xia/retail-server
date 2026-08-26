# RetailServer

无人零售自助收银系统 · 服务端。采用 C/S 架构，管理多台客户端收银终端，提供会员、商品、订单、库存、OTA 等核心业务。

## 技术栈

- **C 风格 epoll / io_uring 双后端 + 线程池** 高性能网络框架（主/子 Reactor 架构）
- **Qt** GUI 管理界面
- **MySQL** 全局存储，按 `client_id` 隔离各客户端数据
- **AES-256-GCM** 加密通信

## 目录结构

```
RetailServer/
├── server/                          # 服务端源码
│   ├── main.cpp                     # 程序入口
│   ├── RetailServer.pro             # Qt 项目文件
│   ├── CMakeLists.txt               # CMake 构建
│   ├── ui/                          # 界面层（管理界面）
│   │   ├── mainwindow.*             # 主窗口
│   │   ├── memberwidget.*           # 会员管理
│   │   ├── goodswidget.*            # 商品管理
│   │   ├── orderwidget.*            # 订单管理
│   │   ├── clientwidget.*           # 客户端监控
│   │   ├── otawidget.*              # OTA 面板
│   │   ├── monitor_widget.*         # 监控
│   │   ├── video_player_widget.*    # 视频播放
│   │   └── stylehelper.*            # 样式
│   └── core/                        # 核心层
│       ├── servermanager.*          # fd <-> client_id 映射
│       ├── businessmanager.*        # 业务逻辑（命令分发）
│       ├── cframeworkadapter.*      # C 框架 <-> Qt 信号桥接
│       ├── databasemanager.*        # MySQL（连接池）
│       ├── net_framework.c/h        # C epoll/io_uring 网络框架
│       ├── nf_epoll.c/h             # epoll 后端
│       ├── nf_iouring.c/h           # io_uring 后端
│       ├── iouring_engine.c/h       # io_uring 引擎
│       ├── stream_receiver.*        # 视频流接收
│       └── thread_pool.c/h          # C 线程池
├── common/                          # 共享库（crypto / logger / cJSON / 公共类型）
├── 3rdparty/liburing/               # 内置 liburing（io_uring 后端依赖）
├── scripts/setup_liburing.sh        # 安装 liburing 依赖
├── CMakeLists.txt                   # 顶层 CMake
└── ...
```

## 通信协议

数据帧：`4 字节长度头（网络序） + JSON 体`。核心命令（与客户端保持一致）：

| 命令 | 方向 | 说明 |
|------|------|------|
| `client_register` / `heartbeat` | C→S | 客户端注册 / 心跳 |
| `goods_add/update/delete/sync_*` | C→S | 商品管理与库存同步 |
| `member_register/query/recharge/update` | C→S | 会员管理 |
| `order_create` / `order_query` | C→S | 下单与订单查询 |
| `stock_deduct` / `balance_update` | C→S | 库存扣减 / 余额更新 |
| `face_verify` | C→S | 人脸验证 |

响应格式：`{"cmd": "...", "code": 0, "msg": "...", "data": {...}}`，`code=0` 表示成功。

## 编译与运行

```bash
# 依赖
sudo apt install qt5-default libqt5sql5-mysql libmysqlclient-dev libcjson-dev

# liburing（io_uring 后端）
./scripts/setup_liburing.sh

# 编译
cd server && qmake RetailServer.pro && make -j$(nproc)
# 或使用 CMake
cmake -B build && cmake --build build -j

# 运行
./RetailServer
```

## License

MIT License — 详见 [LICENSE](LICENSE)。
