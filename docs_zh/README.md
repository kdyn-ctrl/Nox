# Nox 交易系统 — 完整文档 (中文)

Nox 交易引擎的完整文档，按用途组织。

---

## 🚀 快速开始

**初次接触 Nox？** 从这里开始。

- [**START_HERE.md**](setup/START_HERE.md) — 系统概览、功能状态、日常操作（15 分钟阅读）
- [**TICKER_STRATEGY.md**](setup/TICKER_STRATEGY.md) — 头寸规模、推荐观察列表、迁移路径（模拟 → $5k → $20k+）
- [**WATCHLIST_RECOMMENDATIONS.md**](setup/WATCHLIST_RECOMMENDATIONS.md) — 具体股票选择、低价股表、回测示例

---

## 📚 指南与策略

深入的操作指南和系统设计。

- [**STRATEGY_DEVELOPMENT_GUIDE.zh.md**](guides/STRATEGY_DEVELOPMENT_GUIDE.zh.md) — 交易策略的构建和验证
- [**DESIGN_THINKING.zh.md**](guides/DESIGN_THINKING.zh.md) — 架构原理和设计决策
- [**NOX_COMPLETE_GUIDE.zh.md**](guides/NOX_COMPLETE_GUIDE.zh.md) — 包含所有功能的完整系统指南
- [**NOX_USER_GUIDE.zh.md**](guides/NOX_USER_GUIDE.zh.md) — 面向用户的操作指南
- [**PAPER_TRADING_READINESS.zh.md**](guides/PAPER_TRADING_READINESS.zh.md) — 部署前验证检查清单
- [**TRAILING_STOP_MONITOR.md**](../docs/guides/TRAILING_STOP_MONITOR.md) — 经纪商端止损配置和监控（英文）

---

## 🔍 参考资料

系统文档、架构和变更历史。

- [**PROJECT_STATUS.md**](../docs/reference/PROJECT_STATUS.md) — 当前架构、功能矩阵、数据源（英文）
- [**CHANGES_JULY_2026.md**](../docs/reference/CHANGES_JULY_2026.md) — 关键错误修复、发布说明、信号消息格式（英文）
- [**DOCUMENTATION_OVERVIEW.zh.md**](reference/DOCUMENTATION_OVERVIEW.zh.md) — 文档结构和用途
- [**INDEX.zh.md**](reference/INDEX.zh.md) — 文档索引
- [**FUTURE_WORK.md**](../docs/reference/FUTURE_WORK.md) — 路线图和计划功能（英文）

---

## 🧪 测试

测试策略、指南和最佳实践。

- [**TESTING_README.md**](../docs/testing/TESTING_README.md) — 测试概览和方法论（英文）
- **按组件：**
  - [ANALYST_TEST_GUIDE.zh.md](testing/ANALYST_TEST_GUIDE.zh.md) — 市场分析引擎测试
  - [BACKTEST_TEST_GUIDE.zh.md](testing/BACKTEST_TEST_GUIDE.zh.md) — 回测框架和验证
  - [DATA_ENGINE_TEST_GUIDE.zh.md](testing/DATA_ENGINE_TEST_GUIDE.zh.md) — 数据管道测试
  - [EXECUTION_TEST_GUIDE.zh.md](testing/EXECUTION_TEST_GUIDE.zh.md) — 订单执行测试
  - [HEARTBEAT_TEST_GUIDE.zh.md](testing/HEARTBEAT_TEST_GUIDE.zh.md) — 监控和健康检查测试
  - [TEST_GUIDE.zh.md](testing/TEST_GUIDE.zh.md) — 快速测试参考
  - [TEST_MAINTENANCE_GUIDE.zh.md](testing/TEST_MAINTENANCE_GUIDE.zh.md) — 测试套件维护
  - [TEST_UPDATE_CHECKLIST.zh.md](testing/TEST_UPDATE_CHECKLIST.zh.md) — 测试更新流程
- [**TESTING_PHILOSOPHY.md**](../docs/testing/TESTING_PHILOSOPHY.md) — 测试哲学和原理（英文）

---

## 📋 规则与配置

- [**Rules/**](../docs/Rules/) — 信号生成规则和常数（英文）

---

## 📖 开发日志

- [**journal/**](../docs/journal/) — 开发日志、决策和事件记录（英文）

---

## 🌐 English Documentation

English versions available in: [`../docs/README.md`](../docs/README.md)

---

## 快速导航

### 按角色

**交易员（使用系统）**
1. [START_HERE.md](setup/START_HERE.md)
2. [TICKER_STRATEGY.md](setup/TICKER_STRATEGY.md)
3. [NOX_USER_GUIDE.zh.md](guides/NOX_USER_GUIDE.zh.md)

**开发者（修改系统）**
1. [DESIGN_THINKING.zh.md](guides/DESIGN_THINKING.zh.md)
2. [NOX_COMPLETE_GUIDE.zh.md](guides/NOX_COMPLETE_GUIDE.zh.md)
3. [测试指南](testing/) (按组件)

**量化研究员（策略开发）**
1. [STRATEGY_DEVELOPMENT_GUIDE.zh.md](guides/STRATEGY_DEVELOPMENT_GUIDE.zh.md)
2. [BACKTEST_TEST_GUIDE.zh.md](testing/BACKTEST_TEST_GUIDE.zh.md)
3. [NOX_COMPLETE_GUIDE.zh.md](guides/NOX_COMPLETE_GUIDE.zh.md)

### 按问题

- **"Nox 是什么？"** → [START_HERE.md](setup/START_HERE.md)
- **"如何设置我的观察列表？"** → [TICKER_STRATEGY.md](setup/TICKER_STRATEGY.md) + [WATCHLIST_RECOMMENDATIONS.md](setup/WATCHLIST_RECOMMENDATIONS.md)
- **"如何部署它？"** → [PAPER_TRADING_READINESS.zh.md](guides/PAPER_TRADING_READINESS.zh.md)
- **"最近有什么变化？"** → [CHANGES_JULY_2026.md](../docs/reference/CHANGES_JULY_2026.md)（英文）
- **"系统架构如何工作？"** → [DESIGN_THINKING.zh.md](guides/DESIGN_THINKING.zh.md)
- **"如何测试新功能？"** → [测试指南](testing/)
- **"当前状态如何？"** → [PROJECT_STATUS.md](../docs/reference/PROJECT_STATUS.md)（英文）

---

## 文件组织

```
docs_zh/
├── README.md (你在这里)
├── setup/               ← 快速开始
│   ├── START_HERE.md
│   ├── TICKER_STRATEGY.md
│   └── WATCHLIST_RECOMMENDATIONS.md
├── guides/              ← 操作指南和深入讨论
│   ├── STRATEGY_DEVELOPMENT_GUIDE.zh.md
│   ├── DESIGN_THINKING.zh.md
│   ├── NOX_COMPLETE_GUIDE.zh.md
│   ├── NOX_USER_GUIDE.zh.md
│   └── PAPER_TRADING_READINESS.zh.md
├── reference/           ← 文档和状态
│   ├── DOCUMENTATION_OVERVIEW.zh.md
│   ├── INDEX.zh.md
│   └── DOCUMENTATION_SUMMARY.zh.txt
├── testing/             ← 测试策略和指南
│   ├── ANALYST_TEST_GUIDE.zh.md
│   ├── BACKTEST_TEST_GUIDE.zh.md
│   ├── DATA_ENGINE_TEST_GUIDE.zh.md
│   ├── EXECUTION_TEST_GUIDE.zh.md
│   ├── HEARTBEAT_TEST_GUIDE.zh.md
│   └── ...
└── Rules/               ← 配置规则
```

---

**最后更新：** 2026年7月2日  
**状态：** 经过30天模拟交易验证后可用于实盘交易
