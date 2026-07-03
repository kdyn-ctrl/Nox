# Nox 测试理念

本文档阐述了 Nox 交易系统的整体测试策略。

## 核心原则

### 1. **重行为、轻实现**
测试验证的是*系统做了什么*，而不是*它是如何做到的*。我们关心 Kelly 计算器是否遵守其 10% 的上限，而不关心它用的是浮点数还是定点数运算。

### 2. **真实数据验证**
只要条件允许，单元测试就使用真实数据（历史行情数据、真实 API 响应）来尽早发现集成层面的问题。在执行测试中我们不会 mock Alpaca API，而是直接对接模拟盘（paper trading）进行测试。

### 3. **以关卡（Gate）为中心的测试**
系统围绕一系列校验关卡组织：
- **Analyst（分析端）**：数据获取关卡、行情状态（regime）分类关卡、传输关卡
- **Execution（执行端）**：模式（schema）关卡、鉴权关卡、动量过滤关卡、regime 关卡、组合（portfolio）关卡、Kelly 仓位关卡、订单路由关卡

每个关卡都有明确的测试覆盖。层层递进的关卡可以防止系统进入非法状态。

### 4. **快速反馈**
单元测试（RegimeStateMachine、Kelly 仓位计算、CSV 解析）运行时间 < 1 秒，只验证孤立的逻辑。集成测试则是手动执行，或需要显式的 Docker 环境搭建，另行文档说明。

### 5. **文档即测试**
测试指南本身就是可执行的文档。指南中每一个 `bash` 代码块，复制粘贴后都应当能正常运行；每一个 `curl` 命令都展示真实的 HTTP 交互。指南如果不定期运行，就会逐渐失效（decay）。

### 6. **宁可报错，绝不沉默**
配置有误的系统会立即停机。缺少环境变量 → `[FATAL]` 退出；API 响应非法 → `[CRITICAL]` 告警 + Telegram 通知 + 停机。我们不做优雅降级——而是明确地失败，让问题一目了然。

---

## 分层测试策略

### 第 1 层：单元测试（最快）
**目的**：孤立验证独立逻辑。

**内容**：
- RegimeStateMachine 分类
- Kelly 仓位计算
- MCPT 统计工具
- CSV 解析

**方式**：编译并运行独立可执行文件。

**耗时**：每个测试 < 1 秒

**时机**：每次修改这些模块的代码时。提交前运行 `./run_tests.sh`。

**示例**：
```bash
./build/test_regime
./build/test_kelly_sizing
./build/test_mcpt_example
```

### 第 2 层：组件测试（中等）
**目的**：单独验证某个服务的行为（或针对被 mock 的对等服务）。

**内容**：
- Analyst 数据获取（对接真实 Yahoo Finance）
- Execution 订单校验（通过 HTTP curl）
- Data engine 接口（通过 HTTP curl）
- Heartbeat 与 Claude 的集成（通过 API）
- Backtest 回测模拟（对接历史 CSV 数据）

**方式**：启动服务，通过 HTTP/CLI 交互，验证输出。全部记录在各组件的指南文档中。

**耗时**：每个测试 5-30 秒

**时机**：修改某个组件之后。手动运行，或通过 CI 中的集成测试运行。

**示例**：
```bash
./build/backtester ./data/spy_vix_daily.csv
curl -X POST http://localhost:8080/webhook -d '{...}'
python3 heartbeat/monitor.py  # 启动并检查日志
```

### 第 3 层：集成测试（最慢）
**目的**：验证各组件端到端协同工作。

**内容**：
- Analyst → Execution → Alpaca 全链路
- Analyst → Heartbeat → Telegram 通知链路
- 回测结果与实盘 regime 分类结果对照

**方式**：
- 使用 Docker Compose 启动多个容器
- 通过整条链路发送信号
- 验证最终输出（Alpaca 中的订单、Telegram 中的消息）

**耗时**：每个测试 30 秒到 5 分钟

**时机**：发布前或重大重构前。记录在各组件指南中。

**示例**：
```bash
docker-compose up -d
curl http://localhost:8080/health  # 检查 analyst 是否在运行
# ... 通过全链路发送信号
docker logs nox_execution-engine | grep "Order submitted"
```

---

## 测试覆盖地图

| 组件 | 单元测试 | 组件测试 | 集成测试 | 手动测试 |
|-----------|------|-----------|-------------|--------|
| **RegimeStateMachine** | ✅ test_regime.cpp | ✅ TEST_GUIDE.md | ✅ 全部流水线 | - |
| **Kelly Sizing** | ✅ test_kelly.cpp | ✅ EXECUTION_TEST_GUIDE.md | ✅ 订单测试 | - |
| **MCPT** | ✅ mcpt_example.cpp | ✅ TEST_GUIDE.md | - | - |
| **Analyst** | - | ✅ ANALYST_TEST_GUIDE.md | ✅ 完整流水线 | 交易时段 |
| **Execution** | - | ✅ EXECUTION_TEST_GUIDE.md | ✅ 完整流水线 | 交易时段 |
| **Backtest** | ✅ csv_parser | ✅ BACKTEST_TEST_GUIDE.md | - | 优化调参 |
| **Data Engine** | - | ✅ DATA_ENGINE_TEST_GUIDE.md | Docker Compose | 手动 |
| **Heartbeat** | - | ✅ HEARTBEAT_TEST_GUIDE.md | Docker Compose | 手动 |

### 已知空白（有意为之）
- **不 mock 外部 API**：我们对接真实（模拟盘）API，以便发现集成层面的问题
- **不做模糊测试（fuzzing）**：系统不接受不可信输入；校验是基于关卡（gate）实现的
- **不做负载测试**：系统是单信号处理，非高吞吐场景；延迟要求较为宽松

---

## 测试执行流程

### 提交代码前
```bash
# 1. 运行单元测试（< 1 秒）
./run_tests.sh

# 2. 确定需要更新哪些指南
# 参考：TEST_UPDATE_CHECKLIST.md

# 3. 更新指南并抽查一个示例
vi ANALYST_TEST_GUIDE.md
# （复制一个 bash 示例并运行）

# 4. 提交时一并附带指南更新
git add src/mychange.cpp ANALYST_TEST_GUIDE.md
git commit -m "feat: Add feature X (with test guide update)"
```

### 部署前
```bash
# 1. 运行全部单元测试
./run_tests.sh

# 2. 运行组件测试（手动执行，参照 TEST_GUIDE.md 等指南）
./build/backtester ./data/spy_vix_daily.csv

# 3. 通过 Docker Compose 测试完整流水线
docker-compose up -d
curl http://execution:8080/health  # 验证所有服务已启动
# ... 发送测试信号
docker logs nox_execution-engine | grep success

# 4. 按各指南中的"部署前检查清单"完成检查
# （参见各指南中的"Testing Checklist Before Deployment"章节）
```

### 每月例行维护
```bash
# 1. 随机挑选一份指南
# 2. 从该指南中运行一个测试
# 3. 核对输出是否符合预期
# 4. 如果失败，更新指南或提交 bug
# 5. 在指南中添加时间戳："Last verified: YYYY-MM-DD"
```

---

## 测试指南的维护

**核心原则**：指南即代码，应当以同等的严谨态度对待。

### 何时需要更新指南
- ✅ 任何影响行为的代码改动
- ✅ 任何新增的参数、阈值或环境变量
- ✅ 任何新增的错误情形
- ✅ 任何 HTTP 接口或 API 格式的变更
- ❌ 不改变行为的内部重构
- ❌ 提取接口不变的辅助函数

### 指南更新的质量检查
1. **示例可直接复制运行**：`bash` 代码块粘贴后应能编译/运行
2. **输出为最新**：示例输出与代码实际产生的结果一致
3. **覆盖完整**：每个公开 API/参数都有文档
4. **诊断覆盖全面**："常见故障"表格应覆盖现实中可能出现的失败场景

### 应对指南失效（decay）
指南会随时间逐渐失效。应对方法：
1. 每月为每份指南运行一个示例
2. 为示例打上时间戳：`Last verified: 2026-06-22`
3. 一旦出现故障，立即更新对应指南
4. 为外部 API 地址（例如 Yahoo Finance 接口）钉住具体版本号

---

## 理念与实践对照

| 理念 | 落地方式 |
|-----------|-------------------|
| 测试行为，而非实现 | 单元测试针对已知输入验证输出，而非代码路径 |
| 尽可能使用真实数据 | 集成测试对接真实 API（Alpaca 使用模拟盘） |
| 宁可报错，绝不沉默 | `[FATAL]`/`[CRITICAL]` 日志触发告警并停机 |
| 快速反馈 | 单元测试 < 1 秒，组件测试 < 30 秒 |
| 测试即文档 | 指南中包含真实可运行的示例 |
| 指南保持最新 | "Last verified" 时间戳、每月抽查 |

---

## 测试取舍

### 我们全面测试的部分
- 核心逻辑（regime、Kelly），覆盖多种场景
- 全部关卡（schema、鉴权、动量、regime、组合）
- 外部集成（Alpaca、Yahoo Finance、Telegram）
- 错误处理（超时、非法输入、数据缺失）

### 我们不测试的部分（可接受的风险）
- **Mock 的 API**：与真实情况相差太远，价值不大
- **负载测试**：系统一次只处理一个信号
- **模糊测试（Fuzzing）**：系统有严格的输入 schema 关卡
- **向后兼容性**：系统由一人维护，破坏性变更只要有文档说明即可接受

---

## 扩展测试策略

### 如果你新增了一个组件
1. 新建一份指南：`COMPONENT_TEST_GUIDE.md`
2. 包含以下章节：Building（构建）、Unit Tests（单元测试）、Component Tests（组件测试）、Integration Tests（集成测试）、Environment Variables（环境变量）、When to Update（何时更新）、Common Failures（常见故障）、Checklist（检查清单）
3. 在 [TEST_GUIDE.md](TEST_GUIDE.md) 顶层表格中添加条目
4. 在 [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md) 中添加链接
5. 更新 [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md) 中的"何时更新"章节

### 如果你同时修改了多个组件
在每份相关指南中记录跨组件的影响。例如：

```markdown
### Integration Point: Analyst → Execution
If you modify analyst/main.cpp to add a new field to the JSON payload:
1. Update ANALYST_TEST_GUIDE.md "Payload Validation Test"
2. Update EXECUTION_TEST_GUIDE.md "Schema Validation Test" to accept new field
3. Update EXECUTION_TEST_GUIDE.md "Common Failures" if parsing could fail
```

---

## 小结

- **单元测试**验证孤立逻辑（< 1 秒）
- **组件测试**验证单个服务（手动，< 30 秒）
- **集成测试**验证端到端流程（手动，Docker，5 分钟）
- **指南即测试**：示例复制后应可直接运行
- **随代码更新指南**：同一次提交、同一个 PR
- **防止失效**：每月抽查、打时间戳

这种做法是用广度（并非所有内容都自动化测试）换取深度（凡是测试到的内容，都有详尽文档并定期验证）。

---

## 快速链接

- [Test Guide (Unit Tests)](TEST_GUIDE.md)
- [Test Update Checklist](TEST_UPDATE_CHECKLIST.md)
- [Test Maintenance Guide](TEST_MAINTENANCE_GUIDE.md)
- [Component Guides](TEST_GUIDE.md#-all-test-guides)
