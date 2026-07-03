# Nox 项目结构

## 📁 快速导航

### 核心配置
- **[README.md](README.md)** — 项目总体概览
- **[docker-compose.yml](docker-compose.yml)** — 服务编排（C++ 执行引擎、Python 数据引擎、心跳监控）
- **[CMakeLists.txt](CMakeLists.txt)** — C++ 构建配置
- **[.env.example](.env.example)** — 环境变量模板

---

## 🚀 核心服务

### `/execution` — 执行引擎（C++）
支持 Webhook 的订单执行、风险管理与期权信号生成
- 监听 **8080 端口**（对外暴露）
- 接收 TradingView 发来的股票交易 JSON Webhook
- 实现凯利公式仓位管理与移动止损
- 与 Alpaca 券商 API 通信
- **`OptionEngine.hpp`** — 布莱克-舒尔斯（Black-Scholes）定价与希腊字母计算（`/options/price` 接口）
- **`OptionsSignalGenerator.hpp`** — 通过 Telegram 自动生成期权投资建议信号（无需 TradingView）

### `/analyst` — 分析大脑（C++）
市场分析与市场状态（regime）分类
- 每 6 小时运行一个周期（可配置）
- 将交易信号和提醒输出至 Telegram
- 依赖数据引擎提供市场背景信息

### `/china_data_engine` — 中国市场数据（Python）
通过 APScheduler 抓取中国金融数据
- 东方财富、财联社、国家统计局 PMI、央行 LPR
- 15 分钟刷新一次
- 仅供内部使用（8000 端口未对外暴露）

### `/america_data_engine` — 美国市场数据（Python）
Alpaca API 集成与股票数据缓存
- 15 分钟刷新一次
- 仅供内部使用（8000 端口未对外暴露）

### `/heartbeat` — 监控服务（Python）
系统健康检查与 Telegram 通知
- 监控所有数据引擎
- 出现故障时发送告警
- 独立运行

### `/backtest-engine` — 回测引擎（C++）
历史策略测试与样本外验证
- 生成绩效指标
- 帮助验证市场状态分类逻辑
- 通过 `trade_date` 信号字段支持中国 A 股模拟（含 T+1 限制与整手交易规则）

---

## 📊 数据与日志

### `/data`
历史市场数据、回测结果、市场状态分类记录
- 通过 `scripts/download_data.py` 下载

### `/logs`
所有服务的运行日志
- 执行引擎操作记录
- 分析决策记录
- 错误追踪

### `/shared`
共享的 C++ 工具与头文件
- JSON 解析（nlohmann）
- 通用数据结构
- 工具函数

---

## 🧪 测试

位置：**`/docs/testing/`**

| 指南 | 用途 |
|-------|---------|
| [TEST_GUIDE.md](docs/testing/TEST_GUIDE.md) | 完整测试概览 |
| [EXECUTION_TEST_GUIDE.md](docs/testing/EXECUTION_TEST_GUIDE.md) | 测试执行引擎 Webhook |
| [ANALYST_TEST_GUIDE.md](docs/testing/ANALYST_TEST_GUIDE.md) | 测试分析大脑信号 |
| [HEARTBEAT_TEST_GUIDE.md](docs/testing/HEARTBEAT_TEST_GUIDE.md) | 测试监控服务 |
| [DATA_ENGINE_TEST_GUIDE.md](docs/testing/DATA_ENGINE_TEST_GUIDE.md) | 测试数据抓取 |
| [BACKTEST_TEST_GUIDE.md](docs/testing/BACKTEST_TEST_GUIDE.md) | 测试回测引擎（含 A 股规则） |
| [TESTING_PHILOSOPHY.md](docs/testing/TESTING_PHILOSOPHY.md) | 测试原则 |
| [TEST_MAINTENANCE_GUIDE.md](docs/testing/TEST_MAINTENANCE_GUIDE.md) | 测试套件维护指南 |
| [TEST_UPDATE_CHECKLIST.md](docs/testing/TEST_UPDATE_CHECKLIST.md) | 新功能测试检查清单 |

**运行全部测试：**
```bash
./scripts/run_tests.sh
```

**测试源文件：** `/tests/`
- `test_kelly_sizing.cpp` — 仓位管理逻辑
- `test_regime.cpp` — 市场状态分类

---

## 📚 文档

位置：**`/docs/`**

| 文档 | 用途 |
|----------|---------|
| **[NOX_COMPLETE_GUIDE.md](docs/NOX_COMPLETE_GUIDE.md)** | **完整系统指南——从这里开始。用通俗语言覆盖每个功能的端到端说明** |
| [NOX_USER_GUIDE.md](docs/NOX_USER_GUIDE.md) | 原始用户指南——信号故障排查、期权使用方法 |
| [DOCUMENTATION_OVERVIEW.md](docs/DOCUMENTATION_OVERVIEW.md) | 架构与设计 |
| [DOCUMENTATION_SUMMARY.txt](docs/DOCUMENTATION_SUMMARY.txt) | 快速参考 |

**NOX_USER_GUIDE.md 中的关键章节：**
- [为什么我收不到交易信号？](#why-am-i-not-getting-trade-signals) — 6 步诊断法 + 应对措施
- [期权信号生成器](#options-signal-generator) — 使用方法、等级划分、IV 分位数、策略术语表
- [开发你自己的信号](#developing-your-own-signals) — TradingView Webhook 格式、模拟交易

---

## 📈 交易

位置：**`/trading/`**

### `openclaw_weighted_alpha.pinescript`
**OpenClaw 加权 Alpha** — 4 小时动量 + 波动率策略

**标的资产：** SPY、XOM、NVDA、BTCUSD
**信号：** EMA(9) 与 EMA(21) 金叉/死叉 + 成交量放量过滤
**风险等级：**
- **Tier 3**（A 级激进）：伴随成交量放量的入场
- **Tier 1**（标准）：无放量的入场

**Webhook 目标地址：** `http://<YOUR_VPS_IP>/webhook`

---

## 🔧 工具与脚本

位置：**`/scripts/`**

| 脚本 | 用途 |
|--------|---------|
| `download_data.py` | 获取历史市场数据 |
| `refresh_data.sh` | 更新所有数据引擎 |
| `refresh_cron.sh` | 定时后台刷新 |
| `run_tests.sh` | 执行完整测试套件 |

**用法：**
```bash
# 下载数据
python3 scripts/download_data.py

# 刷新所有数据
./scripts/refresh_data.sh

# 运行测试
./scripts/run_tests.sh
```

---

## 🏗️ 构建与开发

### 编译
```bash
mkdir -p build && cd build
cmake ..
make
```

### 源文件
- **`main.cpp`** — 主程序入口
- **`mcpt.cpp`** / **`mcpt.h`** — 蒙特卡洛路径追踪（Monte Carlo Path Tracing）工具
- **`mcpt_example.cpp`** — MCPT 使用示例
- **/tests/** — 单元测试实现

### 构建产物
- **/build/** — 编译生成的可执行文件与测试二进制文件

---

## 🌐 网络拓扑

```
TradingView Webhooks
        ↓
Execution Engine (localhost:8080) ← Direct IP: <YOUR_VPS_IP>:80
        ↓
Analyst Brain ← Data Engines (Internal Network)
        ↓
Alpaca API + Telegram Notifications
```

**要点：**
- 执行引擎直接对外暴露（未经 Traefik 代理）
- 数据引擎运行在内部 `nox_net` 网络中
- 所有服务通过 `docker-compose.yml` 以 Docker 方式运行

---

## ⚡ 快速开始

```bash
# 启动系统
docker-compose up -d

# 查看日志
docker-compose logs -f execution-engine

# 停止系统
docker-compose down
```

---

## 📋 文件组织

```
Nox/
├── docker-compose.yml          # 服务编排
├── CMakeLists.txt              # C++ 构建配置
├── README.md                   # 项目总体概览
├── INDEX.md                    # 本文件
│
├── execution/                  # 订单执行（C++）
├── analyst/                    # 市场状态分析（C++）
├── backtest-engine/            # 回测引擎（C++）
├── china_data_engine/          # 中国数据（Python）
├── america_data_engine/        # 美国数据（Python）
├── heartbeat/                  # 监控服务（Python）
│
├── docs/                       # 文档
│   ├── NOX_USER_GUIDE.md
│   ├── DOCUMENTATION_*.md
│   └── testing/                # 测试指南
│
├── trading/                    # 交易策略
│   └── openclaw_weighted_alpha.pinescript
│
├── scripts/                    # 工具脚本
│   ├── download_data.py
│   ├── refresh_*.sh
│   └── run_tests.sh
│
├── shared/                     # C++ 工具库
├── data/                       # 市场数据
├── logs/                       # 运行日志
├── tests/                      # 单元测试
└── build/                      # 构建产物
```

---

**最后更新：** 2026-06-23
**版本：** 1.2
</content>
</invoke>
