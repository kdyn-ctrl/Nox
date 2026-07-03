# Nox 测试文档

欢迎！本目录包含 Nox 交易系统的完整测试指南。请通过本文件导航到与你当前工作相关的指南。

## 👤 非技术用户

**如果你不是开发者**，请从这里开始：
- **[NOX_USER_GUIDE.md](NOX_USER_GUIDE.md)** — 了解机器人的功能、如何解读告警、如何监控运行表现
  - 用通俗易懂的语言解释 Nox 的工作原理
  - 每条 Telegram 告警代表什么意思
  - 如何判断系统是否出现异常
  - 常见问题解答（FAQ）
  - 无需任何编程知识！

---

## 🚀 我想要...

### ...编写代码并提交
1. **先阅读**：[TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md) — 约 5 分钟
2. **进行代码修改**
3. **更新指南**：使用该清单来确定需要更新哪些测试指南
4. **运行一个示例**，验证每个已更新的指南
5. **提交**：将代码改动与指南更新一起提交

**示例**：你修改了 `execution/main.cpp`：
- 运行：`./run_tests.sh`（验证编译是否通过）
- 更新：`EXECUTION_TEST_GUIDE.md`（如果行为发生了变化）
- 验证：复制一个 curl 示例并运行它
- 提交：`git add execution/main.cpp EXECUTION_TEST_GUIDE.md`

### ...了解测试策略
**阅读**：[TESTING_PHILOSOPHY.md](TESTING_PHILOSOPHY.md) — 约 10 分钟

包含内容：
- 我们为什么采用这种测试方式
- 哪些内容被测试了（哪些没有，以及原因）
- 测试如何按层次组织（单元测试、组件测试、集成测试）
- 何时以及如何维护测试指南

### ...测试某个具体组件

| 组件 | 指南 | 典型测试 |
|-----------|-------|---------------|
| **状态机制（Regime Classification）** | [TEST_GUIDE.md](TEST_GUIDE.md) | 单元测试：`./run_tests.sh` |
| **凯利仓位管理（Kelly Sizing）** | [TEST_GUIDE.md](TEST_GUIDE.md) | 单元测试：`./build/test_kelly_sizing` |
| **MCPT** | [TEST_GUIDE.md](TEST_GUIDE.md) | 单元测试：`./build/test_mcpt_example` |
| **分析端（Analyst，数据抓取、信号生成）** | [ANALYST_TEST_GUIDE.md](ANALYST_TEST_GUIDE.md) | 手动测试：编译、运行、检查 Telegram |
| **执行端（Execution，订单校验、路由）** | [EXECUTION_TEST_GUIDE.md](EXECUTION_TEST_GUIDE.md) | 手动测试：curl 调用 HTTP 接口 |
| **回测引擎（Backtest Engine）** | [BACKTEST_TEST_GUIDE.md](BACKTEST_TEST_GUIDE.md) | 手动测试：使用历史数据运行 |
| **数据引擎（新闻、宏观数据）** | [DATA_ENGINE_TEST_GUIDE.md](DATA_ENGINE_TEST_GUIDE.md) | 手动测试：Docker Compose + curl |
| **心跳监控（Heartbeat Monitor）** | [HEARTBEAT_TEST_GUIDE.md](HEARTBEAT_TEST_GUIDE.md) | 手动测试：启动服务，检查日志 |

### ...更新测试文档
**阅读**：[TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md) — 约 15 分钟

包含内容：
- 何时应该更新指南
- 如何正确地更新指南
- 如何废弃过时的测试
- 如何防止指南内容陈旧过期

### ...准备发布
**清单**：
1. 运行 `./run_tests.sh` ✅
2. 对于每个发生变更的组件，手动运行其指南中"部署前测试清单"部分的测试
3. 审查本次发布中修改过的所有指南，确认内容准确
4. 通过 Docker Compose 运行一次完整的集成测试

### ...为 Nox 添加新组件
1. 创建 `COMPONENT_TEST_GUIDE.md`（可参考现有指南作为模板）
2. 包含内容：构建方式、单元测试、组件测试、集成测试、环境变量、常见故障、检查清单
3. 将其添加到 [TEST_GUIDE.md](TEST_GUIDE.md) 的导航表中
4. 更新 [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md)
5. 更新 [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md)

---

## 📋 测试指南索引

### 核心单元测试
- **[TEST_GUIDE.md](TEST_GUIDE.md)** — RegimeStateMachine（状态机）、Kelly 仓位管理、MCPT
  - 快速开始：`./run_tests.sh`
  - 耗时：不到 1 秒

### 组件测试（手动）
- **[ANALYST_TEST_GUIDE.md](ANALYST_TEST_GUIDE.md)** — 数据抓取、状态分类、信号传输
  - 耗时：每次测试 5-30 秒
- **[EXECUTION_TEST_GUIDE.md](EXECUTION_TEST_GUIDE.md)** — 订单校验、仓位规模计算、路由
  - 耗时：每次测试 5-30 秒
- **[BACKTEST_TEST_GUIDE.md](BACKTEST_TEST_GUIDE.md)** — 历史模拟、参数调优
  - 耗时：每次测试 10-60 秒
- **[DATA_ENGINE_TEST_GUIDE.md](DATA_ENGINE_TEST_GUIDE.md)** — 新闻/宏观数据服务
  - 耗时：每次测试 5-30 秒
- **[HEARTBEAT_TEST_GUIDE.md](HEARTBEAT_TEST_GUIDE.md)** — 监控、智能分析、告警
  - 耗时：每次测试 5-30 秒

### 运维类指南
- **[TESTING_PHILOSOPHY.md](TESTING_PHILOSOPHY.md)** — 测试策略、原则、权衡取舍
- **[TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md)** — 何时/如何更新指南
- **[TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md)** — 提交前的速查清单

---

## 🔄 典型工作流程

### 添加新功能
```bash
# 1. 编写代码
vi analyst/main.cpp

# 2. 检查测试是否通过
./run_tests.sh

# 3. 确定受影响的指南
# 使用 TEST_UPDATE_CHECKLIST.md

# 4. 更新指南
vi ANALYST_TEST_GUIDE.md

# 5. 测试你新增的示例
bash -x example_command

# 6. 提交
git add analyst/main.cpp ANALYST_TEST_GUIDE.md
git commit -m "feat: Add feature"
```

### 修复缺陷
```bash
# 1. 确定根本原因
# 阅读相关指南，了解预期行为

# 2. 修复代码
vi execution/main.cpp

# 3. 使用指南中的测试验证修复效果
bash -x test_example_from_guide

# 4. 如果行为发生变化，更新指南
vi EXECUTION_TEST_GUIDE.md

# 5. 提交
git add execution/main.cpp EXECUTION_TEST_GUIDE.md
git commit -m "fix: Description of bug"
```

### 发布前运行测试
```bash
# 单元测试（必须执行）
./run_tests.sh

# 组件测试（手动，按指南执行）
./build/backtester ./data/spy_vix_daily.csv
curl http://localhost:8080/webhook ...
python3 heartbeat/monitor.py

# 完整集成测试（如果时间允许）
docker-compose up -d
# ... 端到端验证整个流水线
docker-compose down
```

---

## ⚡ 常用命令

```bash
# 运行所有单元测试
./run_tests.sh

# 运行回测
./build/backtester ./data/spy_vix_daily.csv

# 测试执行端 webhook
curl -X POST http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{"regime":"RISK_ON","vix":18.5,"secret_key":"YOUR_SECRET"}'

# 检查 Telegram 告警
# （测试运行期间监视你的 Telegram 聊天窗口）

# 使用 Docker Compose 启动完整技术栈
docker-compose up -d
docker logs -f nox_execution-engine

# 验证指南中的示例是否可用
bash -x ANALYST_TEST_GUIDE.md  # 提取并运行其中所有的 bash 代码块
```

---

## 📚 学习路径

### 新贡献者
1. 阅读：[TESTING_PHILOSOPHY.md](TESTING_PHILOSOPHY.md)（理解为什么这样测试）
2. 阅读：[TEST_GUIDE.md](TEST_GUIDE.md)（查看简单示例）
3. 运行：`./run_tests.sh`（验证你的环境）
4. 挑选一个组件指南，手动运行一次测试

### 代码审查者
1. 使用：[TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md) 来核实指南是否已更新
2. 从修改过的指南中复制粘贴一个示例，验证其正确性
3. 检查每份指南末尾的"测试清单"（Testing Checklist）

### 维护者
1. 每月：随机挑选一份指南，运行一次测试
2. 每季度：检查所有指南的"最后验证时间"（Last verified）日期
3. 发现缺陷时：确保该缺陷已被指南中的示例覆盖
4. 新增指南时：以现有指南作为模板

---

## 🎯 原则

1. **测试即可执行文档** — 每个示例复制粘贴后都应该能正常运行
2. **指南与代码同步更新** — 在同一次提交、同一个 PR 中一起完成
3. **杜绝静默失败** — 系统应大声报错；测试要验证这一点
4. **快速反馈** — 单元测试耗时 < 1 秒；手动测试耗时 < 30 秒
5. **保持时效性** — 指南会逐渐过时；每月抽查可防止内容腐坏

---

## 📞 常见问题

- **哪份指南覆盖了组件 X？** → 参见上方的表格
- **我应该什么时候更新指南？** → [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md) 中的"何时更新"（When to Update）
- **我该如何更新指南？** → [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md) 中的"如何更新"（How to Update）
- **提交前我应该测试什么？** → [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md)
- **我们为什么采用这种测试方式？** → [TESTING_PHILOSOPHY.md](TESTING_PHILOSOPHY.md)

---

**一句话总结**：提交前运行 `./run_tests.sh`。代码变更时同步更新测试指南。从每份更新过的指南中复制粘贴一个示例来验证其可用性。就这么简单！
