# Nox 文档总览

本文档说明了仓库中存在哪些文档，以及如何保持它们的时效性。

---

## 📚 现有文档有哪些？

### 1. 面向非技术用户（无需编程知识）
**文件**：[NOX_USER_GUIDE.md](NOX_USER_GUIDE.md)

- 用通俗易懂的语言解释机器人的功能
- 说明每条 Telegram 提醒的含义
- 介绍如何在不写代码的情况下监控机器人
- 常见问题解答
- 简明术语表

**适合阅读人群**：任何使用该机器人但不是开发者的人

**何时更新**：当机器人行为发生变化、出现新的提醒、或阈值发生调整时

---

### 2. 面向开发者：测试策略
**文件**：
- [TESTING_PHILOSOPHY.md](TESTING_PHILOSOPHY.md) — 说明我们为何采用这种测试方式
- [TESTING_README.md](TESTING_README.md) — 快速导航指南
- [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md) — 提交前的快速参考

**目的**：理解测试理念，明确应该更新哪些指南

**何时阅读**：
- 在进行第一次代码修改之前
- 在决定要测试什么内容时

---

### 3. 面向开发者：组件测试
**文件**（每个组件对应一份）：
- [TEST_GUIDE.md](TEST_GUIDE.md) — 核心逻辑（RegimeStateMachine、凯利仓位、MCPT）
- [ANALYST_TEST_GUIDE.md](ANALYST_TEST_GUIDE.md) — 数据接入与信号
- [EXECUTION_TEST_GUIDE.md](EXECUTION_TEST_GUIDE.md) — 订单校验与路由
- [BACKTEST_TEST_GUIDE.md](BACKTEST_TEST_GUIDE.md) — 历史回测
- [DATA_ENGINE_TEST_GUIDE.md](DATA_ENGINE_TEST_GUIDE.md) — 数据服务
- [HEARTBEAT_TEST_GUIDE.md](HEARTBEAT_TEST_GUIDE.md) — 监控与告警

**目的**：了解如何测试每个组件

**何时阅读**：在处理相应组件时

**何时更新**：当该组件的代码发生变化时

---

### 4. 面向开发者：维护
**文件**：[TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md)

**目的**：
- 了解何时应更新指南
- 理解如何保持指南的准确性
- 防止指南内容过时

**何时阅读**：在更新指南时，或进行季度审查时

---

## 🔄 更新规则

### 核心规则
**每一次代码变更都必须在同一次提交中附带相应的文档更新。**

### 代码变更 → 应更新哪些文档

| 如果你修改了... | 更新... | 原因... |
|------------------|-----------|-----------|
| VIX/SMA 阈值 | NOX_USER_GUIDE.md + ANALYST_TEST_GUIDE.md + TEST_GUIDE.md | 会影响机器人行为和提醒 |
| 凯利上限（10%）或下限（1%） | EXECUTION_TEST_GUIDE.md + NOX_USER_GUIDE.md | 会影响仓位规模和风险 |
| 市场状态（regime）分类逻辑 | ANALYST_TEST_GUIDE.md + TEST_GUIDE.md + NOX_USER_GUIDE.md | 会改变市场状态的判定方式 |
| HTTP 接口 | EXECUTION_TEST_GUIDE.md + DATA_ENGINE_TEST_GUIDE.md | 会改变 API 交互的测试方式 |
| Telegram 提醒类型/格式 | HEARTBEAT_TEST_GUIDE.md + NOX_USER_GUIDE.md | 会影响用户阅读提醒的方式 |
| 凯利公式 | EXECUTION_TEST_GUIDE.md + TEST_GUIDE.md | 会改变仓位大小的计算方式 |
| 交易时段 | NOX_USER_GUIDE.md | 会改变机器人活跃的时间 |
| RSI 阈值 | EXECUTION_TEST_GUIDE.md + NOX_USER_GUIDE.md | 会改变动量过滤器的行为 |
| 环境变量 | 所有相关指南 | 用户/开发者需要知道需要哪些变量 |
| 单元测试逻辑 | TEST_GUIDE.md | 保持单元测试文档的时效性 |
| 回测功能 | BACKTEST_TEST_GUIDE.md | 会改变回测的使用方式 |

### 更新检查清单
提交前，请使用 [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md)：

```bash
# 1. 确定受影响的文件
# 2. 阅读快速参考表
# 3. 更新每一份受影响的指南
# 4. 复制粘贴一个示例并验证其可用性
# 5. 将代码与文档一起提交
```

---

## 📋 按受众划分的文档

### 非技术用户
- **从这里开始**：[NOX_USER_GUIDE.md](NOX_USER_GUIDE.md)
- **然后查看**：[TESTING_README.md](TESTING_README.md) 了解各组件状态

### 新加入的开发者
1. 阅读：[TESTING_PHILOSOPHY.md](TESTING_PHILOSOPHY.md)
2. 阅读：[TEST_GUIDE.md](TEST_GUIDE.md)
3. 运行：`./run_tests.sh`
4. 选择一个组件指南并运行其中一个测试

### 有经验的开发者
1. 提交前检查：[TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md)
2. 更新：相关的组件指南
3. 验证：复制粘贴的示例可正常运行

### 代码审查者
1. 使用：[TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md) 确认文档已同步更新
2. 检查：示例是否依然有效
3. 通过：当代码与文档都正确无误时

### 系统维护者
1. 每月阅读一次：[TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md)
2. 抽查：随机选一份指南中的一个示例
3. 更新：任何与当前实际行为不符的文档
4. 添加时间戳："Last verified: YYYY-MM-DD"

---

## 🔍 如何保持文档时效性

### 每月维护（30 分钟）
```bash
# 随机选取一份指南
GUIDE=$(ls *_TEST_GUIDE.md | shuf | head -1)

# 复制其中一个测试示例并运行
bash -x $GUIDE

# 如果测试通过，更新时间戳
sed -i "s/Last verified:.*/Last verified: $(date +%Y-%m-%d)/" $GUIDE
```

### 季度审查（1 小时）
```bash
# 检查所有指南中 "Last verified" 时间戳是否超过 90 天
find . -name "*_TEST_GUIDE.md" -o -name "*_USER_GUIDE.md" | \
  xargs grep -l "Last verified"

# 从每份过旧的指南中至少运行一个测试
# 更新时间戳
```

### 发现 Bug 时（立即处理）
1. **在代码中修复** 该 Bug
2. **在组件指南中添加测试用例**，展示该 Bug 已被修复
3. **验证** 该测试示例可正常运行
4. **提交**，提交信息格式为："Fix: 描述该 Bug + 添加测试用例"

### 代码变更时（提交前）
1. **使用** [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md) **确定受影响的指南**
2. **更新每份指南**，反映新的行为
3. **验证示例** 依然可用
4. **将代码与文档一起提交**

---

## ⚠️ 警示信号（说明指南已过时）

- ❌ 示例命令无法执行
- ❌ 预期输出与实际输出不一致
- ❌ 指南中的阈值与代码中的阈值不一致
- ❌ 指南中提到的环境变量在代码中并不存在
- ❌ 指南中提到的 HTTP 接口在代码中并不存在
- ❌ 指南中的提醒消息与实际消息不一致
- ❌ "Last verified" 时间戳超过 90 天

**如果发现任何警示信号**：请立即更新该指南，不要跳过。

---

## 📊 文档质量检查清单

在认定一份指南"已完成"之前，请确认：

### 所有指南通用
- [ ] 示例可以正常编译/运行，没有错误
- [ ] 预期输出与实际行为一致
- [ ] 所有命令均可直接复制粘贴使用
- [ ] 对代码的引用（行号、文件路径）是最新的
- [ ] 时间戳是最近的（少于 90 天）

### 组件指南
- [ ] "环境变量"部分是最新的
- [ ] "常见故障"表格覆盖了实际可能出现的故障
- [ ] "部署前测试检查清单"完整
- [ ] 所有 HTTP 接口/参数均正确

### 用户指南
- [ ] 提醒示例与实际的 Telegram 格式一致
- [ ] 阈值（VIX、SMA、RSI）与代码一致
- [ ] 风险限额（10%、1%）与代码一致
- [ ] 交易时段与代码一致
- [ ] FAQ 中的回答准确无误

### 理念/维护类指南
- [ ] 示例内容切合实际
- [ ] 文件路径正确
- [ ] 工作流描述贴近实际情况

---

## 🚀 典型的文档维护流程

### 场景 1：你修改了 VIX 阈值
```
代码变更：VIX 阈值 从 35 → 40

需要更新的文档：
1. ANALYST_TEST_GUIDE.md — 更新市场状态分类测试，使用 40
2. EXECUTION_TEST_GUIDE.md — 更新市场状态门控测试，使用 40
3. NOX_USER_GUIDE.md — 更新 "VIX" 术语条目以及"Nox 关注哪些指标"部分
4. TEST_GUIDE.md — 如果 test_regime 有变化，更新预期输出

对每份指南：
- 找出所有提到 "35" 的地方
- 替换为 "40"
- 运行一个测试示例
- 验证输出结果
- 将它们一起提交
```

### 场景 2：你新增了一种提醒类型
```
代码变更：新增提醒 "EQUILIBRIUM_DETECTED"

需要更新的文档：
1. HEARTBEAT_TEST_GUIDE.md — 为新提醒添加测试
2. NOX_USER_GUIDE.md — 在"理解提醒"部分新增一节
3. TEST_UPDATE_CHECKLIST.md — 注明用户指南会受心跳（heartbeat）变更影响

对每份指南：
- 添加新提醒的说明
- 展示示例 Telegram 消息
- 说明用户应该采取的操作
- 完成验证流程
- 将它们一起提交
```

### 场景 3：代码重构（不改变行为）
```
代码变更：提取辅助函数、重命名内部变量

需要更新的文档：
- 无需更新（属于内部重构）
- 除非：对外可见的行为发生了变化
- 除非：示例代码路径发生了变化

规则：如果开发者感知不到这个变化，文档就不需要更新。
```

---

## 📞 常见疑问

- **我该更新哪份指南？** → [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md)
- **我该如何更新？** → [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md)
- **为什么我们有这么多文档？** → [TESTING_PHILOSOPHY.md](TESTING_PHILOSOPHY.md)
- **我发现文档有错误** → 立即修复，并在提交信息中说明原因
- **某份指南已经过时** → 按照上面的"每月维护"流程进行更新

---

## 总结

**每一次代码变更都应该有相应的文档变更与之对应。** 这样可以保持两者同步，避免产生混淆。

在每次提交之前，使用 [TEST_UPDATE_CHECKLIST.md](TEST_UPDATE_CHECKLIST.md) 来确定哪些文档需要更新。对每份更新过的指南运行一个示例以验证其正确性。将代码与文档一起提交。

每月的抽查可以让指南保持最新状态。如果某个指南示例无法运行，请立即更新它。

---

### 5. 模拟交易与实盘迁移指南
**文件**：[PAPER_TRADING_READINESS.md](PAPER_TRADING_READINESS.md)

- 对当前哪些功能已可用、哪些需要在投入实盘资金前修复，给出如实的评估
- 指出投入实盘前的 3 个关键缺口（PositionManager 接线、重启后同步、财报过滤器）
- 分步说明实盘迁移流程（激活 IBKR 或切换至 Alpaca）
- 提供为期 60 天的模拟交易检查清单，附有明确的上线/不上线判断标准
- 现实的预期收益优势部分，涵盖方差风险溢价、方向性偏差，以及 WS1–WS3 带来的提升

**适合阅读人群**：任何需要决定是否投入实盘资金的人

**何时更新**：在完成 60 天模拟交易周期后；用实际统计数据更新第 2 节

---

**Last Updated: 2026-06-29**
