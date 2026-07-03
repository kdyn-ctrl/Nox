# TradingView ↔ Nox 集成指南

> 如何将 TradingView 与 Nox 配合使用——既可以完全独立运行，也可以接入 Nox
> 的执行引擎，让 TradingView 的提醒（alert）注入实时交易信号。
>
> 已对照 `execution/main.cpp`（`/webhook` 处理函数位于第 1603 行，
> `TradeSignal` 结构体位于第 43 行）以及 `docker-compose.yml`（Traefik
> 标签，第 136–140 行）核实，核实时间为 2026-06-29。

---

## 太长不看版（TL;DR）

Nox 只对外暴露一个可从外部访问的接入端点：执行引擎（8080 端口）上的
**`POST /webhook`**。它通过 JSON 请求体**内部**的 `secret_key` 字段做鉴权——
这恰好也是 TradingView webhook 能够发送的方式。

你有两种模式可选：

1. **独立模式** —— TradingView 只是你的图表/提醒工具，Nox 运行自己的扫描器，
   不需要做任何集成。（建议从这里开始。）
2. **集成模式** —— TradingView 的提醒触发一次 HTTP POST 到 Nox 的
   `/webhook`，由 Nox 完成仓位大小计算并通过 Alpaca 下单路由。

---

## 模式一 —— 独立运行 TradingView（建议起步方式）

在 Nox 这边不需要做任何配置。把 TradingView 当作图表、指标和提醒工具，
提醒可以推送到你的手机或邮箱。Nox 自身的 `OptionsSignalGenerator` 线程会
按照自己的节奏持续扫描和交易（`execution/main.cpp:1794`）。

**需要注意的一点：** 一旦你做了集成，TradingView 和 Nox 的扫描器就都能在
同一账户上下单了。在纸上交易（paper trading）测试期间，务必先确定谁是
"信号来源的唯一真相"，或者干脆用两个不同的纸上交易账户，这样你才能分清
每一笔成交到底是谁触发的。两条路径之间没有任何去重机制。

---

## 模式二 —— 将 TradingView 提醒接入 Nox

### 信号是如何流转的（已核实的路径）

```
TradingView alert  ──HTTPS POST──▶  Traefik  ──▶  execution-engine:8080 /webhook
                                                        │ (main.cpp:1603)
                                                        ▼
                                          secret_key auth gate (main.cpp:1616)
                                                        │  (mismatch = silent drop, still HTTP 200)
                                                        ▼
                                          build TradeSignal (main.cpp:1621)
                                                        ▼
                                          record_signal()  → /recent-signals
                                                        ▼
                                          process(signal)  (main.cpp:539)
                                              BUY  → Kelly/tier sizing → Alpaca /v2/orders
                                              SELL → close position (T+1 gated)
                                              HOLD / REPORT → no-op
```

---

## 第一步 —— 获取一个可访问的 webhook URL

### 方案 A —— 你已经有域名/VPS（生产环境配置）

如果你的服务器有静态 IP 和域名，Traefik 会自动处理 TLS
（`docker-compose.yml:137-140`）。你的 URL 就是：
```
https://<你的域名>/webhook
```

> **关于临时域名的提醒：** 如果你之前用过某个 Hostinger / vanhellsing 的
> 域名，那个域名现在已经不再使用了。在把 TradingView 指向新地址之前，
> 请先在 `docker-compose.yml` 里更新 Traefik 标签，换成你当前正在使用的
> 域名。

### 方案 B —— 还没有域名（用 ngrok 隧道做测试）

在还没决定用哪个域名之前，测试 TradingView 集成最快的方式：

1. 安装 ngrok：`curl -sSL https://ngrok-agent.s3.amazonaws.com/ngrok.asc | sudo tee /etc/apt/trusted.gpg.d/ngrok.asc && sudo apt install ngrok`
2. 启动一个指向执行引擎的隧道：`ngrok http 8080`
3. ngrok 会给你一个临时的 HTTPS 地址，形如 `https://abc123.ngrok-free.app`
4. 在 TradingView 里填入 `https://abc123.ngrok-free.app/webhook`

**局限性：** 免费版 ngrok 的 URL 每次重启都会变。这个方案只适合用来测试——
一旦你确认没问题了，就去搭建一个正式域名。

### 方案 C —— 直接用 IP（不推荐）

TradingView 的 webhook 要求必须是 HTTPS。裸的 `http://IP:8080/webhook`
是行不通的。如果你的 VPS 只有 IP 没有域名，用方案 B（ngrok）来免费获取一个
带 TLS 的地址。

### 获取你的密钥

`.env` 中的 `WEBHOOK_SECRET_TOKEN` 就是鉴权凭证。它需要作为 `secret_key`
字段放进 JSON 请求体里。

---

## 第二步 —— 在 TradingView 中创建提醒（alert）

1. 打开一个图表，添加你的指标/策略。
2. 点击**提醒闹钟图标 → Create Alert（创建提醒）**。
3. 设置你的触发条件（指标交叉、价格水平、策略下单等）。
4. 在 **Notifications（通知）→ Webhook URL** 中粘贴第一步得到的 webhook
   地址。
5. 在 **Message（消息）** 框中粘贴下面的 JSON payload。

TradingView 会在提醒触发时，把 `{{ticker}}`、`{{close}}` 等占位符替换成
实际值。

---

## 第三步 —— payload 的字段规范

这个 webhook 接受**单个 JSON 对象**，也接受**一个 JSON 对象数组**。
各字段说明如下（`TradeSignal` 定义于 `main.cpp:43-58`，解析逻辑位于
`main.cpp:1621-1684`）：

| 字段 | 类型 | 是否必填 | 说明 |
|---|---|---|---|
| `secret_key` | string | **是** | 必须等于 `WEBHOOK_SECRET_TOKEN`。错误或缺失 = 静默返回 200。 |
| `ticker` | string | 是（实际意义上） | 若省略，默认值为 `"UNKNOWN"`。 |
| `action` | string | **是** | `BUY`、`SELL`、`HOLD` 或 `REPORT`。只有 `BUY`/`SELL` 会真正下单。 |
| `price` | number 或 string | 建议填写 | 入场/参考价格。 |
| `risk_tier` | int | 建议填写 | `0` 或省略 = 使用凯利公式（Kelly）计算仓位；`1`、`2`、… = 固定百分比档位。 |
| `rsi` | number | 否 | 可选的指标上下文信息。 |
| `vol` | int | 否 | 成交量。 |
| `atr` | number | 否 | 与 `stop_loss_atr_multiplier` 配合使用。 |
| `stop_loss_atr_multiplier` | number | 否 | 默认值 `2.0`。 |
| `vix` | number | 否 | 默认值 `20.0`。 |
| `spy_price` | number | 否 | 大盘环境上下文。 |
| `spy_200_sma` | number | 否 | 大盘环境上下文。 |
| `trade_date` | string | 否 | 格式为 `"YYYY-MM-DD"`——仅用于回测 / A 股 T+1 场景。 |

**最简可用的 BUY 提醒消息：**
```json
{"secret_key":"YOUR_WEBHOOK_SECRET_TOKEN","ticker":"{{ticker}}","action":"BUY","price":{{close}},"risk_tier":1}
```

**SELL（平仓）提醒消息：**
```json
{"secret_key":"YOUR_WEBHOOK_SECRET_TOKEN","ticker":"{{ticker}}","action":"SELL","price":{{close}}}
```

**在一条提醒里发送多个信号（数组形式）：**
```json
[
  {"secret_key":"YOUR_WEBHOOK_SECRET_TOKEN","ticker":"AAPL","action":"BUY","price":150.0,"risk_tier":1},
  {"secret_key":"YOUR_WEBHOOK_SECRET_TOKEN","ticker":"MSFT","action":"BUY","price":410.0,"risk_tier":1}
]
```

---

## 第四步 —— 验证你的提醒是否真的生效

> **重要提示：** TradingView 始终会显示 HTTP 200——即便 `secret_key`
> 错误，或者 payload 格式不对，也是如此，因为 Nox 会静默丢弃不合法的请求，
> 以避免暴露鉴权边界的细节（`main.cpp:1616`）。**永远不要只凭 TradingView
> 显示的"发送成功"就下结论。**

每次修改 payload 或 URL 之后，都请用下面这份验证清单确认一遍：

### 方法一 —— Telegram 确认（最快）
每一条成功解析的信号都会触发一条 Telegram 消息：
```
🚀 Signal Parsed: BUY AAPL
```
如果你在 TradingView 测试触发后大约 5 秒内没看到这条消息，说明信号被丢弃了。
常见原因：`secret_key` 错误、JSON 格式不对、`action` 拼写错误。

### 方法二 —— 手动 curl 测试（最适合调试 payload）
直接在运行 Nox 的机器上，用你实际的 payload 测试：
```bash
# 在 Docker 网络内部（从宿主机执行）
docker compose exec execution-engine curl -s -X POST \
  http://localhost:8080/webhook \
  -H "Content-Type: application/json" \
  -d '{"secret_key":"YOUR_WEBHOOK_SECRET_TOKEN","ticker":"AAPL","action":"BUY","price":150.0,"risk_tier":1}'
```
预期返回：`Processed 1 signal(s)`（如果鉴权失败则会返回
`Processed 0 signal(s)`）。

如果你的 payload 看起来完全正确，但返回的却是 `Processed 0 signal(s)`，
说明你的密钥不对。请仔细核对 `.env` 中的 `WEBHOOK_SECRET_TOKEN`，并重新
构建容器。

### 方法三 —— `/recent-signals` 接口
Nox 会在内存中保存最近 50 条收到的信号（仅限鉴权通过的）：
```bash
docker compose exec execution-engine curl -s localhost:8080/recent-signals | python3 -m json.tool
```
如果你的信号出现在列表里，说明它已通过鉴权并被正确解析。如果没有出现，
说明是被鉴权环节静默丢弃了。

### 方法四 —— Telegram 的 `/signals` 命令
向你的机器人发送 `/signals`，它会代理请求执行引擎的 `/recent-signals`
接口，并展示最近 N 条已解析信号及其时间戳。数据内容和方法三相同，只是
可以直接在手机上查看。

### 方法五 —— Docker 日志
```bash
docker compose logs -f execution-engine | grep -E "Signal Parsed|WARN.*auth|BUY|SELL"
```
成功解析会记录如下日志：`[INFO] Signal Parsed successfully: BUY AAPL`
鉴权失败会记录如下日志：`[WARN] [EXECUTION] Unauthorized signal silently dropped`

### 快速排查清单
- [ ] Telegram 是否显示了 `🚀 Signal Parsed`？→ 说明信号已到达引擎
- [ ] `/recent-signals` 里是否有这条信号？→ 说明鉴权通过了
- [ ] Alpaca 纸上交易账户里是否出现了订单？→ 说明 `process()` 完成了路由
- [ ] `action` 是否精确为 `BUY` 或 `SELL`（而不是 `buy`/`sell`——区分大小写）
- [ ] `secret_key` 是否与 `.env` 中的 `WEBHOOK_SECRET_TOKEN` 完全一致（没有多余空格）
- [ ] JSON 是否合法——如果拿不准，可以把 payload 粘贴到 `jsonlint.com` 校验
- [ ] TradingView 的 `{{ticker}}` 占位符是否正确解析（例如应该是 `AAPL`，而不是 `AAPL1!`）

---

## 第五步 —— 先在纸上交易环境测试

1. 确认 `.env` 中的 Alpaca 密钥指向的是**纸上交易环境**：
   `ALPACA_BASE_URL=https://paper-api.alpaca.markets`
2. 使用 TradingView 提醒设置里的"**Send test notification（发送测试通知）**"
   按钮（或者设置一个会针对某个已知标的立即触发的条件）。
3. 观察 Telegram 是否出现 `🚀 Signal Parsed`（见上方方法一）。
4. 检查 Alpaca 纸上交易账户 → Orders（订单）标签页，看是否有成交。
5. 在 Telegram 里运行 `/signals`，查看完整的信号记录。

---

## 需要留意的重要事项（上线实盘前请务必阅读）

1. **只支持股票 webhook。** `main.cpp:895` 构建的是股票（stock）合约，
   目前没有针对期权的 webhook 路径——期权信号只能由 Nox 内部扫描器生成。
2. **没有重复下单保护。** 一次重试或重复的提醒 = 第二笔订单。TradingView
   在超时时可能会自动重试。在纸上交易测试期间要留意是否出现重复成交。
3. **`action` 区分大小写。** 必须精确为 `BUY` 或 `SELL`；`buy`/`sell`
   会被当作 `HOLD`（默认值）处理——不会下单。
4. **存在两个交易信号来源。** Nox 自己的扫描器会一直运行。你需要事先确定
   谁才是"信号来源的唯一真相"。
5. **公网暴露。** `/webhook` 端点是可以从公网直接访问的。在投入真实资金前，
   请务必先做好加固（见下方"安全加固"一节）。

---

## 安全加固（上线实盘交易前必须完成）

- **在 Traefik 层对 TradingView 做 IP 白名单。** TradingView 会公布其
  webhook 的来源 IP（这些 IP 会变化，请查阅 TradingView 官方文档）。在
  `execution-webhook` 路由上添加一个 Traefik 的 `ipAllowList` 中间件，
  只允许这些 IP（加上你自己的 VPS/家庭 IP）向 `/webhook` 发送 POST 请求。
- **轮换 `WEBHOOK_SECRET_TOKEN`**，只要它曾经被分享过，就应该更换。
- **持续在纸上交易环境运行**，直到你观察到完整一周的提醒都能正常运作为止。

---

## 扩展 Nox 以实现更丰富的 TradingView 控制能力（可选，需要改代码）

一个干净的切入点是在现有的处理函数旁边（`execution/main.cpp:1603`）插入：

- **通过 webhook 支持期权：** 增加一个分支，当 `action` 为 `BUY_CALL` /
  `BUY_PUT` 时，构建一个 `OptionsSignal`
  （`execution/OptionsSignalTypes.hpp:14`），并通过 `OptionsOrderRouter`
  来路由，而不是走 `process()`。

---

## 快速参考

| 项目 | 值 |
|---|---|
| 端点 | `POST https://<你的主机>/webhook` |
| Content-Type | `application/json` |
| 鉴权方式 | 请求体中的 `secret_key` 字段 = `WEBHOOK_SECRET_TOKEN` |
| 处理函数 | `execution/main.cpp:1603` |
| 结构体 | `TradeSignal`，位于 `execution/main.cpp:43` |
| 路由函数 | `process()`，位于 `execution/main.cpp:539` |
| 确认信号已收到 | Telegram 的 `🚀` / `curl /recent-signals` / Telegram 的 `/signals` |
| 排查鉴权失败 | `docker compose logs execution-engine` 并 grep `WARN.*auth` |
| 纸上交易安全设置 | `.env` 中的 `ALPACA_BASE_URL=https://paper-api.alpaca.markets` |
| 临时 HTTPS 隧道 | `ngrok http 8080` → 在 TradingView 中使用生成的 ngrok 地址 |
</content>
