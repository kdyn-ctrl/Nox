# 测试指南更新检查清单

**在提交代码更改之前使用本文档。**

## 快速参考：应该更新哪个指南？

```
修改的文件                           需要更新的指南
────────────────────────────────────────────────────────────
analyst/main.cpp                    → ANALYST_TEST_GUIDE.md + NOX_USER_GUIDE.md
analyst/RegimeStateMachine.hpp      → ANALYST_TEST_GUIDE.md + TEST_GUIDE.md + NOX_USER_GUIDE.md

execution/main.cpp                  → EXECUTION_TEST_GUIDE.md + NOX_USER_GUIDE.md
execution/RegimeStateMachine.hpp    → EXECUTION_TEST_GUIDE.md + TEST_GUIDE.md + NOX_USER_GUIDE.md

backtest-engine/main.cpp            → BACKTEST_TEST_GUIDE.md
backtest-engine/csv_parser.hpp      → BACKTEST_TEST_GUIDE.md

*_data_engine/main.py               → DATA_ENGINE_TEST_GUIDE.md
*_data_engine/scrapers.py           → DATA_ENGINE_TEST_GUIDE.md

heartbeat/monitor.py                → HEARTBEAT_TEST_GUIDE.md + NOX_USER_GUIDE.md

tests/test_*.cpp                    → TEST_GUIDE.md

shared/RegimeStateMachine.hpp       → TEST_GUIDE.md + 所有使用该文件的指南 + NOX_USER_GUIDE.md
```

**注意**：如果你的更改涉及**告警、阈值、风控限制或交易行为**，也要同步更新 `NOX_USER_GUIDE.md`，以便非技术用户能理解新的行为。

## 提交前检查清单

在运行 `git commit` 之前：

- [ ] **代码可以编译**，且没有警告/错误
  ```bash
  ./run_tests.sh  # 用于 C++ 更改
  python3 -m py_compile heartbeat/monitor.py  # 用于 Python 更改
  ```

- [ ] **确定受影响的指南**（参考上面的对照表）

- [ ] **对每个受影响的指南**，逐一核对：
  - [ ] 代码示例的语法仍然正确
  - [ ] 预期输出与实际代码行为一致
  - [ ] 任何变更过的阈值/参数都已更新
  - [ ] 任何新增的错误情形都已写明
  - [ ] 环境变量章节内容为最新

- [ ] **环境变量**：如果你新增/修改/删除了环境变量：
  - [ ] 更新"所需环境变量"章节
  - [ ] 标注为 `REQUIRED`（必填）或 `OPTIONAL`（可选）
  - [ ] 如有默认值，需注明

- [ ] **API 变更**：如果你修改了 HTTP 路由、参数或响应格式：
  - [ ] 更新指南中的 `curl` 示例
  - [ ] 更新预期的响应 JSON 结构
  - [ ] 如状态码有变化，一并更新

- [ ] **测试这一具体改动**：
  ```bash
  # 示例：修改了 analyst 的数据抓取逻辑
  g++ -std=c++17 -o build/analyst analyst/main.cpp
  ./build/analyst  # 运行并核实输出

  # 然后查阅 ANALYST_TEST_GUIDE.md 中的 "Live Data Fetch Test" 章节
  # 确保它与你刚才运行的结果一致
  ```

## 变更类型 → 指南更新对照

### 🔧 新增功能 / 新代码路径
- [ ] 新增测试用例章节
- [ ] 记录预期行为
- [ ] 如出现新的错误路径，在"常见故障与诊断"表中新增一行
- [ ] 如验收标准有变化，更新"部署前测试清单"

### 📊 参数 / 阈值变更
- [ ] 更新示例中的"预期"值
- [ ] 更新"常见故障"中的诊断阈值
- [ ] 更新所有包含当前数值的表格

### 🚪 新增/修改/删除 HTTP 接口
- [ ] 更新所有 `curl` 示例（路径、请求头、请求体）
- [ ] 更新预期响应结构
- [ ] 如状态码有变化，一并更新
- [ ] 记录新增/变更的环境变量

### 💾 数据库结构 / 文件格式变更
- [ ] 更新结构（schema）说明
- [ ] 更新示例 CSV/JSON 结构
- [ ] 更新解析/校验逻辑相关的描述

### 🔗 外部 API 集成变更
- [ ] 更新 API 端点 URL
- [ ] 如认证方式有变化，一并更新
- [ ] 更新预期响应格式
- [ ] 在"常见故障"中补充新的错误模式

### ♻️ 代码重构（行为不变）
- [ ] 通常不需要更新指南
- [ ] 除非：对外可见的行为发生了变化

## 提交信息格式

在提交信息中注明测试指南的更新情况：

```
feat: Add streaming support to analyst webhook

- Allows analyst to send multiple signals in batch
- Adds BATCH_SIZE env var (default 10)
- Retries unchanged

ANALYST_TEST_GUIDE.md:
- Updated "Payload Validation Test" to show array example
- Added env var: BATCH_SIZE
- Documented batch chunking behavior
```

## 示例：分步演示

假设你修改了 `execution/main.cpp`，把 Kelly 仓位上限从 10% 调整为 15%：

```bash
# 1. 进行代码修改
vi execution/main.cpp
# 修改：kelly_cap = 0.10  →  kelly_cap = 0.15

# 2. 本地测试
g++ -std=c++17 -o build/execution execution/main.cpp
curl -X POST http://localhost:8080/webhook -d '{...signal...}'
# 核实订单是否按 15% 上限正确计算仓位

# 3. 更新测试指南
vi EXECUTION_TEST_GUIDE.md
# 找到："Kelly Sizing Calculation Test"
# 更新示例："10% → 15%"
# 找到："Common Failures" 表
# 更新仓位上限的引用值为 15%

# 4. 运行测试示例进行验证
g++ -std=c++17 -pthread -o build/test_kelly_sizing tests/test_kelly_sizing.cpp
./build/test_kelly_sizing
# 应仍然通过

# 5. 提交并附上说明
git add execution/main.cpp EXECUTION_TEST_GUIDE.md
git commit -m "feat: Increase Kelly position cap to 15%

Allows more aggressive sizing while maintaining risk bounds.

EXECUTION_TEST_GUIDE.md: Updated cap value in all examples"
```

## 指南更新警示信号 🚩

出现以下情况，说明你很可能忘记更新指南了：

- [ ] 代码中新增了参数，但"环境变量"章节没有变化
- [ ] 你修改了 API 响应格式，但测试指南里仍然是旧的 JSON
- [ ] 你新增了校验逻辑，但"常见故障"表没有更新
- [ ] 你改变了错误处理行为，但示例中的预期输出没有反映这一点
- [ ] 你重命名了某个命令行参数，但指南里仍引用旧名称

## 测试指南示例

更新完指南后，抽查其中一个示例：

```bash
# 示例：测试 ANALYST_TEST_GUIDE.md 中的 "Live Data Fetch Test"
cd /root/Nox
export TELEGRAM_BOT_TOKEN="test"
export TELEGRAM_CHAT_ID="test"

# 复制指南中的测试命令
g++ -std=c++17 -o build/analyst analyst/main.cpp
./build/analyst

# 核实输出是否与指南中的 "Expected:" 章节一致
```

## 常见问题

- **什么时候需要更新？** → 参见 [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md)
- **应该怎么更新？** → 参见 [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md) 中的"如何更新测试指南"
- **哪个指南覆盖组件 X？** → 参见本文件顶部的对照表
- **需要新增一份指南怎么办？** → 参见 [TEST_MAINTENANCE_GUIDE.md](TEST_MAINTENANCE_GUIDE.md)

---

**一句话总结**：每次提交代码时，也要一并提交指南更新。使用上方对照表找出受影响的指南，并从每份指南中运行一个示例来验证其正确性。完成！
