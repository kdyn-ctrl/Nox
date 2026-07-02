# Analyst 模块测试指南

Analyst 模块负责获取市场数据（VIX、SPY）、计算技术指标、对市场状态（regime）进行分类，并将信号传输给执行引擎（execution engine）。

## 被测试的组件

- **数据获取**：从 Yahoo Finance 获取 VIX 和 SPY 价格
- **状态分类**：RegimeStateMachine 评估
- **信号序列化**：生成 JSON 载荷（payload）
- **传输**：向执行引擎 webhook 发送 HTTP POST

## 手动测试

### 1. 独立的状态分类测试
```bash
g++ -std=c++17 -o build/test_regime tests/test_regime.cpp
./build/test_regime
```
**目的**：验证状态分类逻辑，使其独立于数据获取环节。

**预期输出**：
```
✓ RISK_ON: VIX < 35, SPY > 200-SMA
✓ RISK_OFF: VIX >= 35 or SPY < 200-SMA*0.98
✓ TRANSITION: SPY between thresholds
```

### 2. 实时数据获取测试（手动）
```bash
cd /root/Nox
export TELEGRAM_BOT_TOKEN="your_token"
export TELEGRAM_CHAT_ID="your_chat_id"

# 编译 analyst
g++ -std=c++17 -o build/analyst analyst/main.cpp -L. -lcurl -lssl -lcrypto

# 运行一次迭代
./build/analyst
```

**需要检查的内容**：
1. 查看日志中是否出现 `[INFO] Fetching VIX from Yahoo Finance`
2. 验证 SPY 价格是否为最新（当天收盘价）
3. 确认 200 日 SMA 是基于 252+ 个数据点计算得出的
4. 查找 `[INFO] Regime classification: RISK_ON|RISK_OFF|TRANSITION`
5. 确认在传输之前 JSON 载荷已被记录到日志

**预期信号**（在交易时段运行时）：
- 若 VIX < 35 且 SPY > 200-SMA → `RISK_ON`
- 若 VIX >= 35 或 SPY < 200-SMA*0.98 → `RISK_OFF`

### 3. 网络连通性测试
```bash
# 检查执行引擎是否可达
curl -X GET http://execution-engine:8080/health

# 如果使用 Docker Compose：
docker-compose up -d
curl -X GET http://localhost:8080/health
```

**预期结果**：执行引擎返回 HTTP 200 响应。

### 4. 载荷校验测试
```bash
# 从 analyst 日志中提取 JSON 载荷
# 验证其包含以下字段：
{
  "regime": "RISK_ON|RISK_OFF|TRANSITION",
  "vix": <float>,
  "spy_price": <float>,
  "sma_200": <float>,
  "timestamp": "ISO 8601 string"
}
```

## 集成测试

### Analyst → Execution 流水线
```bash
# 终端 1：启动执行引擎
cd /root/Nox && docker-compose up execution-engine

# 终端 2：运行一次 analyst
docker-compose run analyst

# 终端 3：查看执行引擎日志
docker logs nox_execution-engine

# 验证：
# 1. analyst 已向 /webhook 发送 POST 请求
# 2. execution 已接收并记录该信号
# 3. execution 日志中没有 [CRITICAL] 错误
```

### Telegram 通知测试
当 analyst 运行时，检查 Telegram 是否收到状态分类消息：
```
📊 [ANALYST] Regime Classification
Regime: RISK_ON
VIX: 18.5
SPY: 482.50
200-SMA: 475.20
Timestamp: 2026-06-22 14:30:00 UTC
```

如果没有收到消息，请检查：
- 是否已设置 `TELEGRAM_BOT_TOKEN` 和 `TELEGRAM_CHAT_ID`
- 网络连接是否可用
- Token 是否有效（可用 `curl` 测试 Telegram API）

## 所需的环境变量

```bash
TELEGRAM_BOT_TOKEN       # Telegram 机器人 token（可选，缺失时该功能会被禁用）
TELEGRAM_CHAT_ID         # Telegram 会话 ID（可选，缺失时该功能会被禁用）
```

## 何时需要更新本指南

在以下情况下需要更新本指南：
1. **数据获取来源变更时** —— 例如从 Yahoo Finance 迁移到其他数据提供商
2. **状态阈值变更时** —— 记录新的 VIX/SMA 边界值
3. **信号格式变更时** —— 更新预期的 JSON 结构
4. **新增指标时** —— 记录新增计算内容的说明
5. **传输协议变更时** —— 更新 curl 示例及预期响应

## 常见故障与诊断

| 现象 | 检查项 | 解决办法 |
|---------|-------|-----|
| `[CRITICAL] Failed to fetch VIX` | 网络连接、Yahoo Finance 速率限制 | 5 分钟后重试，检查 IP 是否被封禁 |
| execution 中出现 JSON 解析错误 | analyst 的载荷格式发生了变化 | 检查 analyst/main.cpp 的近期修改 |
| Telegram 通知缺失 | Bot token / chat ID | 核实环境变量，用 curl 测试 token |
| 状态一直是 RISK_OFF | SPY 数据已过期 | 检查 Yahoo Finance 接口返回的是否为当前 K 线 |
| 200-SMA 计算结果异常 | 返回数据中不足 252 根 K 线 | 确认已获取完整一年的 SPY 数据 |
