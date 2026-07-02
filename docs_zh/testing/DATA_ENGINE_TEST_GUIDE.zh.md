# 数据引擎测试指南（美国 & 中国）

数据引擎是一组 FastAPI 服务，负责抓取、处理并向心跳监控器提供市场数据、新闻和情报。系统运行两个实例：`america-data-engine`（美股、宏观数据、新闻）和 `china-data-engine`（中国 A 股、宏观数据、新闻）。

## 被测组件

- **认证网关（Authentication Gate）**：API key 请求头校验
- **数据抓取器（Data Scrapers）**：从外部来源采集新闻/宏观数据
- **缓存（Caching）**：缓存响应以减少外部 API 调用
- **限流（Rate Limiting）**：高负载下的优雅降级
- **错误处理（Error Handling）**：过期数据回退行为

## 构建与运行

```bash
# 构建 Docker 镜像
docker build -t nox-data-engine ./

# 或者直接用 Python 运行
cd /root/Nox/america_data_engine
python3 -m pip install -r requirements.txt
WEBHOOK_SECRET_TOKEN="test_secret" python3 main.py
```

## 单元测试

### 认证测试（手动）
```bash
# 启动数据引擎
export WEBHOOK_SECRET_TOKEN="my_secret_key"
python3 america_data_engine/main.py &
DATA_PID=$!

sleep 2

# 使用有效 token 测试
curl -X GET http://localhost:8001/health \
  -H "X-Nox-Token: my_secret_key"

# 预期：HTTP 200，响应体为：{"status": "healthy"}

# 使用无效 token 测试
curl -X GET http://localhost:8001/health \
  -H "X-Nox-Token: wrong_secret"

# 预期：HTTP 401 Unauthorized

# 不带 token 测试
curl -X GET http://localhost:8001/health

# 预期：HTTP 403 Forbidden（FastAPI 的 auto_error=True 行为）

kill $DATA_PID
```

### 抓取函数测试
```bash
# 测试 Alpaca 新闻抓取器
python3 << 'EOF'
import sys
sys.path.insert(0, '/root/Nox/america_data_engine')
from scrapers import fetch_alpaca_news

# 使用有效股票代码测试
news = fetch_alpaca_news("SPY")
assert isinstance(news, list), "Should return list of articles"
assert len(news) > 0, "Should return non-empty list"
assert "title" in news[0], "Articles should have title field"

print("✓ Alpaca news scraper works")
EOF
```

### 限流测试
```bash
# 模拟高频请求
for i in {1..10}; do
  curl -X GET http://localhost:8001/api/news/spy \
    -H "X-Nox-Token: my_secret_key" &
done
wait

# 检查日志：
# - 前几个请求：200 OK
# - 后续请求：429 Too Many Requests（如果限流阈值是 5 次/分钟）
# - 优雅降级（不应出现 500 错误）
```

## 集成测试

### America Data Engine 完整测试
```bash
# 终端 1：启动 America 数据引擎
cd /root/Nox
export WEBHOOK_SECRET_TOKEN="test_secret"
python3 america_data_engine/main.py

# 终端 2：测试各接口
# 健康检查
curl -X GET http://localhost:8001/health \
  -H "X-Nox-Token: test_secret"

# 获取 SPY 的新闻
curl -X GET http://localhost:8001/api/news/spy \
  -H "X-Nox-Token: test_secret"

# 预期响应：
# [
#   {
#     "title": "Market Rally Continues...",
#     "source": "Reuters",
#     "publish_time": "2026-06-22T14:30:00Z",
#     "url": "https://..."
#   },
#   ...
# ]

# 获取宏观数据
curl -X GET http://localhost:8001/api/macro \
  -H "X-Nox-Token: test_secret"

# 预期响应：
# {
#   "vix": 18.5,
#   "spy_price": 482.50,
#   "unemployment": 4.1,
#   "gdp_growth": 2.4,
#   "timestamp": "2026-06-22T14:30:00Z"
# }
```

### China Data Engine 完整测试
```bash
# 终端 1：启动 China 数据引擎
cd /root/Nox
export WEBHOOK_SECRET_TOKEN="test_secret"
python3 china_data_engine/main.py

# 终端 2：测试各接口
# 健康检查
curl -X GET http://localhost:8002/health \
  -H "X-Nox-Token: test_secret"

# 获取阿里巴巴（Alibaba）的新闻
curl -X GET http://localhost:8002/api/news/baba \
  -H "X-Nox-Token: test_secret"

# 预期响应：与 America 引擎格式相同
```

### Docker Compose 集成测试
```bash
# 启动全部三个数据引擎
docker-compose up -d

sleep 3

# 测试 America 引擎
curl -X GET http://localhost:8001/health \
  -H "X-Nox-Token: ${WEBHOOK_SECRET_TOKEN}"

# 测试 China 引擎
curl -X GET http://localhost:8002/health \
  -H "X-Nox-Token: ${WEBHOOK_SECRET_TOKEN}"

# 验证两者均返回 200
docker-compose logs america-data-engine | tail -10
docker-compose logs china-data-engine | tail -10
```

## 性能与负载测试

### 响应时间测试
```bash
# 测量 100 次请求的响应时间
time for i in {1..100}; do
  curl -s -X GET http://localhost:8001/health \
    -H "X-Nox-Token: test_secret" > /dev/null
done

# 预期：应在 5 秒内完成
# 平均每次请求延迟 < 50ms
```

### 并发请求测试
```bash
# 发送 20 个并发请求
ab -n 20 -c 10 \
  -H "X-Nox-Token: test_secret" \
  http://localhost:8001/health

# 预期：所有请求均成功完成（0 次失败）
# 延迟应保持稳定一致
```

### 缓存命中测试
```bash
# 第一次请求（缓存未命中）
time curl -X GET http://localhost:8001/api/news/spy \
  -H "X-Nox-Token: test_secret" > /dev/null

# 第二次请求（缓存命中）——应显著更快
time curl -X GET http://localhost:8001/api/news/spy \
  -H "X-Nox-Token: test_secret" > /dev/null

# 检查日志：
# 第一次请求：[DEBUG] Cache miss for /api/news/spy
# 第二次请求：[DEBUG] Cache hit for /api/news/spy (TTL: 300s)
```

## 错误处理测试

### 外部 API 超时
```bash
# 断开网络连接或屏蔽 API 主机
# 尝试获取新闻
curl -X GET http://localhost:8001/api/news/spy \
  -H "X-Nox-Token: test_secret"

# 预期：503 Service Unavailable，消息为：
# {"error": "External API timeout", "cached_data_age": "5 minutes old"}
# （前提是存在可用的缓存数据）
```

### 格式错误的请求
```bash
# 测试无效股票代码
curl -X GET http://localhost:8001/api/news/INVALID_SYMBOL_XYZABC \
  -H "X-Nox-Token: test_secret"

# 预期：400 Bad Request，消息为：
# {"error": "Invalid symbol format"}
```

### 缺少必需请求头
```bash
# 测试不带 X-Nox-Token 的请求
curl -X GET http://localhost:8001/health

# 预期：HTTP 403 Forbidden（FastAPI 会在进入处理函数之前自动拒绝）
```

## 所需环境变量

```bash
WEBHOOK_SECRET_TOKEN          # 认证网关的共享密钥（必需）
ALPACA_API_KEY                # Alpaca 新闻抓取器所需（必需）
ALPACA_SECRET_KEY             # Alpaca 新闻抓取器所需（必需）
# 可选：代理、限流阈值覆盖、数据源相关配置
```

## 何时更新本指南

以下情况应更新本指南：
1. **新增接口** —— 需记录 URL 路径、参数、响应格式
2. **抓取数据源变更** —— 例如从 Alpaca 切换到其他新闻提供商
3. **认证方式变更** —— 新的请求头格式或 token 校验逻辑
4. **缓存 TTL 变更** —— 更新预期的缓存命中时间
5. **错误处理变更** —— 新的错误码或回退策略
6. **限流策略变更** —— 更新并发预期
7. **响应格式变更** —— 更新预期的 JSON 结构

## 常见故障与诊断

| 现象 | 检查项 | 解决方法 |
|---------|-------|-----|
| 每次请求都返回 `401 Unauthorized` | WEBHOOK_SECRET_TOKEN 环境变量 | 确认 token 与请求头中的值一致 |
| `503 Service Unavailable` | 外部 API 连通性 | 检查网络连接，查看 API 状态页 |
| 新闻数组为空 `[]` | 数据源未返回文章 | 在低活跃时段属正常现象 |
| 响应耗时超过 5 秒 | 限流或外部 API 延迟 | 检查并发请求数量 |
| `[ERROR] Parse error in scraper` | 数据源格式发生变化 | 检查数据源的 HTML/API 格式 |
| 不同副本间响应不一致 | 缓存未共享 | 确认所有副本使用相同的 WEBHOOK_SECRET_TOKEN |

## 部署前测试检查清单

- [ ] 认证网关能拒绝不带有效 token 的请求
- [ ] 所有接口在通过认证后均返回 200
- [ ] 新闻抓取器返回有效的文章数组
- [ ] 宏观数据包含所有预期字段
- [ ] 响应在 2 秒内完成（p95）
- [ ] 并发请求（10+）不会导致失败
- [ ] 缓存能有效减少外部 API 调用
- [ ] 超时不会导致服务崩溃
- [ ] 无效请求返回恰当的 4xx 错误
