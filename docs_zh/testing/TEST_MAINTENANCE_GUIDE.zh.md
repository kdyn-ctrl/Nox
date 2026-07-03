# 测试维护指南

本指南说明在 Nox 系统中，何时以及如何让测试文档与代码改动保持同步。

## 概述

以下组件各自配有对应的测试指南：
- `TEST_GUIDE.md` — 核心 C++ 单元测试（RegimeStateMachine、Kelly 仓位管理、MCPT）
- `ANALYST_TEST_GUIDE.md` — 数据摄取与信号生成
- `EXECUTION_TEST_GUIDE.md` — 订单校验、仓位计算、路由
- `BACKTEST_TEST_GUIDE.md` — 历史回测
- `DATA_ENGINE_TEST_GUIDE.md` — 市场数据服务
- `HEARTBEAT_TEST_GUIDE.md` — 监控与智能分析
- `TEST_MAINTENANCE_GUIDE.md` — 本文件

## 何时需要更新测试指南

### 必须更新的情况：

1. **新增代码路径**
   - 新的校验门（validation gates）
   - 新的接口（REST API）
   - 新的 CLI 参数或标志
   - 新的定时任务

   **操作**：在相关指南中新增测试用例章节，说明如何触发新代码路径。

2. **修改阈值或参数**
   - 市场状态（Regime）分类的 VIX/SMA 边界值
   - Kelly 仓位的上限或乘数
   - 速率限制或超时时间
   - 数据保留策略

   **操作**：更新测试示例中的期望值，以及"常见故障"表格。

3. **修改数据格式**
   - JSON 载荷结构
   - CSV 文件格式
   - HTTP 请求/响应体
   - 数据库结构（schema）

   **操作**：更新受影响指南中的所有代码示例和期望输出。

4. **修改外部 API 集成**
   - Yahoo Finance → 新的数据提供方
   - Alpaca 接口 URL
   - Telegram API 变更
   - Claude API 参数变更

   **操作**：更新集成测试章节，写明新的接口详情和鉴权机制。

5. **新增错误情形**
   - 新的异常类型
   - 新的校验拒绝逻辑
   - 新的超时场景

   **操作**：在"常见故障与诊断"表格中新增一行。

### 可考虑更新的情况：

6. **内部逻辑重构**（不改变行为）
   - 提取辅助函数
   - 重新组织代码结构
   - 变量重命名

   **操作**：只要对外行为未变，无需更新指南。

7. **性能优化**
   - 更快的算法
   - 更好的缓存
   - 连接池

   **操作**：如果延迟目标发生变化，更新"响应时间测试"或性能预期。

8. **环境变量变更**
   - 新增必需变量
   - 变量改为可选
   - 重命名现有变量

   **操作**：更新相关指南中的"所需环境变量"章节。

## 如何更新测试指南

### 第一步：确定受影响的指南
当你修改代码时，先判断哪些测试指南会受到影响：

```
如果你修改了...             需要更新以下指南：
─────────────────────────── ──────────────────────────────
analyst/main.cpp            ANALYST_TEST_GUIDE.md
execution/main.cpp          EXECUTION_TEST_GUIDE.md
tests/*.cpp                 TEST_GUIDE.md
backtest-engine/main.cpp    BACKTEST_TEST_GUIDE.md
*_data_engine/main.py       DATA_ENGINE_TEST_GUIDE.md
heartbeat/monitor.py        HEARTBEAT_TEST_GUIDE.md
shared/RegimeStateMachine   TEST_GUIDE.md 以及所有使用它的指南
```

### 第二步：更新相关章节
对每一份受影响的指南，按以下顺序更新对应章节：

1. **代码示例** — 如果命令行、API 调用或编译方式发生变化
2. **期望输出** — 如果结果、日志或控制台输出发生变化
3. **环境变量** — 如果新增变量或要求发生变化
4. **常见故障** — 如果出现新的错误情形
5. **测试清单** — 如果验收标准发生变化

### 第三步：测试你的改动
提交前请先执行：

```bash
# 运行与你的改动对应的测试
./run_tests.sh                              # 针对 C++ 单元测试

# 或手动测试相关组件
python3 heartbeat/monitor.py                # 针对 Python 服务

# 验证指南中的示例仍然可用
bash -x <(grep "^g++" ANALYST_TEST_GUIDE.md)  # 提取并执行编译命令
```

### 第四步：代码与文档一起提交
**规则**：影响行为的代码改动，必须在同一次提交中包含指南更新。

```bash
git add src/mychange.cpp TEST_GUIDE.md
git commit -m "feat: Add new validation gate

- Implements stricter portfolio check in execution
- Rejects orders when cash < 10% of portfolio

TEST_GUIDE.md: Updated 'Common Failures' table with new error case"
```

## 各指南逐一检查清单

更新每份指南时，请使用以下检查清单：

### NOX_USER_GUIDE.md（非技术用户指南）
- [ ] 提醒说明与实际 Telegram 消息格式一致
- [ ] 市场状态描述与 RegimeStateMachine 规则一致
- [ ] 风险限额（10% 上限、1% 最小值）为最新值
- [ ] 交易时段正确
- [ ] 绩效指标（夏普比率、胜率）解释准确
- [ ] FAQ 答案仍然有效

**何时更新**：
- 当提醒类型或格式发生变化时
- 当市场状态阈值发生变化时（VIX 35、SPY/SMA 比率等）
- 当风险限额发生变化时（10% 上限、Kelly 公式）
- 当交易时段发生变化时
- 当新增功能时（新接口、新的监控选项）

### TEST_GUIDE.md（核心 C++ 单元测试）
- [ ] 测试命令示例可正常编译
- [ ] 期望输出与实际测试运行结果一致
- [ ] 状态表中列出了所有通过的测试
- [ ] 每个测试的功能说明准确

### ANALYST_TEST_GUIDE.md
- [ ] 数据抓取 URL 仍然有效（Yahoo Finance 接口）
- [ ] 市场状态阈值与 RegimeStateMachine.hpp 一致
- [ ] 期望的 JSON 载荷结构正确
- [ ] Telegram 通知格式为最新

### EXECUTION_TEST_GUIDE.md
- [ ] HTTP 接口路径与 execution/main.cpp 一致
- [ ] 鉴权请求头名称与格式正确
- [ ] Kelly 公式变量及上限为最新
- [ ] Alpaca 订单载荷结构准确
- [ ] 校验门（gate）顺序描述正确

### BACKTEST_TEST_GUIDE.md
- [ ] CSV 列名与 csv_parser.hpp 一致
- [ ] 命令行参数与 main.cpp 一致
- [ ] 参数范围合理
- [ ] 示例输出指标与代码一致

### DATA_ENGINE_TEST_GUIDE.md
- [ ] REST 接口路径与 FastAPI 路由一致
- [ ] 鉴权请求头校验与代码一致
- [ ] 响应 JSON 结构与 scrapers.py 一致
- [ ] 速率限制反映当前配置

### HEARTBEAT_TEST_GUIDE.md
- [ ] SQLite 结构与数据库初始化一致
- [ ] Telegram 消息格式与代码一致
- [ ] 定时任务间隔正确
- [ ] Claude 集成提示词为最新

### TEST_MAINTENANCE_GUIDE.md（本文件）
- [ ] 如出现新分类，更新"何时更新"章节
- [ ] 新增组件时补充相应指南
- [ ] 若出现新的常见模式，予以记录

## 自动化测试指南校验

### Pre-Commit 钩子（可选）
在 `.git/hooks/pre-commit` 中添加以下内容，以捕获明显的指南遗漏：

```bash
#!/bin/bash
# 校验代码改动是否有对应的指南更新

STAGED_CODE=$(git diff --cached --name-only | grep -E '\.(cpp|py|hpp)$')
STAGED_DOCS=$(git diff --cached --name-only | grep -E '_TEST_GUIDE\.md$')

if [ -n "$STAGED_CODE" ] && [ -z "$STAGED_DOCS" ]; then
  echo "⚠️  代码已修改，但测试指南未更新"
  echo "已修改文件: $STAGED_CODE"
  echo "请更新对应的测试指南"
  exit 1
fi
exit 0
```

### 手动校验
编辑完某份指南后，验证其中的示例：

```bash
# 提取代码块并运行
python3 << 'EOF'
import re
guide_file = "ANALYST_TEST_GUIDE.md"
with open(guide_file) as f:
    content = f.read()
    
# 查找所有 bash 代码块
blocks = re.findall(r'```bash\n(.*?)\n```', content, re.DOTALL)
print(f"Found {len(blocks)} bash code blocks in {guide_file}")

# 检查示例是否失效
for i, block in enumerate(blocks):
    # 提取命令（排除变量赋值）
    for line in block.split('\n'):
        if line.startswith('curl ') or line.startswith('./'):
            print(f"Block {i}: {line[:60]}...")
EOF
```

## 常见的指南更新误区

### ❌ 不要：
1. 在指南中加入尚未编码实现的"理想化"改动
2. 为"未来"尚未落地的功能编写指南
3. 删除仍然有效的测试用例
4. 未经代码验证就修改期望输出
5. 把代码改动和指南更新拆分成不同的提交（这样会丢失上下文）

### ✅ 应该：
1. 更新示例后运行测试，确认它们仍然通过
2. 将指南更新与代码改动放在同一次提交中
3. 将已废弃的测试章节标记为"[DEPRECATED — 请改用 X]"
4. 当一个测试依赖另一个测试时，在指南之间做好交叉引用
5. 即使测试暂时失败，也要提交指南更新（标记为 [TODO]）

## 废弃测试用例

当某个组件变化很大，导致旧的测试不再适用时：

```markdown
### [DEPRECATED] 旧测试名称
该测试不再有效，原因是 [原因]。
→ 请改用 [新测试名称]（见下方章节）。

### 新测试名称
[更新后的测试步骤]
```

## 代码评审者的评审清单

评审涉及可测试代码的 PR 时，请检查：

- [ ] 代码改动附带对应的指南更新
- [ ] 指南示例语法正确
- [ ] 期望输出与代码行为一致
- [ ] 新增的错误情形已有文档记录
- [ ] 如有需要，环境变量章节已更新
- [ ] "何时更新"章节中提及了此类改动

## 指南之间的相互引用

测试指南之间会相互引用。当你重命名或合并某份指南时：

```bash
# 查找所有交叉引用
grep -r "ANALYST_TEST_GUIDE\|EXECUTION_TEST_GUIDE" *.md

# 统一更新
sed -i 's/ANALYST_TEST_GUIDE/NEW_GUIDE_NAME/g' *.md
```

## 防止指南过时（Guide Rot）

随着代码不断变化，测试指南会逐渐老化失效。为保持其时效性：

1. **每月抽查**：随机挑选一份指南，运行其中一个测试，确认仍然有效
2. **发布前检查**：运行所有指南中的示例，捕捉潜在的老化问题
3. **CI 集成**：考虑将指南校验纳入 CI 流水线
4. **时间戳更新**：在指南中添加"最后验证时间：YYYY-MM-DD"注释

示例：
```markdown
### Kelly 仓位计算测试
最后验证时间：2026-06-22

```bash
g++ -std=c++17 -pthread -o build/test_kelly tests/test_kelly_sizing.cpp
```

该测试最后一次运行并验证的时间为 2026-06-22。
```

## 小结

**核心规则**：每一次可测试的代码改动，都必须在同一次提交中包含相应的指南更新。定期运行示例，保持其时效性。

这些指南并非摆设——它们是整个系统用于验证正确性的知识库。请以对待代码本身同等的态度去维护它们。
