# Nox 交易系统——完整指南

**献给任何想彻底搞清楚这个系统到底做什么、怎么运作、以及如何使用每一项功能的人。**

不需要编程知识。如果这套系统是你自己搭建的，想从头到尾把它理解透彻；或者你要把它交接给别人接手，那这份文档就是为你准备的。

---

## 目录

1. [Nox 是什么？](#1-nox-是什么)
2. [全局视角——各部分如何互联](#2-全局视角各部分如何互联)
3. [五大服务](#3-五大服务)
4. [股票交易是如何一步步完成的](#4-股票交易是如何一步步完成的)
5. [机制状态系统——Nox 何时交易、何时停手](#5-机制状态系统nox-何时交易何时停手)
6. [仓位规模——Nox 如何决定买多少](#6-仓位规模nox-如何决定买多少)
7. [风控规则——安全闸门](#7-风控规则安全闸门)
8. [期权信号生成器](#8-期权信号生成器)
9. [个人档 vs 机器人档期权配置](#9-个人档-vs-机器人档期权配置)
10. [读懂你的 Telegram 提醒](#10-读懂你的-telegram-提醒)
11. [环境变量——控制面板](#11-环境变量控制面板)
12. [中国 A 股规则（CN-RULE）](#12-中国-a-股规则cn-rule)
13. [故障排查](#13-故障排查)
14. [速查表](#14-速查表)

---

## 1. Nox 是什么？

Nox 是一套跑在你的 VPS（云服务器）上的、自成一体的算法交易系统。它连接到你的 Alpaca 券商账户，代表你自动执行交易。

它主要做三件截然不同的事：

**1. 监控市场**——持续检查当前市场环境是否健康到足以进行交易。

**2. 执行股票交易**——当 TradingView 发来信号时，它会校验这个信号、决定该买多少股，然后向 Alpaca 下单。

**3. 生成期权信号**——完全独立于 TradingView，它每 30 分钟扫描一遍你的股票观察列表，并通过 Telegram 把期权交易点子发给你。如果你开启了自动执行，它也可以自动下这些期权单。

Nox **不会**凭空选股，也不做自己的基本面分析。它依赖的是：
- TradingView 策略提示（alert）来做股票的买卖决策
- 量化模型（Black-Scholes 定价模型、凯利公式）用于期权定价和仓位规模计算
- 宏观数据（VIX、SPY 走势）用于判断市场机制状态（regime）

把它想象成一个严格执行你所设定规则的执行层，而不是一个选股大师。

---

## 2. 全局视角——各部分如何互联

下面是信息在整个系统中流转的完整路径：

```
┌─────────────────────────────────────────────────────────────┐
│                        EXTERNAL                              │
│                                                              │
│   TradingView Strategy ──────────────────────────────────┐  │
│   (Pine Script alerts on 4H chart)                       │  │
│                                                          │  │
│   Yahoo Finance (VIX, SPY data) ──────┐                  │  │
│   Alpaca Market Data (options chain) ─┤                  │  │
└───────────────────────────────────────┼──────────────────┼──┘
                                        │                  │
                    ┌───────────────────▼──────────────────▼──────────────────────┐
                    │                   YOUR VPS (Docker)                          │
                    │                                                              │
                    │  ┌─────────────┐    ┌──────────────────────────────────┐   │
                    │  │   Analyst   │───▶│        Execution Engine           │   │
                    │  │   Brain     │    │                                   │   │
                    │  │  (C++)      │    │  • Validates webhook signals      │   │
                    │  │             │    │  • Enforces all risk gates        │   │
                    │  │  • VIX      │    │  • Sizes positions (Kelly)        │   │
                    │  │  • SPY SMA  │    │  • Routes to Alpaca               │   │
                    │  │  • Regime   │    │  • Options signal generator       │◀──┘
                    │  └─────────────┘    │    (BOT profile + PERSONAL)       │
                    │                     └──────────────┬───────────────────-─┘
                    │  ┌─────────────┐                   │
                    │  │  Heartbeat  │                   │
                    │  │  Monitor    │                   │
                    │  │  (Python)   │                   │
                    │  │             │                   │
                    │  │  • Health   │                   │
                    │  │  • Reports  │                   │
                    │  └─────────────┘                   │
                    │                                     │
                    │  ┌──────────────┐  ┌─────────────┐ │
                    │  │ America Data │  │ China Data  │ │
                    │  │   Engine     │  │   Engine    │ │
                    │  │  (Python)    │  │  (Python)   │ │
                    │  └──────────────┘  └─────────────┘ │
                    └─────────────────────────────────────┘
                                        │
                    ┌───────────────────▼─────────────────────┐
                    │               OUTPUT                     │
                    │                                          │
                    │   Alpaca Account (orders placed here)    │
                    │   Your Telegram (all alerts sent here)   │
                    └──────────────────────────────────────────┘
```

每个服务都跑在独立的 Docker 容器里，彼此通过内部网络（`nox_net`）通信——除了 TradingView 需要访问的 webhook 端点，内部的一切都不对公网暴露。

---

## 3. 五大服务

### 3.1 执行引擎（Execution Engine）

**整个系统的大脑。** 这是一个 24/7 常驻运行的 C++ 程序，负责：

- 在 8080 端口监听来自 TradingView 的交易信号
- 通过一系列安全闸门校验每一个信号
- 使用凯利公式（Kelly Criterion）计算仓位规模
- 向 Alpaca 提交买单和卖单
- 在后台定时器上运行期权信号生成器
- 把所有交易确认和提醒发送到你的 Telegram

只要你看到任何一笔交易发生了，都是这个服务干的。

**关键端点：** `POST /webhook`——这是 TradingView 发送提示（alert）的 URL。
**期权端点：** `POST /options/price`——你可以直接查询 Black-Scholes 定价结果。
**健康检查：** `GET /health`——如果服务正常运行，会返回 `{"status": "healthy"}`。

---

### 3.2 分析师大脑（Analyst Brain）

**市场观察员。** 每 6 小时运行一次（可配置）。它会：

1. 从 Yahoo Finance 获取当前的 VIX（恐慌指数）
2. 从 Yahoo Finance 获取 SPY 的价格，并计算其 200 日移动均线
3. 把这些数值输入到机制状态机（Regime State Machine）
4. 通过 `/webhook` 把机制分类结果发给执行引擎
5. 通过 Telegram 提醒你当前的市场状态

这个服务**不会**为股票生成买卖信号。它只是告诉系统「现在市场安全，可以交易」或者「停止交易」。买卖决策来自 TradingView。

---

### 3.3 心跳监控（Heartbeat Monitor）

**看门狗。** 一个 Python 服务，负责：

- 监控其他所有服务是否存活
- 向 Telegram 发送健康状态报告
- 使用 Claude AI API 运行智能分析报告
- 检查 SEC 备案雷达，关注你持仓相关的新闻
- 一旦出现异常就提醒你

你不需要直接和这个服务交互。它在后台安静运行，出问题时会呼叫你。

---

### 3.4 美股数据引擎（America Data Engine）

**美股行情数据缓存。** 连接 Alpaca 的行情数据 API，缓存股票价格及相关数据。供其他服务内部调用，避免各服务重复发起 API 请求。

---

### 3.5 中国市场数据引擎（China Data Engine）

**中国市场数据缓存。** 以 15 分钟为周期抓取东方财富（East Money）、财联社（Cailian Press）、国家统计局 PMI（中国经济活动指数）和央行 LPR（中国基准利率）数据。如果你交易中国 A 股，这个服务会派上用场。仅在内部运行，不对公网暴露。

---

## 4. 股票交易是如何一步步完成的

如果你想搞清楚 Nox 何时交易、为什么交易，这一节是最重要的。

### 第一步——TradingView 触发提示

你在 TradingView 上跑着一套策略（OpenClaw 加权 Alpha 策略，基于 4 小时图）。当策略的触发条件满足时——具体是 EMA(9) 上穿/下穿 EMA(21)，并配合成交量过滤——TradingView 就会触发一个 webhook 提示（alert）。

这个提示是一条发送到你服务器的 JSON 消息，长这样：

```json
{
  "secret_key": "your_secret",
  "ticker": "SPY",
  "action": "BUY",
  "price": 580.50,
  "rsi": 54.2,
  "atr": 5.31,
  "vol": 45000000,
  "stop_loss_atr_multiplier": 2.0,
  "risk_tier": 0,
  "vix": 16.4,
  "spy_price": 580.50,
  "spy_200_sma": 565.00
}
```

### 第二步——Schema 校验闸门

执行引擎收到这段 JSON 后，会立即检查它是否是合法、格式良好的 JSON。如果内容是垃圾数据或格式错误，它会返回 HTTP 400 并忽略这条信号。任何业务逻辑都不会在坏数据上运行。

### 第三步——鉴权闸门

系统会检查 JSON 里的 `secret_key` 字段是否与环境变量中的 `WEBHOOK_SECRET_TOKEN` 相匹配。如果不匹配，这个信号会被**静默丢弃**——不返回任何错误响应，也不发 Telegram 提醒。这是故意设计的：如果有人在探测你的端点，你不会想告诉对方「密码错了」。

### 第四步——RSI 闸门

对于 BUY（买入）信号：如果 RSI 低于 30，交易会被拦截。RSI 低于 30 意味着资产处于极度超卖状态——此时买入有接飞刀的风险。

对于 SELL（卖出）信号：如果 RSI 高于 70，交易会被拦截。RSI 高于 70 意味着资产处于极度超买状态——此刻卖出可能还为时过早。

如果被拦截，你会收到一条 `🚧 RSI GATE BLOCK` 的 Telegram 消息。

### 第五步——机制状态闸门

执行引擎会用信号中随附的 VIX 和 SPY 数值（而不是缓存的旧数据）重新计算一遍机制状态。结果决定：

- **RISK_ON（风险偏好开启）**：交易正常进行，使用全部资金
- **TRANSITION（过渡期）**：交易照常进行，但仓位规模减半
- **RISK_OFF（风险规避）**：交易被强制拦截，不下任何单

关于机制状态的更多细节见第 5 节。

### 第六步——实时账户净值获取

在计算仓位之前，引擎会先从 Alpaca 获取你的实时账户净值。它会以指数退避的方式（2 秒 → 4 秒 → 8 秒）最多尝试 3 次。如果 3 次都失败，交易会被取消。这条规则存在的原因是：如果不知道你的账户净值就去计算仓位规模，可能会导致下单违反你的风控上限。

### 第七步——仓位规模计算

引擎会计算应该买入多少股。计算方法取决于信号中的 `risk_tier`（风险档位）字段：

- **Tier 0（默认）**：凯利公式——基于胜率和盈亏比的数学公式
- **Tier 1**：不管凯利公式怎么算，固定用组合净值的 1%
- **Tier 3**：组合净值的 5%，并配合更宽的止损（3.5 倍 ATR）

关于仓位规模的更多细节见第 6 节。

### 第八步——名义金额闸门

计算出股数之后，引擎会检查这笔交易的总金额是否超过组合净值的 10%。这是一道兜底的物理检查，以防之前的环节出现漏检。如果超过 10%，订单会被拦截，你会收到一条 `🚨 RULE-018` 提醒。

### 第九步——提交订单

一笔市价买单会被发送到 Alpaca。与此同时，系统会在入场价下方 `ATR × stop_loss_atr_multiplier` 的距离处放置一笔移动止损单（trailing stop）。这个移动止损单在价格上涨时锁定利润，在价格下跌时限制亏损。

### 第十步——确认

你会收到一条 Telegram 消息，确认订单和止损单已成功下达。

---

## 5. 机制状态系统——Nox 何时交易、何时停手

机制状态系统是最重要的安全机制。它回答的问题是：**「当前的市场环境是否足够安全，可以承担新的风险敞口？」**

### 三种机制状态

```
VIX < 35 AND SPY > 200-day SMA
           ↓
        RISK_ON ✅
        Full capital deployed
        Stop loss: 2× ATR

VIX < 35 AND SPY between SMA×0.98 and SMA
           ↓
       TRANSITION 🟡
        Half capital deployed
        Stop loss: 1.5× ATR

VIX ≥ 35 OR SPY < SMA × 0.98
           ↓
       RISK_OFF 🔴
        Zero new entries
        Stop loss: 1.0× ATR
```

### VIX——恐慌指数

VIX 衡量的是期权市场对未来 30 天标普 500 波动幅度的预期。本质上是市场恐慌与不确定性的量表：

- **VIX < 15**：市场非常平静，所有人都很有信心。
- **VIX 15–25**：正常市场状态。
- **VIX 25–35**：市场担忧情绪升高，有些事情让投资者感到不安。
- **VIX ≥ 35**：危机水平。这个水平会在 Nox 中触发 RISK_OFF。

历史上 VIX 曾飙升至 35 以上的案例：2020 年新冠疫情崩盘、2022 年加息恐慌、2025 年关税冲击。

### SPY 与 200 日均线

SPY 是一支追踪标普 500 指数（美国市值最大的 500 家公司）的 ETF（交易所交易基金）。200 日简单移动均线（SMA）是过去 200 个交易日（约 10 个月）收盘价的平均值。

当 SPY 高于其 200 日均线时，大盘处于上升趋势中；一旦跌破，说明情况发生了变化。这个 0.98 的缓冲区（低于均线 2%）可以防止系统因为一次微小的波动就翻转进入 RISK_OFF 状态。

### 这跟你有什么关系？

如果你没看到任何交易发生，而你觉得市场看起来一切正常，那就去检查一下当前的机制状态。一次你可能已经忘记的 VIX 飙升，或许已经把系统锁定在了 RISK_OFF 状态。分析师每 6 小时会发一次机制状态更新，所以你随时都能了解当前的状态。

---

## 6. 仓位规模——Nox 如何决定买多少

### 凯利公式（Tier 0——默认档）

凯利公式（Kelly Criterion）是一个数学公式，回答的问题是：「根据我历史上的胜率和平均盈亏规模，我应该拿出多大比例的本金去下注？」

公式为：`K% = 胜率 − ((1 − 胜率) / 盈亏比)`

以 Nox 配置的参数为例（胜率 = 68.42%，盈亏比 = 2.316）：
- 原始凯利值 = 68.42% − (31.58% / 2.316) = **54.8%**

这个数字非常激进——原始凯利本身就很冒险。所以 Nox 应用了一个 **0.15（15%）的凯利系数**：
- 调整后凯利值 = 54.8% × 0.15 = **组合净值的 8.2%**（每笔交易）

这个数字低于 10% 的硬性上限，也就是说凯利公式确实在真正发挥作用——它不会在每一笔交易上都被硬顶截断。

**硬性上限：** 无论凯利公式算出多少，系统在单笔交易上承担的风险绝不会超过组合净值的 **10%**。这是代码里的一道物理性闸门，而不只是一条准则。

**零股保护：** 如果凯利公式分配的金额买不到一整股（比如你有 1000 美元，某只股票要价 350 美元——凯利只给你分配了 82 美元，买不了一股），交易会被直接取消。硬性凑单会导致把账户 35% 的资金压在单笔交易上，这完全违背了 10% 上限的初衷。

### Tier 1——标准档（1%）

很简单：固定拿组合净值的 1% 去冒险。适用于信心不那么强的信号，或者你想要更可预测的仓位规模时。

举例：10,000 美元的组合 → 每笔交易 100 美元 → 如果 SPY 是 580 美元，那就是 0 股（向下取整）。这个档位其实只适合较大的账户。

### Tier 3——激进档（5%）

拿 5% 去冒险，同时配合更宽的止损（3.5 倍 ATR，而不是 2 倍）。用于高信心的交易机会，预期会有更大的行情波动。这个档位被称为「让刀多切一会儿」——给交易更多的呼吸空间。

### 机制状态调整

机制状态乘数（1.0 / 0.5 / 0.0）会在仓位计算之前先行应用。在 TRANSITION（过渡期）状态下，你的有效净值在凯利公式运行前就已经减半，所以计算出的仓位规模自然会更小。

---

## 7. 风控规则——安全闸门

这些规则都在代码层面强制执行，任何信号都无法绕过它们。

| 规则 | 作用 |
|------|-------------|
| **RULE-004**（鉴权闸门） | 密钥不匹配的信号被静默丢弃 |
| **RULE-005**（凯利守卫） | 负凯利值 = 无优势 = 交易取消。零股 = 交易取消 |
| **RULE-007**（Telegram 必需） | 缺少 Telegram 凭证时，机器人拒绝启动 |
| **RULE-008**（超时控制） | 所有 Alpaca API 调用严格限制 5 秒连接超时 / 10 秒读取超时 |
| **RULE-009**（启动校验） | 启动时检查所有必需的环境变量，缺失则拒绝启动 |
| **RULE-013**（双重可观测性） | 系统若无法向你发出提醒，就不能执行动作 |
| **RULE-014**（禁止硬编码 URL） | 实盘与模拟 API 始终通过环境变量设置，绝不写死在代码里 |
| **RULE-018**（名义金额上限） | 订单提交时的金额不得超过组合净值的 10% |
| **CN-RULE-001**（整手交易） | 中国 A 股必须以 100 的整数倍（一手）买入 |
| **CN-RULE-002**（T+1） | 中国 A 股当日买入不能当日卖出 |

这些规则都不是随意设定的——每一条都对应着一个真实存在的场景：如果没有这条规则，系统就可能亏钱或行为异常。代码中的注释解释了每一条规则的由来。

### 移动止损

每一笔 BUY 订单都会配对一笔 Alpaca 上的移动止损单（trailing stop）。止损会以 `ATR × 乘数` 美元的距离跟随价格移动：

- ATR（平均真实波幅）= 过去 14 天的平均日内价格波动区间，用来衡量该股票当前的波动性。
- 如果 ATR = 5 美元，乘数 = 2.0，止损会跟随在最高价下方 10 点的位置。

也就是说，如果你以 100 美元买入，股价涨到 120 美元，此时你的止损已经上移到 110 美元。它会在你什么都不做的情况下自动锁定利润。

---

## 8. 期权信号生成器

这是一个自成一体的系统，跑在执行引擎内部的后台定时器上。与股票交易不同，**它不需要 TradingView**。它每 30 分钟自主扫描一次市场，生成自己的信号。

### 它做什么

对观察列表中的每一支股票，每个扫描周期都会：

1. **从 Yahoo Finance 获取历史价格**，并计算：
   - RSI（14 日）——动量指标
   - ATR（14 日）——波动性衡量指标
   - 20 日移动均线——短期趋势
   - 50 日移动均线——中期趋势

2. **判断方向性偏向：**
   - *看涨（Bullish）*：RSI 处于 40–65，价格高于两条均线
   - *看跌（Bearish）*：RSI 处于 35–60，价格低于两条均线
   - *中性（Neutral）*：介于两者之间的一切情况

3. **从 Alpaca 的期权链获取 IV Rank（隐含波动率百分位）。** IV Rank 告诉你现在期权是便宜还是贵：
   - **IV Rank 低于阈值** = 期权便宜 → 适合*买入*权利金（看涨、看跌期权）
   - **IV Rank 高于阈值** = 期权昂贵 → 适合*卖出*权利金（备兑看涨、现金担保看跌）

4. **根据方向偏向与隐含波动率环境的组合，选出最合适的策略。**

5. **使用 Black-Scholes 公式定价合约**，并在 Alpaca 找到最接近目标的真实合约代码。

6. **通过 Telegram 向你发送完整的交易点子提醒。**

7. **如果设置了 `OPTIONS_BOT_AUTO_EXECUTE=true`，则自动下单。**

### 期权策略一览

| 策略 | 通俗解释 | 适用场景 |
|----------|--------------|-----------------|
| **Long Call（买入看涨期权）** | 押注股价上涨。支付固定成本，价格涨破行权价后获利。 | 看涨 + 波动率便宜 |
| **Long Put（买入看跌期权）** | 押注股价下跌。支付固定成本，价格跌破行权价后获利。 | 看跌 + 波动率便宜 |
| **Bull Call Spread（牛市看涨价差）** | 押注股价上涨但限制成本。买入一个看涨期权，卖出一个更高行权价的看涨期权。最大风险和最大收益都被封顶。 | 看涨 + 任意波动率 |
| **Bear Put Spread（熊市看跌价差）** | 押注股价下跌但限制成本。买入一个看跌期权，卖出一个更低行权价的看跌期权。 | 看跌 + 任意波动率 |
| **Cash-Secured Put（现金担保看跌期权，CSP）** | 收取权利金，承诺以更低的价格买入股票。只要股价保持在行权价以上，权利金就归你所有。 | 看涨/中性 + 波动率昂贵 |
| **Covered Call（备兑看涨期权，CC）** | 收取权利金，承诺以更高的价格卖出你已持有的股票。无论最终是否行权，权利金都归你。 | 中性/看跌 + 波动率昂贵 |
| **Long Straddle（跨式组合）** | 押注股价会有大波动，但不确定方向。同一行权价同时买入看涨和看跌期权。 | 中性 + 波动率极其便宜 |
| **Long Strangle（宽跨式组合）** | 与跨式组合类似但更便宜——买入虚值看涨和虚值看跌期权。需要更大的波动才能获利。 | 中性 + 波动率昂贵、期待突破行情 |

### Black-Scholes 与希腊字母

期权定价引擎使用 Black-Scholes 模型——业内标准的期权定价数学公式。它会计算「希腊字母」，用来衡量期权价格预期会如何变化：

| 希腊字母 | 衡量的内容 | 举例 |
|-------|-----------------|---------|
| **Delta（Δ）** | 标的股价每变动 1 美元，期权价格随之变动的幅度 | Delta 为 0.45 → 股价上涨 1 美元，期权价格约上涨 0.45 美元 |
| **Gamma（Γ）** | Delta 本身变化的速度 | Gamma 高 = Delta 变化很快 |
| **Theta（Θ）** | 每日时间损耗——单纯因为时间流逝，期权每天损失多少价值 | Theta 为 -0.05 美元 → 期权每个日历日损失 0.05 美元 |
| **Vega（V）** | 对隐含波动率变化的敏感度 | 正 Vega = 波动率上升时受益 |
| **Rho（ρ）** | 对利率变化的敏感度 | 对短期期权来说不那么重要 |

**对初学者来说，最重要的是 Theta。** 每过一天，你持有的看涨/看跌期权都会因为时间流逝而损失一小部分价值。这就是为什么期权有到期日、为什么择时如此重要。

### IV Rank——关键过滤指标

IV Rank（隐含波动率百分位）告诉你*当下*的期权价格相对过去一年是便宜还是昂贵：

```
IV Rank = (Current IV − 52-week Low IV) / (52-week High IV − 52-week Low IV) × 100
```

- **IV Rank 为 0**：期权处于过去一年中最便宜的水平，适合买入权利金。
- **IV Rank 为 100**：期权处于过去一年中最昂贵的水平，适合卖出权利金。
- **IV Rank 为 50**：正好处于中间——中性状态。

经验法则是：**「低 IV 时买入，高 IV 时卖出。」** 你想在期权便宜的时候买入，在期权昂贵的时候卖出。IV Rank 就是那个指南针。

---

## 9. 个人档 vs 机器人档期权配置

这是整个系统中最重要的设计决策之一。**你自己**能承受的风险容忍度，要高于你希望自动化机器人所承担的风险。因此系统会同时运行两套完全独立的期权信号生成器。

### 为什么要分开？

机器人是自动用真金白银交易的。如果它误判了一个激进信号，你什么都没做就会亏钱。所以它的设计理念是保守为先。

而你自己，会审视信号之后再决定是否行动。因为循环中有人工过滤这一环，你能承受更高的风险。

### 两套配置档并列对比

| 参数 | BOT（机器人档，保守） | PERSONAL（个人档，激进） |
|-----------|-------------------|----------------------|
| **Delta 目标值** | 0.45（接近平值 ATM） | **0.60（价内 ITM）**——更多内在价值，更少 theta 损耗 |
| **到期天数——单腿** | 45 天 | **14 天**——短伽马博弈，更快见分晓 |
| **到期天数——价差组合** | 45 天 | **21 天** |
| **IV Rank 买入阈值** | < 30% | **< 50%**——愿意在波动率适度昂贵时也买入 |
| **IV Rank 卖出阈值** | > 50% | **> 40%** |
| **单笔风险占比** | 1.0–2.0% | **2.0–3.0%** |
| **策略限制** | 按资金规模分档 | **全部策略始终可用** |
| **RISK_OFF 状态下的行为** | 硬性拦截买入权利金操作 | **显示 50% 置信度警告——从不拦截你** |
| **自动执行** | 可配置 | **始终仅作参考——从不自动下单** |
| **默认观察列表** | SPY, QQQ, AAPL, TSLA, NVDA | **SPY, QQQ, AAPL, TSLA, NVDA, AMZN, META** |

### 如何在 Telegram 中区分二者

每条提醒的标题都会标明所属配置档：

```
📊 OPTIONS SIGNAL — AAPL [PERSONAL · FREE_CAPITAL]   ← 这是你的个人档信号
📊 OPTIONS SIGNAL — AAPL [BOT · STANDARD]            ← 这是机器人的保守档信号
```

### 资金分档（仅机器人档适用——个人档不受此限制）

| 档位 | 资金规模 | 可用策略 |
|------|---------|---------------------|
| **STARTER（起步档）** | 低于 5000 美元 | 仅限单纯买入看涨/看跌期权 |
| **STANDARD（标准档）** | 5000 – 30000 美元 | 增加现金担保看跌、备兑看涨 |
| **ADVANCED（进阶档）** | 30000 – 75000 美元 | 增加价差组合、跨式组合、宽跨式组合 |
| **FREE_CAPITAL（自定义资金档）** | 75000 美元以上，或自定义金额 | 全部策略均可用，计算基于你指定的资金额度 |

**自定义资金模式：** 如果你有一笔独立的资金池，专门想用来交易期权（比如另一个账户里的 1 万美元），可以在 `.env` 中设置 `OPTIONS_PERSONAL_FREE_CAPITAL_AMOUNT=10000`。信号计算会使用这个数字，而不是机器人在 Alpaca 账户里的余额。这样无论机器人账户里有多少钱，你都能得到规模计算正确的个人档信号。

---

## 10. 读懂你的 Telegram 提醒

每一个重要事件都会给你发一条消息。下面解释这些消息都是什么意思。

### 股票交易提醒

**🟢 BUY ORDER EXECUTED（买单已执行）**
```
🟢 BUY ORDER EXECUTED
────────────────────────
• Ticker: SPY
• Quantity: 12 Shares (Dynamic Kelly)
• Order ID: abc123-def456
```
一笔买单已成功提交。订单 ID 可以让你在 Alpaca 中查询该订单。

---

**⚪ POSITION CLOSED（仓位已平仓）**
```
⚪ POSITION CLOSED
────────────────────────
• Ticker: SPY
• Trigger: Webhook SELL Signal
• Alpaca Order ID: xyz789
```
一个卖出信号到达，仓位已被平掉。这可能来自 TradingView 的信号，也可能是移动止损被触发。

---

**📊 机制状态检查**
```
📊 Regime Check: STATUS: RISK-ON. Volatility low. Deploying full capital.
```
分析师刚刚完成了一次市场评估。每 6 小时发送一次。

---

**🚧 RSI GATE BLOCK（RSI 闸门拦截）**
```
🚧 RSI GATE BLOCK
────────────────────────
• Ticker: SPY
• Action: BUY
• RSI: 28.3 (Below Floor < 30)
⚠️ Order canceled to protect buying power.
```
一个买入信号到达，但 RSI 过低。交易已被取消。这正是系统正常运作的表现——它在保护你，避免你在动能崩盘时买入。

---

**🛑 REGIME BLOCK: RISK-OFF（机制状态拦截：风险规避）**
```
🛑 REGIME BLOCK: RISK-OFF
────────────────────────
• Ticker: SPY
⛔ VIX ≥ 30 or SPY below 200 SMA. No new entries.
```
宏观环境对新交易来说过于危险。在这个状态解除之前，不会有任何新资金被投入。

---

**🚨 CRITICAL: Equity Fetch Failed（严重：账户净值获取失败）**
```
🚨 CRITICAL: Equity Fetch Failed
────────────────────────
All 3 Alpaca equity fetch attempts failed.
⛔ New order entries halted for this cycle.
Manual review required.
```
系统无法连接 Alpaca 查询你的账户余额。这可能是网络问题、Alpaca API 中断，或者你的 API 密钥已过期。请检查 Alpaca 的状态页面以及你的日志。

---

### 期权信号提醒

**📊 期权信号（仅供参考）**
```
📊 OPTIONS SIGNAL — AAPL [PERSONAL · FREE_CAPITAL]
────────────────────────────────────
🎯 Strategy: Long Call
📅 Expiry: 2026-08-01 (39 DTE)
💵 Strike: $200
💰 Entry: $4.20 | Max Risk: $420 | Max Gain: Unlimited

📐 Greeks
• Delta: 0.60 | Gamma: 0.021
• Theta: -$0.09/day | Vega: +0.38
• IV Rank: 22% ← LOW (buy premium zone ✅)

📈 Technicals — AAPL
• RSI(14): 57 | ATR(14): $2.80
• Price vs 20-SMA: ✅ above | vs 50-SMA: ✅ above

🌐 Macro Regime: RISK_ON ✅ (VIX 16.4, SPY > 200-SMA)
🎯 Signal Confidence: 87%

⚠️ Advisory only — manual execution required.
```

解读这条提醒：
- **Strategy（策略）**：Long Call——看涨方向性押注
- **Expiry（到期日）**：2026 年 8 月 1 日，距今 39 天
- **Strike（行权价）**：200 美元——只要 AAPL 到期时收盘价高于 204.20 美元（行权价 + 已付权利金），你就能获利
- **Max Risk（最大风险）**：420 美元——这是你能损失的最大金额（对买入的单纯期权来说，付出的权利金就是你的全部风险）
- **Delta 0.60**：如果 AAPL 上涨 1 美元，这份期权大约获利 0.60 美元
- **Theta -0.09 美元/天**：单纯因为时间流逝，这份期权每天大约损失 0.09 美元
- **IV Rank 22%**：期权处于便宜区间，是买入的好时机
- **Confidence 87%（置信度 87%）**：机制状态有利，技术面也保持一致

---

**✅ OPTIONS ORDER PLACED（期权订单已提交）**（仅在开启自动执行时出现）
```
✅ OPTIONS ORDER PLACED
────────────────────────
• Ticker: AAPL
• Strategy: LONG_CALL
• Contracts: 1
• Expiry: 2026-08-01
• Order ID: ord-abc123
```
机器人已在 Alpaca 中实际提交了这笔订单。去查看你的持仓吧。

---

**🚨 OPTIONS ORDER FAILED（期权订单失败）**
```
🚨 OPTIONS ORDER FAILED
────────────────────────
• Ticker: AAPL
• Strategy: LONG_CALL
• Reason: Contract lookup failed for AAPL — HTTP 403
```
订单曾尝试提交，但被 Alpaca 拒绝。常见原因：你的账户没有开通期权交易权限；模拟账户没有单独启用期权访问权限；或者该特定合约不可用（行权价过于虚值、已到期、被停牌）。

---

**🚫 CN T+1 GATE BLOCKED（中国 A 股 T+1 闸门拦截）**
```
🚫 CN T+1 GATE BLOCKED
────────────────────────
• Ticker: 600519.SH
• Entry Date: 2026-06-23
• Sell Date: 2026-06-23
⛔ Same-day sell prohibited (T+1 rule). Signal discarded.
```
你今天买入了一支中国 A 股，同一天又来了一个卖出信号。中国证券交易所规则禁止当日回转交易。明天才能卖出。

---

## 11. 环境变量——控制面板

所有配置都保存在你的 `.env` 文件中。这是控制 Nox 行为的总控制面板。**绝不要把这个文件提交到 git。**

### 必需项——缺失则机器人无法启动

| 变量 | 作用 |
|----------|-------------|
| `ALPACA_API_KEY` | 你的 Alpaca API 密钥 ID |
| `ALPACA_SECRET_KEY` | 你的 Alpaca API 密钥 |
| `ALPACA_BASE_URL` | 模拟盘用 `https://paper-api.alpaca.markets`，实盘用 `https://api.alpaca.markets` |
| `WEBHOOK_SECRET_TOKEN` | TradingView 在其 webhook 载荷中携带的密码 |
| `TELEGRAM_BOT_TOKEN` | 你的 Telegram 机器人的令牌 |
| `TELEGRAM_CHAT_ID` | 你的 Telegram 聊天 ID（提醒会发到这里） |
| `KELLY_WIN_RATE` | 你策略的历史胜率（例如 `0.6842` = 68.42%） |
| `KELLY_WIN_LOSS_RATIO` | 平均盈利 ÷ 平均亏损（例如 `2.316`） |
| `KELLY_FRACTION` | 施加在原始凯利值上的缩放系数（例如 `0.15` = 原始凯利值的 15%） |
| `ANTHROPIC_API_KEY` | Claude AI API 密钥（用于心跳服务生成智能分析报告） |

### 可选项——股票交易

| 变量 | 默认值 | 作用 |
|----------|---------|-------------|
| `ANALYST_CYCLE_HOURS` | `6` | 分析师检查 VIX 和机制状态的频率 |
| `CN_BOARD_LOT_SIZE` | `100` | 中国 A 股订单的整手股数 |
| `CN_POSITIONS_PATH` | `/tmp/china_positions.json` | T+1 持仓记录的保存路径 |

### 可选项——期权信号

| 变量 | 默认值 | 作用 |
|----------|---------|-------------|
| `OPTIONS_BOT_WATCHLIST` | `SPY,QQQ,AAPL,TSLA,NVDA` | BOT 档扫描的股票代码 |
| `OPTIONS_BOT_SCAN_INTERVAL_MINUTES` | `30` | BOT 档的扫描频率 |
| `OPTIONS_BOT_AUTO_EXECUTE` | `false` | 设为 `true` 以自动下达机器人档期权订单 |
| `OPTIONS_BOT_QTY_CONTRACTS` | `1` | 机器人档每笔订单的合约数量 |
| `OPTIONS_BOT_FREE_CAPITAL_AMOUNT` | _（关闭）_ | 覆盖机器人档使用的资金额度 |
| `OPTIONS_PERSONAL_WATCHLIST` | `SPY,QQQ,AAPL,TSLA,NVDA,AMZN,META` | PERSONAL 档扫描的股票代码 |
| `OPTIONS_PERSONAL_SCAN_INTERVAL_MINUTES` | `30` | PERSONAL 档的扫描频率 |
| `OPTIONS_PERSONAL_FREE_CAPITAL_AMOUNT` | _（关闭）_ | 用于个人档仓位计算的资金额度（例如 `5000`） |
| `OPTIONS_PERSONAL_QTY_CONTRACTS` | `1` | 个人档信号计算所用的合约数量 |

### 在模拟盘与实盘之间切换

**模拟交易**（安全，虚拟资金）：
```
ALPACA_BASE_URL=https://paper-api.alpaca.markets
```

**实盘交易**（真实资金）：
```
ALPACA_BASE_URL=https://api.alpaca.markets
```

任何新策略都要先在模拟盘环境下测试。模拟盘 API 的行为与实盘完全一致，唯一的区别是资金不是真的。

---

## 12. 中国 A 股规则（CN-RULE）

如果你交易中国股票（上海或深圳交易所），有两条规则会自动生效。

### CN-RULE-001——整手交易

中国交易所要求订单必须是 100 股的整数倍（一「手」）。你不能买 150 股，必须买 100 股或 200 股。Nox 会自动截断：如果凯利公式算出应买 145 股，实际下单会是 100 股。

如果凯利公式算出应买 60 股（不足一手），交易会被直接取消——反正在交易所也无法执行。

### CN-RULE-002——T+1 结算

中国的监管规定禁止在买入的当天卖出该股票。「T」指交易日，你必须等到 T+1（下一个交易日）或之后才能卖出。

Nox 会为每一笔 A 股买入记录时间戳。如果卖出信号在与买入记录同一天到达，卖出会被拒绝。持仓记录会持久化到磁盘，即便服务器重启也能保留。每天启动时，系统会清理前几日的记录（因为限制已经解除）。

如果你在买入之后重启了引擎（导致记录丢失），系统会记录一条警告并允许卖出，而不是无限期锁死你的仓位——这是两种失败模式中更安全的一种。

---

## 13. 故障排查

### 「我没有收到任何交易信号」

按以下顺序逐项排查：

**1. 检查 TradingView 提示是否处于激活状态**
打开 TradingView → 你的图表 → 提示（Alerts，铃铛图标）。找到你的 webhook 提示，应该显示绿色的激活标志。如果显示灰色、已过期或者根本找不到，请重新创建。

**2. 检查 webhook URL**
提示的 webhook URL 应该是 `https://yourdomain.com/webhook`（或者你服务器的 IP）。如果你的 VPS IP 变了，这个 URL 就指向了一个不存在的地方。

**3. 检查引擎是否在运行**
在你的 VPS 上执行：`docker-compose ps`。你应该能看到 `nox_execution` 的状态是 `Up`。如果显示 `Exit` 或找不到该服务，运行 `docker-compose up -d execution-engine`。

**4. 检查当前的机制状态**
查看你最近一次收到的 Telegram 分析师报告。如果显示 RISK_OFF，所有新的买入都会被拦截——这是正常行为。去查一下新闻，看看是什么吓到了市场。

**5. 检查 Pine Script 是否在生成信号**
打开 TradingView，看看你的 4 小时图。最近的 K 线上有没有买/卖三角标记？如果几周都没有，说明 EMA 交叉的条件一直没有满足。策略在等待合适的市场结构出现。

**6. 检查你的日志**
在你的 VPS 上执行：`docker-compose logs -f execution-engine`。查找是否有 WARN 或 ERROR 级别的日志行。

---

### 「期权信号停了」

期权扫描器跑在执行引擎内部的一个后台线程上。只要执行引擎在运行，它就应该在扫描。

检查方式：`docker-compose logs execution-engine | grep OPTIONS_SCAN`

你应该每 30 分钟看到类似 `[INFO] [OPTIONS_SCAN][PERSONAL] Tier=STARTER | Capital=...` 这样的日志行。

如果看到 `Skipping scan — equity unavailable`，说明 Alpaca 没有响应（和上面提到的账户净值获取失败是同一类问题）。

---

### 「期权订单失败，报 HTTP 403」

你的 Alpaca 账户需要显式开通期权交易权限。登录 Alpaca → 账户设置 → 交易 → 期权交易，把它启用。模拟账户可能也需要单独开通期权权限。

---

### 「T+1 闸门一直拦截我的卖单」

这个只会影响中国 A 股。这是正常工作的表现——你正在试图当天卖出当天买入的股票。等到明天再说。如果你确定今天没有买入，但仍然被拦截，可能是持久化文件中存在过期记录。检查 `CN_POSITIONS_PATH` 指定的文件（默认是 `/tmp/china_positions.json`），如果需要的话把过期条目删掉。

---

### 「凯利公式一直算出 0 股」

有两种情况：
1. **胜率和盈亏比算出的是负凯利值**：说明你的策略没有数学上的优势。交易被正确取消了。这正是 RULE-005 在发挥作用。请检查你的凯利参数。
2. **凯利股数本身有效，但向下取整后变成了 0**：相对于该股票的价格，你的账户规模太小，即便按 1% 分配也买不到一整股。你需要更多资金，或者交易更便宜的股票。

---

### 「Telegram 提醒停了」

1. 检查你的机器人令牌是否被吊销：在 Telegram 上给 `@BotFather` 发消息 → `/mybots` → 检查状态
2. 检查 `TELEGRAM_CHAT_ID` 是不是你的个人聊天 ID，而不是频道 ID（两者的格式不同）
3. 检查网络连通性：`docker-compose logs execution-engine | grep TELEGRAM`
4. 尝试手动发送一条测试消息：`docker-compose exec execution-engine curl -s "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/getMe"`

---

### 「如何从模拟盘切换到实盘交易？」

1. 打开你的 `.env` 文件
2. 把 `ALPACA_BASE_URL=https://paper-api.alpaca.markets` 改成 `ALPACA_BASE_URL=https://api.alpaca.markets`
3. 重启：`docker-compose down && docker-compose up -d`
4. 确认下一次 Telegram 机制状态报告能够确认引擎已经在实盘运行

**在这么做之前：** 请确认你已经连续观察到至少 30 天稳定的模拟盘交易结果，并且已经理解第 10 节中的每一种提醒类型。

---

## 14. 速查表

### 服务命令

```bash
# Start everything
docker-compose up -d

# Stop everything
docker-compose down

# Check all service statuses
docker-compose ps

# Watch live execution engine logs
docker-compose logs -f execution-engine

# Watch live analyst logs
docker-compose logs -f analyst-brain

# Restart just the execution engine
docker-compose restart execution-engine

# Rebuild and restart after code changes
docker-compose up -d --build execution-engine
```

### 关键 URL（请替换为你自己的服务器地址）

| URL | 作用 |
|-----|-------------|
| `GET https://yourserver.com/health` | 检查执行引擎是否在运行 |
| `GET https://yourserver.com/last-report` | 分析师最近一次报告是什么时候？ |
| `POST https://yourserver.com/webhook` | TradingView 发送交易信号的地址 |
| `POST https://yourserver.com/options/price` | 直接查询 Black-Scholes 定价 |

### 手动测试 webhook

```bash
curl -X POST https://yourserver.com/webhook \
  -H "Content-Type: application/json" \
  -d '{
    "secret_key": "your_WEBHOOK_SECRET_TOKEN",
    "ticker": "SPY",
    "action": "BUY",
    "price": 580.0,
    "rsi": 54.0,
    "atr": 5.0,
    "vol": 50000000,
    "stop_loss_atr_multiplier": 2.0,
    "risk_tier": 0,
    "vix": 16.0,
    "spy_price": 580.0,
    "spy_200_sma": 565.0
  }'
```

预期响应：`Processed 1 signal(s)`

### 直接查询期权价格

```bash
curl -X POST https://yourserver.com/options/price \
  -H "Content-Type: application/json" \
  -d '{
    "symbol": "AAPL",
    "option_type": "call",
    "strike": 200,
    "underlying": 195.5,
    "expiry": 0.11,
    "risk_free_rate": 0.05,
    "volatility": 0.25
  }'
```

返回完整的希腊字母数据：price（价格）、delta、gamma、theta、vega、rho、implied_volatility（隐含波动率）。

### 机制状态阈值一览

| 条件 | 机制状态 | 资金乘数 |
|-----------|--------|-------------------|
| VIX < 35 AND SPY > 200-SMA | RISK_ON | 1.0（全仓） |
| VIX < 35 AND SPY between SMA×0.98 and SMA | TRANSITION | 0.5（半仓） |
| VIX ≥ 35 OR SPY < SMA×0.98 | RISK_OFF | 0.0（停止） |

### 期权策略一览

| 方向偏向 | 波动率便宜（百分位 < 阈值） | 波动率昂贵（百分位 > 阈值） |
|------|-----------------------------|--------------------------------|
| 看涨 | 牛市看涨价差 / 买入看涨期权 | 现金担保看跌期权 |
| 看跌 | 熊市看跌价差 / 买入看跌期权 | 备兑看涨期权 |
| 中性 | 跨式组合（买入） | 宽跨式组合（买入）/ 现金担保看跌期权 |

---

*本文档记录的是截至 2026 年 6 月的系统状态。如果你新增了功能或修改了参数，请同步更新这份指南——记录一件事最好的时机，就是在你刚刚构建它的时候。*
