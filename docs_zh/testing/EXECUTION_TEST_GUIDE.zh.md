# 执行模块测试指南

执行模块接收来自分析端（Analyst）的信号，通过多重关卡（schema 校验、鉴权、动量过滤、市场状态判断）进行验证，使用凯利公式（Kelly Criterion）计算仓位大小，并将订单路由至 Alpaca。

## 已测试组件

- **Schema Gate（结构校验关卡）**：JSON 格式校验
- **Auth Gate（鉴权关卡）**：密钥令牌验证
- **Momentum Filter（动量过滤器）**：RSI 超买/超卖检测
- **Regime Gate（市场状态关卡）**：市场状态重新评估
- **Portfolio Fetch（组合拉取）**：实盘净值验证
- **Kelly Sizing（凯利仓位计算）**：仓位规模计算
- **Order Routing（订单路由）**：Alpaca API 对接

## 单元测试

### 凯利公式仓位计算
```bash
g++ -std=c++17 -pthread -o build/test_kelly_sizing tests/test_kelly_sizing.cpp
./build/test_kelly_sizing
```

**测试内容**：
- 当组合资金不足以买入最小单位（1 股）时阻止交易
- 根据胜率和盈亏比动态调整仓位大小
- 遵守 10% 最大风险上限
- 遵守 1% 最小配置下限

**预期输出**：
```
✓ Kelly sizing: $100k portfolio, 1% min, 10% max
  - Kelly calc: 15% of portfolio
  - Cap to max: 10% → 1,000 shares @ $100 = $100,000
✓ Kelly sizing prevents 0-share trades
✓ Kelly sizing handles edge cases (no edge = 0 allocation)
```

## 手动集成测试

### 1. Schema 校验测试
```bash
# 使用合法的请求体测试
curl -X POST http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{
    "regime": "RISK_ON",
    "vix": 18.5,
    "spy_price": 482.50,
    "sma_200": 475.20,
    "secret_key": "YOUR_WEBHOOK_SECRET"
  }'

# 预期：HTTP 200，订单被处理
# 检查日志：[INFO] Schema validation passed
```

### 2. 鉴权关卡测试
```bash
# 使用错误密钥测试
curl -X POST http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{
    "regime": "RISK_ON",
    "vix": 18.5,
    "spy_price": 482.50,
    "sma_200": 475.20,
    "secret_key": "WRONG_SECRET"
  }'

# 预期：HTTP 401（静默丢弃，不暴露任何指纹信息）
# 检查日志：[WARN] Authentication failed
```

### 3. Schema 拒绝测试
```bash
# 使用格式错误的 JSON 测试
curl -X POST http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{invalid json}'

# 预期：HTTP 400，在鉴权关卡之前即被拒绝
# 检查日志：[ERROR] JSON parse failed
```

### 4. 动量过滤器测试（手动）
构造一个 RSI > 70（超买）的测试信号：

```bash
curl -X POST http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{
    "regime": "RISK_ON",
    "vix": 18.5,
    "spy_price": 482.50,
    "sma_200": 475.20,
    "rsi": 75,
    "secret_key": "YOUR_WEBHOOK_SECRET"
  }'

# 预期：[WARN] RSI > 70, blocking BUY signal
# 不应下单
```

### 5. 市场状态关卡测试（手动）
构造一个切换为 RISK_OFF 的信号：

```bash
curl -X POST http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{
    "regime": "RISK_ON",
    "vix": 40,              # VIX 现在处于高位
    "spy_price": 450.0,     # SPY 现在跌破 SMA
    "sma_200": 475.20,
    "secret_key": "YOUR_WEBHOOK_SECRET"
  }'

# 预期：[CRITICAL] Regime re-evaluated to RISK_OFF, blocking order
# 检查 Telegram 是否收到市场状态告警
```

### 6. 组合净值拉取测试
```bash
# 将 ALPACA_BASE_URL 设为模拟盘（paper trading）端点
export ALPACA_BASE_URL="https://paper-api.alpaca.markets"
export ALPACA_API_KEY="your_alpaca_key"
export ALPACA_SECRET_KEY="your_alpaca_secret"

# 发送一个合法信号
curl -X POST http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{...signal...}'

# 检查日志是否出现：
# [INFO] Fetching live portfolio equity from Alpaca
# [INFO] Live equity: $100,000.00
# 若净值拉取连续失败 3 次：[CRITICAL] Aborting order
```

### 7. 凯利仓位计算测试（手动）
```bash
# 发送带有预期仓位计算场景的信号：
# 组合：$100k，历史胜率：0.55，平均盈利：2%，平均亏损：1%
# Kelly = 0.55 - ((1-0.55)/2) = 0.55 - 0.225 = 0.325 = 32.5%

curl -X POST http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{
    "regime": "RISK_ON",
    "vix": 18.5,
    "spy_price": 482.50,
    "sma_200": 475.20,
    "kelly_fraction": 0.25,    # 安全系数：32.5% * 0.25 = 8.125%
    "secret_key": "YOUR_WEBHOOK_SECRET"
  }'

# 检查日志是否出现：
# [INFO] Kelly calculated: 8.125% of $100k = $8,125
# [INFO] Share quantity: 16 shares @ $482.50 = $7,720.00
```

### 8. 订单路由测试（模拟盘）
```bash
# 确保 ALPACA_BASE_URL 指向模拟盘 API
# 在交易时段内发送一个合法信号

curl -X POST http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{
    "regime": "RISK_ON",
    "vix": 18.5,
    "spy_price": 482.50,
    "sma_200": 475.20,
    "secret_key": "YOUR_WEBHOOK_SECRET"
  }'

# 预期结果：
# 1. [INFO] Order submitted to Alpaca
# 2. [INFO] Order ID: xxxx-xxxx-xxxx-xxxx
# 3. 检查 Alpaca 后台面板 —— 应出现新的模拟盘订单
# 4. 收到带有订单确认信息的 Telegram 通知
```

## 集成测试（完整链路）

### Analyst → Execution → Alpaca
```bash
# 终端 1：启动执行引擎
docker-compose up execution-engine

# 终端 2：运行分析端
docker-compose run analyst

# 终端 3：监控执行日志
docker logs -f nox_execution-engine

# 验证完整链路：
# 1. 分析端拉取数据
# 2. 分析端发送信号
# 3. 执行端接收并校验
# 4. 执行端调用 Alpaca
# 5. 订单出现在 Alpaca 后台面板
# 6. 收到 Telegram 通知
```

## 所需环境变量

```bash
WEBHOOK_SECRET_TOKEN          # 鉴权关卡使用的共享密钥（必填）
ALPACA_BASE_URL               # 模拟盘或实盘 API 端点（必填）
ALPACA_API_KEY                # Alpaca API key（必填）
ALPACA_SECRET_KEY             # Alpaca secret key（必填）
KELLY_FRACTION                # 安全系数，默认 0.25（可选）
TELEGRAM_BOT_TOKEN            # Telegram 通知（可选）
TELEGRAM_CHAT_ID              # Telegram 会话 ID（可选）
```

## 何时更新本指南

出现以下情况时应更新本指南：
1. **关卡逻辑变更** —— 新增校验规则或调整阈值
2. **凯利公式变更** —— 输入参数或计算方法发生变化
3. **Alpaca API 变更** —— 接口更新、新增必填字段
4. **订单格式变更** —— 订单请求体结构被修改
5. **新增关卡** —— 需记录校验顺序及失败处理方式

## 常见故障与排查

| 现象 | 排查方向 | 解决方法 |
|---------|-------|-----|
| `[WARN] Authentication failed` | 检查 WEBHOOK_SECRET_TOKEN 是否匹配 | 确认环境变量设置正确 |
| `[ERROR] JSON parse failed` | 请求体格式错误 | 校验 JSON 语法及必填字段 |
| `[CRITICAL] RSI filter blocked` | 检查信号中的 RSI 值 | 调整 RSI 阈值或信号来源 |
| `[CRITICAL] Portfolio equity failed 3x` | 检查 Alpaca 连接状态 | 检查 API key、端点 URL、网络连接 |
| `[INFO] Kelly calculated 0%` | 无统计优势（edge） | 提高胜率或盈亏比 |
| Alpaca 后台未见订单 | 检查 ALPACA_BASE_URL | 确认模拟盘与实盘端点未混用 |
| 未收到 Telegram 通知 | 检查 bot token / chat ID | 单独测试 Telegram 连接 |

## 部署前测试清单

- [ ] 全部 3 个关卡（schema、鉴权、动量）均能拒绝非法信号
- [ ] 市场状态重新评估能在 VIX 达到 40 时正确切换为 RISK_OFF
- [ ] 凯利仓位计算遵守 1% 最小、10% 最大的上下限
- [ ] 组合净值拉取正常工作，且失败时会重试 3 次
- [ ] 订单路由能在 Alpaca 模拟盘中成功创建订单
- [ ] 所有关键事件均能触发 Telegram 告警
- [ ] HTTP 超时不会导致服务器挂起
- [ ] 大体量请求（批量）能被正确处理
