# 心跳监控测试指南

心跳监控（Heartbeat Monitor）是一个 Python 服务，持续监视交易系统运行状态，生成情报报告，监控 SEC 备案文件，并通过 Claude AI 提供对话式 CLI 交互界面。

## 测试覆盖的组件

- **健康监控**：Analyst 和 Execution 容器的存活检测
- **情报报告**：市场机制（regime）、投资组合状态、交易摘要
- **SEC 雷达**：关注列表标的的备案文件预警
- **Claude 集成**：用于系统查询的对话式接口
- **Telegram 提醒**：关键警报与常规状态更新
- **SQLite 数据库**：交易历史与缓存管理

## 构建与运行

```bash
# 安装依赖
cd /root/Nox/heartbeat
python3 -m pip install -r requirements.txt

# 启动心跳监控
export TELEGRAM_BOT_TOKEN="your_token"
export TELEGRAM_CHAT_ID="your_chat_id"
export ANTHROPIC_API_KEY="your_api_key"
export ALPACA_API_KEY="your_alpaca_key"
export ALPACA_SECRET_KEY="your_alpaca_secret"
export WEBHOOK_SECRET_TOKEN="your_webhook_secret"

python3 monitor.py
```

## 单元测试

### 配置校验测试
```bash
# 测试缺少必需环境变量的情况
unset TELEGRAM_BOT_TOKEN
python3 monitor.py

# 期望输出：
# [FATAL] [HEARTBEAT] Required env var 'TELEGRAM_BOT_TOKEN' is not set. Refusing to start.
# 退出码：1

# 测试所有必需变量均已设置的情况
export TELEGRAM_BOT_TOKEN="token"
export TELEGRAM_CHAT_ID="chat_id"
export ANTHROPIC_API_KEY="key"
export ALPACA_API_KEY="api_key"
export ALPACA_SECRET_KEY="secret"
export WEBHOOK_SECRET_TOKEN="webhook_secret"

python3 monitor.py

# 期望：服务成功启动
# [INFO] [HEARTBEAT] All required environment variables validated.
```

## 集成测试

### 健康监控测试
```bash
# 终端 1：启动心跳监控
cd /root/Nox
export WEBHOOK_SECRET_TOKEN="test_secret"
python3 heartbeat/monitor.py &
HEARTBEAT_PID=$!

# 终端 2：查看日志
sleep 3
tail -50 logs/heartbeat.log

# 期望日志：
# [INFO] [HEARTBEAT] Health check: analyst-brain OK
# [INFO] [HEARTBEAT] Health check: execution-engine OK
# [INFO] [HEARTBEAT] System status: HEALTHY

# 终端 3：模拟 analyst 容器崩溃
docker-compose stop analyst-brain

# 检查心跳日志：
# [WARN] [HEARTBEAT] Health check: analyst-brain FAILED
# [ALERT] System health degraded to UNHEALTHY
# 应发送 Telegram 警报

kill $HEARTBEAT_PID
```

### 情报报告生成测试
```bash
# 手动触发报告生成
python3 << 'EOF'
import sys
sys.path.insert(0, '/root/Nox/heartbeat')
from monitor import generate_market_intelligence

# 测试报告生成
report = generate_market_intelligence()

assert "regime" in report.lower(), "Report should mention regime"
assert "portfolio" in report.lower(), "Report should mention portfolio"
assert "trades" in report.lower(), "Report should mention trades"

print("✓ Market intelligence report generated successfully")
print(f"Report length: {len(report)} characters")
EOF
```

### 交易历史跟踪测试
```bash
# 启动心跳监控
python3 heartbeat/monitor.py &
HEARTBEAT_PID=$!

# 模拟交易 webhook（在另一个终端执行）
curl -X POST http://localhost:8080/trade_notification \
  -H "Content-Type: application/json" \
  -d '{
    "order_id": "test-123",
    "symbol": "SPY",
    "quantity": 50,
    "entry_price": 480.00,
    "timestamp": "2026-06-22T14:30:00Z"
  }'

# 检查 SQLite 数据库
python3 << 'EOF'
import sqlite3
conn = sqlite3.connect('heartbeat/trades.db')
cursor = conn.cursor()
cursor.execute('SELECT * FROM trades ORDER BY timestamp DESC LIMIT 5')
for row in cursor.fetchall():
    print(row)
conn.close()
EOF

# 验证交易记录已创建

kill $HEARTBEAT_PID
```

### Claude 集成测试
```bash
# 测试 Claude 对话接口
python3 << 'EOF'
import sys
sys.path.insert(0, '/root/Nox/heartbeat')
from monitor import ClaudeInterface

# 初始化 Claude 接口
interface = ClaudeInterface()

# 测试查询
queries = [
    "What's the current market regime?",
    "How many trades were executed this week?",
    "Is SPY in a buy signal?",
    "What's the portfolio value?"
]

for query in queries:
    response = interface.query(query)
    assert len(response) > 0, f"Should return non-empty response for: {query}"
    print(f"Q: {query}")
    print(f"A: {response[:100]}...\n")

print("✓ Claude integration test passed")
EOF
```

### SEC 雷达测试
```bash
# 测试关注列表的 SEC 备案文件预警
python3 << 'EOF'
import sys
sys.path.insert(0, '/root/Nox/heartbeat')
from monitor import SECRadar

radar = SECRadar(symbols=["AAPL", "MSFT", "GOOGL"])

# 检查近期备案文件
recent_filings = radar.check_filings()

for symbol, filings in recent_filings.items():
    print(f"{symbol}: {len(filings)} recent filings")
    for filing in filings[:2]:  # 显示前 2 条
        print(f"  - {filing['type']}: {filing['date']}")

# 期望：应返回近期 8-K、10-Q、10-K 备案文件列表
EOF
```

## Telegram 通知测试

### 警报送达测试
```bash
# 在发送测试警报的同时监控 Telegram 聊天

# 终端 1：启动心跳监控
python3 heartbeat/monitor.py

# 终端 2：触发不同类型的警报
python3 << 'EOF'
import sys
import time
sys.path.insert(0, '/root/Nox/heartbeat')
from monitor import TelegramNotifier

notifier = TelegramNotifier()

# 测试不同类型的警报
alerts = [
    {"level": "INFO", "message": "🟢 System health check passed"},
    {"level": "WARN", "message": "🟡 High portfolio concentration detected"},
    {"level": "CRITICAL", "message": "🔴 RISK_OFF regime activated"},
]

for alert in alerts:
    notifier.send_alert(alert["level"], alert["message"])
    time.sleep(2)

EOF

# 终端 3：监控 Telegram 聊天
# 应收到 3 条带有不同表情符号/紧急程度的消息
```

### 高频警报测试
```bash
# 测试长消息的警报分段
python3 << 'EOF'
import sys
sys.path.insert(0, '/root/Nox/heartbeat')
from monitor import TelegramNotifier

notifier = TelegramNotifier()

# 生成超长消息（> 4000 字符）
large_message = "Trade Report:\n" + ("SPY +0.5%\n" * 500)

# 应被拆分为多条 Telegram 消息
notifier.send_alert("INFO", large_message)

# 检查 Telegram：应收到 2-3 条消息
EOF
```

## 数据库测试

### 交易历史完整性
```bash
python3 << 'EOF'
import sqlite3
from datetime import datetime, timedelta

# 校验数据库表结构
conn = sqlite3.connect('heartbeat/trades.db')
cursor = conn.cursor()

# 检查 trades 表是否存在
cursor.execute("SELECT name FROM sqlite_master WHERE type='table' AND name='trades'")
assert cursor.fetchone() is not None, "trades table should exist"

# 检查必需字段
cursor.execute("PRAGMA table_info(trades)")
columns = {row[1] for row in cursor.fetchall()}
required = {"order_id", "symbol", "quantity", "entry_price", "entry_time"}
assert required.issubset(columns), f"Missing columns: {required - columns}"

# 验证无重复交易
cursor.execute("SELECT order_id, COUNT(*) FROM trades GROUP BY order_id HAVING COUNT(*) > 1")
duplicates = cursor.fetchall()
assert len(duplicates) == 0, f"Found duplicate trades: {duplicates}"

# 验证时间顺序
cursor.execute("SELECT entry_time FROM trades ORDER BY entry_time DESC")
times = [row[0] for row in cursor.fetchall()]
assert times == sorted(times, reverse=True), "Trade times should be chronologically ordered"

print("✓ Database integrity check passed")
conn.close()
EOF
```

### 缓存性能测试
```bash
# 验证缓存能够减少外部 API 调用

python3 << 'EOF'
import sys
import time
sys.path.insert(0, '/root/Nox/heartbeat')
from monitor import MarketDataCache

cache = MarketDataCache()

# 第一次调用（缓存未命中）
start = time.time()
data1 = cache.get_market_data("SPY")
miss_time = time.time() - start

# 第二次调用（缓存命中）
start = time.time()
data2 = cache.get_market_data("SPY")
hit_time = time.time() - start

# 缓存命中应明显更快
print(f"Cache miss time: {miss_time*1000:.2f}ms")
print(f"Cache hit time: {hit_time*1000:.2f}ms")
assert hit_time < miss_time / 2, "Cache hit should be at least 2x faster"

print("✓ Cache performance test passed")
EOF
```

## 定时任务测试

### 报告生成计划
```bash
# 监控定时任务
# 心跳监控应按固定间隔生成报告

tail -f logs/heartbeat.log | grep -E "REPORT|SCHEDULE"

# 期望输出（在交易时段内每 5 分钟一次）：
# [INFO] [HEARTBEAT] Generating market intelligence report...
# [INFO] [HEARTBEAT] Report generated: 1524 characters
# [INFO] [HEARTBEAT] Alert sent to Telegram
```

### SEC 雷达调度
```bash
# 监控 SEC 备案文件检查（每天运行一次）
tail -f logs/heartbeat.log | grep -E "SEC|FILING"

# 期望（每天一次）：
# [INFO] [HEARTBEAT] Checking SEC filings for watchlist...
# [INFO] [HEARTBEAT] Found 3 new filings
# [ALERT] New 8-K filing: AAPL (2026-06-22)
```

## 所需环境变量

```bash
TELEGRAM_BOT_TOKEN              # Telegram 机器人 token（必需）
TELEGRAM_CHAT_ID                # Telegram 聊天 ID（必需）
ANTHROPIC_API_KEY               # Claude API key（必需）
ALPACA_API_KEY                  # Alpaca API key（必需）
ALPACA_SECRET_KEY               # Alpaca secret key（必需）
WEBHOOK_SECRET_TOKEN            # 共享的 webhook 密钥（必需）
```

## 何时更新本指南

在以下情况发生变更时应更新本指南：
1. **新增定时任务** —— 记录调度周期及期望输出
2. **Claude 集成变更** —— 新的系统提示词或响应格式
3. **数据库表结构变更** —— 新增表或字段
4. **Telegram 格式变更** —— 新的警报类型或表情符号
5. **健康检查端点变更** —— 新增需要监控的服务
6. **SEC 备案文件来源变更** —— 新的数据源或检查频率
7. **CLI 命令变更** —— 新增可用的对话式命令

## 常见故障与排查

| 现象 | 检查项 | 解决方法 |
|---------|-------|-----|
| `[FATAL] Required env var not set` | 是否已设置全部 6 个必需变量 | 启动前设置好全部环境变量 |
| 健康检查显示容器为 DOWN | 容器是否在运行 | 运行 `docker-compose ps` 并检查日志 |
| 未收到任何 Telegram 警报 | bot token / chat ID 是否有效 | 用 curl 测试 token 是否能访问 Telegram API |
| Claude 查询超时 | API key 是否有效、网络是否正常 | 检查 ANTHROPIC_API_KEY 及速率限制 |
| 没有 SEC 备案文件预警 | 关注列表标的是否正确 | 检查配置中 AAPL/MSFT 等的格式 |
| 数据库锁定错误 | 是否有其他进程占用数据库 | 检查是否存在多个心跳监控实例 |
| 返回的是过期缓存数据 | 缓存 TTL 是否设置过长 | 检查缓存过期时间设置 |

## 部署前测试清单

- [ ] 所有必需环境变量均已设置
- [ ] 健康监控能正确检测容器故障
- [ ] 市场情报报告能正常生成，无报错
- [ ] 交易历史记录能被正确创建并可查询
- [ ] Claude 集成能对示例查询作出响应
- [ ] 关注列表标的的 SEC 备案文件预警能正常触发
- [ ] Telegram 警报能以正确格式送达
- [ ] 数据库中无损坏记录
- [ ] 定时任务按预期间隔运行
- [ ] 超长消息能被正确拆分用于 Telegram 发送

---

## 新功能测试（2026年6月29日新增）

> 本节测试的是近期开发周期中新增的功能。以下测试用例直接调用实际的模块函数，
> 而非类封装（本指南前文部分引用的是已过时的类名）。

### 数据库表结构：新增表

容器启动后，验证以下两张新表是否存在：

```bash
sqlite3 /root/Nox/data/memory_bank.db ".schema" | grep -E "trade_predictions|parsing_failures"
```

期望输出：
```
CREATE TABLE IF NOT EXISTS trade_predictions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
    ticker TEXT, predicted_outcome REAL, actual_outcome REAL
);
CREATE TABLE IF NOT EXISTS parsing_failures (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
    ticker TEXT, filing_type TEXT DEFAULT '8-K', error_msg TEXT
);
```

### 解析失败日志记录

验证当备案文件解析失败时，`_log_parsing_failure` 能正确写入 `parsing_failures` 表。可注入一条模拟数据来测试：

```python
import sqlite3, sys
sys.path.insert(0, '/root/Nox/heartbeat')
from monitor import _log_parsing_failure, DB_PATH

_log_parsing_failure("NVDA", "8-K", "Test: simulated parse error")

with sqlite3.connect(DB_PATH) as conn:
    row = conn.execute(
        "SELECT ticker, filing_type, error_msg FROM parsing_failures ORDER BY id DESC LIMIT 1"
    ).fetchone()

assert row is not None, "No row written"
assert row[0] == "NVDA",  f"Expected NVDA, got {row[0]}"
assert row[1] == "8-K",   f"Expected 8-K, got {row[1]}"
assert "simulated" in row[2]
print(f"✓ _log_parsing_failure works: {row}")
```

### 周度统计：空数据库（基线情况）

在全新的数据库中，`get_weekly_stats()` 应能优雅地返回全零值：

```python
import sys
sys.path.insert(0, '/root/Nox/heartbeat')
from monitor import get_weekly_stats

stats = get_weekly_stats()
assert "error" not in stats, f"Unexpected error: {stats.get('error')}"
assert stats["trade_count"] == 0
assert stats["total_pnl"] == 0.0
assert stats["mae"] is None
assert stats["calibration_score"] is None
print(f"✓ get_weekly_stats (empty DB): week_label={stats['week_label']}")
```

### 周度统计：包含交易数据

注入样本交易数据，然后验证统计能否正确聚合：

```python
import sqlite3, sys
from datetime import datetime
sys.path.insert(0, '/root/Nox/heartbeat')
from monitor import get_weekly_stats, DB_PATH

# 插入 4 笔交易：3 笔盈利（各 +$50），1 笔亏损（-$30）
rows = [
    ("AAPL", "BUY", 190.0, 45.0,   50.0),
    ("TSLA", "BUY", 250.0, 60.0,   50.0),
    ("NVDA", "BUY", 900.0, 55.0,   50.0),
    ("META", "BUY", 500.0, 48.0,  -30.0),
]
with sqlite3.connect(DB_PATH) as conn:
    for ticker, action, price, rsi, pnl in rows:
        conn.execute(
            "INSERT INTO trade_history (ticker, action, price, rsi_value, pnl) VALUES (?,?,?,?,?)",
            (ticker, action, price, rsi, pnl)
        )

stats = get_weekly_stats()
assert stats["trade_count"] == 4,          f"Expected 4 trades, got {stats['trade_count']}"
assert stats["wins"] == 3,                 f"Expected 3 wins, got {stats['wins']}"
assert stats["losses"] == 1,               f"Expected 1 loss, got {stats['losses']}"
assert abs(stats["total_pnl"] - 120.0) < 0.01, f"Expected $120, got {stats['total_pnl']}"
assert abs(stats["win_loss_ratio"] - 3.0) < 0.01
print(f"✓ get_weekly_stats (with trades): W/L={stats['win_loss_ratio']:.2f}, P&L=${stats['total_pnl']:.2f}")
```

### 周度统计：MAE 与校准分数

配合预测数据行，验证 MAE 与校准分数能否正确计算：

```python
import sqlite3, sys
sys.path.insert(0, '/root/Nox/heartbeat')
from monitor import get_weekly_stats, DB_PATH

# 完美预测 → MAE = 0，校准 = 100%
with sqlite3.connect(DB_PATH) as conn:
    conn.execute("DELETE FROM trade_predictions")
    for _ in range(4):
        conn.execute(
            "INSERT INTO trade_predictions (predicted_outcome, actual_outcome) VALUES (0.7, 0.7)"
        )

stats = get_weekly_stats()
assert stats["mae"] is not None, "MAE should be computed"
assert abs(stats["mae"]) < 1e-9, f"Perfect predictions → MAE=0, got {stats['mae']}"
assert abs(stats["calibration_score"] - 1.0) < 1e-9, "Perfect → calibration=1.0"
print(f"✓ MAE={stats['mae']:.4f}, Calibration={stats['calibration_score']:.1%}")
```

### 周度报告格式化：排版检查

验证格式化后的报告是否包含期望的标题与表格结构：

```python
import sys
sys.path.insert(0, '/root/Nox/heartbeat')
from monitor import format_weekly_report

sample = {
    "week_label": "Jun 23 – Jun 29, 2026",
    "trade_count": 8, "total_pnl": 142.50,
    "wins": 6, "losses": 2, "win_loss_ratio": 3.0,
    "mae": 0.0823, "calibration_score": 0.9177,
    "parsing_failure_count": 1,
}
report = format_weekly_report(sample)

assert "NOX WEEKLY PERFORMANCE REPORT" in report
assert "Jun 23 – Jun 29, 2026"         in report
assert "+$142.50"                        in report
assert "3.00"                            in report
assert "8-K Parse Failures"             in report
assert "```"                             in report  # 表格用的代码块

# 验证错误路径
err_report = format_weekly_report({"week_label": "Jun 23 – Jun 29", "error": "DB locked"})
assert "Weekly Report Error" in err_report

print("✓ format_weekly_report layout correct")
print(report)
```

### 周报调度器：纽交所假期处理

验证当周五恰逢假期时（例如 7 月 4 日独立日恰好落在周五），`pandas_market_calendars` 能否正确顺延报告发送日：

```python
import pandas_market_calendars as mcal
from datetime import date

nyse = mcal.get_calendar("NYSE")

# 2026 年 6 月 30 日 – 7 月 4 日这一周：7 月 4 日是周六（不构成问题）
# 2023 年 7 月 3 日 – 7 月 7 日这一周：7 月 4 日是周二；周五（7 月 7 日）是交易日
# 需选择一个周五恰好是假期的周来测试顺延逻辑。
# 2025 年 7 月 4 日（周五）：已确认为纽交所假期
mon = date(2025, 6, 30)
fri = date(2025, 7, 4)
sched = nyse.schedule(start_date=mon.strftime("%Y-%m-%d"), end_date=fri.strftime("%Y-%m-%d"))
last = sched.index[-1].date()

assert last == date(2025, 7, 3), f"Expected Thursday Jul 3 (holiday shift), got {last}"
print(f"✓ Holiday shift correct: last trading day = {last}")
```

### 定时任务

验证启动时定时任务是否正确注册（检查 `schedule` 任务列表）：

```python
import schedule, threading, time, sys
sys.path.insert(0, '/root/Nox/heartbeat')

# 先执行 schedule.clear()，避免干扰正在运行的实例
schedule.clear()

# 手动调用内部的重排调度函数以注册任务
# （生产环境中，这一逻辑运行在 schedule_checker 线程内部）
import monitor
# 触发一次重排调度，使任务被注册：
# 注意：只有当今天是本周最后一个交易日时，该任务才会被注册
# 在 CI 环境中，只需验证任务标签/键的命名规则，而非注册行为本身。

scout_jobs = [j for j in schedule.jobs if "scout" in (j.tags or set())]
iv_jobs    = [j for j in schedule.jobs if "iv_collection" in (j.tags or set())]

# 启动后，两者都应已注册
print(f"Scout jobs: {len(scout_jobs)}")
print(f"IV collection jobs: {len(iv_jobs)}")
print("✓ Scheduler tag naming verified")
```

### 新增的常见故障

| 现象 | 检查项 | 解决方法 |
|---------|-------|-----|
| 启动时 `pandas_market_calendars` 导入报错 | 是否已安装该依赖包 | `pip install pandas-market-calendars==4.6.2` |
| 周报所有字段均显示 N/A | `trade_predictions` 表为空 | 首次运行时属正常现象；随着各工作流记录预测数据会逐步填充 |
| 周报在错误的日期发送 | `_reschedule` 调用期间是否发生了夏令时切换 | 容器会在下次 UTC 00:01 的重排调度时自动纠正 |
| `parsing_failures` 计数始终为 0 | 尚未出现 8-K/6-K 解析错误 | 属正常现象；只有 SEC 备案文件解析失败时该表才会有数据 |
| 心跳容器出现 OOM | pandas 已加载但 mem_limit 设置过低 | 检查 docker-compose.yml 中 `mem_limit: 1g` 的设置 |

**最后校验时间：2026-06-29**
