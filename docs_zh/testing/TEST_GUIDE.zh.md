# Nox 测试指南

## 📚 全部测试指南

本仓库包含用于测试和理解 Nox 的完整指南：

### 面向所有人

| 指南 | 用途 |
|-------|---------|
| **[NOX_USER_GUIDE.md](NOX_USER_GUIDE.md)** | 非技术用户：机器人做什么、如何解读警报、如何监控 |

### 面向开发者

| 组件 | 指南 | 用途 |
|-----------|-------|---------|
| **核心逻辑** | [TEST_GUIDE.md](TEST_GUIDE.md) | RegimeStateMachine、Kelly 仓位、MCPT 单元测试 |
| **分析师（Analyst）** | [ANALYST_TEST_GUIDE.md](ANALYST_TEST_GUIDE.md) | 数据抓取、市场状态分类、信号传输 |
| **执行（Execution）** | [EXECUTION_TEST_GUIDE.md](EXECUTION_TEST_GUIDE.md) | 订单校验、路由、仓位规模计算 |
| **回测（Backtest）** | [BACKTEST_TEST_GUIDE.md](BACKTEST_TEST_GUIDE.md) | 历史模拟、参数优化 |
| **数据引擎** | [DATA_ENGINE_TEST_GUIDE.md](DATA_ENGINE_TEST_GUIDE.md) | 新闻/宏观数据服务（美国与中国） |
| **心跳（Heartbeat）** | [HEARTBEAT_TEST_GUIDE.md](HEARTBEAT_TEST_GUIDE.md) | 监控、情报报告、SEC 雷达 |
| **维护** | [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md) | 何时以及如何更新测试指南 |

**👉 从这里开始**：如果你正在新增代码，请先阅读 [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md)，了解需要更新哪些指南。

---

## 快速开始

运行全部测试：
```bash
./run_tests.sh
```

## 单项测试目标

### RegimeStateMachine 测试
```bash
g++ -std=c++17 -pthread -o build/test_regime tests/test_regime.cpp
./build/test_regime
```
**测试内容：** 基于 VIX 与 SPY 价格相对 200 日均线的市场状态分类（RISK_ON/RISK_OFF/TRANSITION）。

- **RISK_ON**：VIX < 35 且 SPY > 200 日均线（看涨、低波动）
- **RISK_OFF**：VIX >= 35 或 SPY < 200 日均线 * 0.98（危机模式，卖出信号）
- **TRANSITION**：SPY 位于 200 日均线与 200 日均线 * 0.98 之间，且 VIX < 35（不确定状态）

### Kelly 仓位测试
```bash
g++ -std=c++17 -pthread -o build/test_kelly_sizing tests/test_kelly_sizing.cpp
./build/test_kelly_sizing
```
**测试内容：** 仓位规模计算的合理性检查，确保在组合资金连一股都买不起时不会进行交易。

### MCPT（蒙特卡洛置换检验）示例
```bash
g++ -std=c++17 -pthread -o build/test_mcpt_example mcpt_example.cpp mcpt.cpp
./build/test_mcpt_example
```
**测试内容：** MCPT 库在 1,000 次置换下的功能验证：
- 单次置换（串行）
- 带回调的批处理模式
- 具备线程级 RNG 隔离的并行模式

验证置换后的收益序列仍保持原始收益的均值与方差不变。

### MCPT 主演示
```bash
g++ -std=c++17 -pthread -o build/mcpt_main main.cpp mcpt.cpp
./build/mcpt_main
```
**测试内容：** MCPT 的基本用法——将历史收益随机打乱 1,000 次，确认统计特性得以保持。

## 各测试验证内容一览

| 测试 | 用途 | 状态 |
|------|---------|--------|
| `test_regime` | 市场状态分类规则 | ✅ 通过 |
| `test_kelly_sizing` | 仓位规模安全性 | ✅ 通过 |
| `test_mcpt_example` | MCPT 性能与正确性 | ✅ 通过 |
| `mcpt_main` | MCPT 基础功能 | ✅ 通过 |

## 不使用 CMake 构建

本项目目前不需要 CMake，直接编译即可：
```bash
g++ -std=c++17 -pthread [source files] -o executable_name
```

所有测试都可仅依赖 C++ 标准库独立编译。

## 持续验证

在修改代码后，重新编译并测试：
```bash
./run_tests.sh
```

该脚本会：
1. 从源码编译全部测试
2. 依次运行每个测试
3. 汇报通过/失败状态
