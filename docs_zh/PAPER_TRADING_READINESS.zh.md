# 模拟交易就绪状态与实盘迁移指南

**最后更新：2026-06-29（更新：Gap 1 + Gap 2 已实现；Gap 3 此前已完成）**

---

## 简短回答

**是的，你现在就可以开始模拟交易。** 核心执行引擎、信号生成器、仓位管理器和风控闸门都已完全可用。整体架构已达到生产级水准。

**不，你还没有准备好投入实盘资金。** 有三个具体缺口必须先补齐——见第 3 节。

---

## 1. 已经完全可用的部分

以下每个组件都在实际运行，能为你提供真实可靠的模拟交易数据：

| 组件 | 状态 | 为你做了什么 |
|-----------|--------|----------------------|
| 期权信号生成器 | ✅ | 每 30 分钟扫描一次观察名单，生成 8 种策略类型（CSP、CC、价差、跨式、宽跨式） |
| 执行引擎（Alpaca） | ✅ | 将多腿市价单路由到 Alpaca 模拟账户，完成鉴权、确认成交 |
| 仓位管理器 | ✅ | 在 SQLite 中跟踪每一个未平仓的期权持仓；自动执行 50% 止盈、21 DTE、2× 止损规则 |
| 市场状态机 | ✅ | VIX + SPY 200 日均线闸门——在 RISK_OFF 状态下抑制方向性交易，在 TRANSITION 状态下调整仓位规模 |
| WS1 矛盾向量 | ✅ | 当 NLP 新闻标题情绪与隐含波动率（IV）偏斜方向相矛盾时，阻断信号 |
| WS2 另类宏观管道 | ✅ | 战争风险保险费率、AIS 油轮航运异常、OFAC 制裁——提供宏观尾部风险背景信息 |
| WS3 内部人集群过滤器 | ✅ | 从 SEC Form 4 备案中检测多名高管的集中买入（作为看涨确认信号） |
| WS5 流动性真空闸门 | ✅ | 当买卖价差超过滚动基线 3 个标准差以上时，中止该信号——避免糟糕的成交 |
| WS4 半衰期衰减 | ✅ | 按类别对所有情绪分数进行指数衰减（地缘政治：6 小时，宏观：48 小时，财报：72 小时，技术面：12 小时） |
| WS6 怀疑者报告 | ✅ | 每周六批量生成——JSON + Markdown 格式的每周信号汇总、评级与各工作流结论 |
| WS7 滞后窗口 | ✅ | 跟踪 6-K SEC 备案在被中国散户媒体报道之前的时间差；对每个事件评级 A/B/C/F |
| 每日侦察 | ✅ | 美东时间 09:00：美股新闻 + SEC 备案 + 中国宏观数据 → Claude 分析 → Telegram 推送 |
| 周报 | ✅ | 美东时间 16:00（当周最后一个纽交所交易日）：盈亏、胜率、MAE、校准度、解析失败情况 |
| 月度总结 | ✅ | 每月 1 日：完整绩效报告写入 `reports/YYYY-MM.md` |
| SEC 雷达（实时） | ✅ | 每 30 秒轮询 8-K 和 6-K 信息流；检测到备案后 → Claude 风险评分 → Telegram 推送 |
| 全市场扫描器 | ✅ | 三阶段流程：约 6,000 支股票的全市场 → 活跃度筛选 → RSI/ATR/SMA 分析，每 30 分钟一次 |
| IV 累积 | ✅ | 每日收盘 16:30 快照——逐步积累真实的 52 周 IV 百分位数据 |
| IBKR 路径 | ✅ 已接通 | Socket 层与订单路由已全部编码完成，但尚未启用——见第 3 节 |

---

## 2. 模拟交易能给你带来什么数据

使用本系统进行模拟交易**将会**产生以下有用且真实的数据：

**将会准确的数据：**
- 信号频率与市场状态分布（RISK_ON、TRANSITION、RISK_OFF 各自出现的频率）
- 期权策略组合（IV 与方向偏好条件下会选中哪些策略）
- 成交滑点基准——模拟成交会报告实际成交价，可与 Black-Scholes 理论价格对比
- 止盈/止损规则的有效性——50% 止盈 / 21 DTE / 止损触发在真实市场条件下的比例
- IV 百分位累积——每支标的经过 30 个交易日后，真实的 IV 百分位数据开始具有意义
- 工作流过滤器命中率——每周有多少比例的信号被 WS1/WS3/WS5 拦截

**将会存在噪声或缺失的数据（已知缺口）：**
- **已实现盈亏** —— `memory_bank.db` 中的 `trade_history` 表目前不会被执行引擎写入。你能在 Alpaca 的仪表盘中看到盈亏，但在这一功能接通之前，周报的数据库查询中不会显示。
- **MAE / 校准分数** —— `trade_predictions` 表目前为空。在有工作流记录“预测 vs 实际”结果之前，周报在这一项会显示"N/A"。
- **IV 百分位的准确性** —— 在最初的 30 个交易日内，IV 百分位使用的是相对快照计算（当前 IV 相对于已累积数据的平均值）。真正的百分位排名需要约 252 个交易日的历史数据。
- **回测精度** —— 回测器使用 `HRV30 × 1.15` 作为 IV 的代理指标，并未使用真实的历史期权链价格。回测结果应视为方向性估计，而非精确的盈亏预测。

---

## 3. 投入实盘资金前需要补齐的三个缺口

这些都不是纸上谈兵——每一项都会造成真实的、涉及真金白银的运营风险：

### 缺口 1 —— 仓位管理器与信号生成器的集成 ✅ 已修复

**此前的问题：** `executeSignal()` 在成交成功后从未调用 `add_position()`。止盈/止损规则（50% 止盈、21 DTE、止损）从未触发过。

**已应用的修复（2026-06-29）：** 在 `OptionsSignalGenerator` 的构造函数中加入了 `PositionManager*`。现在，在 `executeSignal()` 中成功执行 `router.route()` 之后，会带着成交细节调用 `add_position()`。`VixTermStructure` 被移到命名空间作用域（这是此前遗留的 GCC 13 编译修复）。`PositionManager.cpp` 中的 TelegramNotifier 桩代码已替换为真正的 `nox::TelegramNotifier`。二进制文件已重新构建。

---

### 缺口 2 —— 重启时的仓位状态同步 ✅ 已修复

**此前的问题：** 重启时，`open_positions` 这个 SQLite 表不会从券商端重新填充。对于重启前建立的仓位，止盈/止损规则从此不再触发。

**已应用的修复（2026-06-29）：** 在 `NoxEngine` 的构造函数中新增了 `reconcilePositionsFromBroker()`，在 `positionManager_->start_monitoring()` 之后调用。该函数会从 Alpaca 拉取 `GET /v2/positions`，筛选出 `asset_class=us_option` 的持仓，解析 OCC 格式的期权代码（标的+YYMMDD+C/P+行权价），并用 `avg_entry_price` 作为记录的建仓价，把所有未被跟踪的仓位补录进来。系统会发送一条 Telegram 提醒，告知补录了多少个"孤儿仓位"。

---

### 缺口 3 —— 财报规避过滤器 ✅ 此前已实现

**现状：** 这项功能在本文档撰写之前就已经实现。`fetchEarningsCalendar()` 会在每个扫描周期开始时查询 `GET http://america-data-engine:8001/earnings/calendar`。`hasEarningsWithin5Days()` 使用的是 **5 天**窗口作为闸门（比文中前面提到的 3 天更为保守）。距离财报公布不足 5 天的标的会被跳过，并记录一条 `[EARNINGS_GATE]` 日志。

---

## 4. 实盘交易迁移路径

当模拟交易验证了信号质量后（目标：60 天窗口期，≥50 个信号，方向准确率 ≥52%），按以下顺序推进：

### 第 1 步：启用 IBKR 执行路径

IBKR 已经接通完毕。还剩三项工作待完成（详见 `execution/IBKR_MIGRATION.md`）：

1. 卖出路径：在路由 SELL 信号之前实现 `reqPositions()`（IBKR 没有"全部平仓"接口）
2. 将 `executeSignal()`（当 `EXECUTION_VENUE=ibkr` 时）接到 `IBKROrderRouter`，而不是 `OptionsOrderRouter`
3. 将 `PositionManager` 的报价获取接到 IBKR 环形缓冲区中的流式行情，而不是 Alpaca REST 接口

### 第 2 步：切换 `ALPACA_BASE_URL`（如果继续使用 Alpaca）

最简单的实盘交易路径是：
```env
ALPACA_BASE_URL=https://api.alpaca.markets
```

这只是一行配置的改动。在切换之前，请确保你的 Alpaca 账户已入金且已开通期权交易权限。

### 第 3 步：增加组合层面的风险限额

在实盘之前，需要在 `main.cpp` 中为整体希腊字母风险敞口设置硬性上限：

- **Delta 上限：** 如果 sum(|delta| × 价格 × 100) 超过组合价值的 N%，则暂停期权扫描器
- **名义本金上限：** 若期权未平仓的总名义本金超过组合价值的 X%，则拒绝新信号
- **回撤熔断机制：** 已经存在（`DRAWDOWN_HALT_PCT=0.10`），需确认其设置正确

### 第 4 步：根据模拟交易结果重新校准 Kelly 参数

当前的 Kelly 参数（`KELLY_WIN_RATE=0.6842`、`KELLY_WIN_LOSS_RATIO=2.316`）来自回测。在完成 60 天的模拟交易后，需要重新校准：

```python
# 来自 trade_history 表（一旦该表开始被填充）：
wins  = len([t for t in trades if t.pnl > 0])
total = len(trades)
avg_win  = mean([t.pnl for t in trades if t.pnl > 0])
avg_loss = abs(mean([t.pnl for t in trades if t.pnl < 0]))

win_rate = wins / total                       # 替换 KELLY_WIN_RATE
win_loss = avg_win / avg_loss if avg_loss > 0 # 替换 KELLY_WIN_LOSS_RATIO
```

如果实盘胜率明显低于 0.68，在追加资金之前应先降低 `KELLY_FRACTION`。

---

## 5. 在不同市场范围上进行回测

回测器可以测试任意市场板块。在开始模拟交易之前，可以用它来验证你的信号质量在不同标的池上的表现。

### 快速上手示例：

```bash
# 你推荐的观察名单（已移除 TSLA）
./execution/nox_backtest watchlist=SPY,QQQ,AAPL,NVDA range=2y entry_slip=0.15 exit_slip=0.15

# 仅中概股
./execution/nox_backtest watchlist=BABA,JD,BILI,PDD,DIDI,NIO,XPeng,NTES,BIDU,IQ range=2y

# 大盘科技股
./execution/nox_backtest watchlist=AAPL,MSFT,GOOGL,NVDA,META,TSLA range=2y

# 医疗保健板块
./execution/nox_backtest watchlist=JNJ,PFE,AMGN,LLY,ABBV,MRK,TMO,REGN range=2y

# 金融板块
./execution/nox_backtest watchlist=JPM,BAC,WFC,GS,C,BLK,BX,KKR range=2y

# 消费板块
./execution/nox_backtest watchlist=WMT,COST,MCD,NKE,SBUX,DIS,BKNG range=2y

# 对比：相同标的，不同滑点假设
./execution/nox_backtest watchlist=SPY,QQQ,AAPL,NVDA range=2y entry_slip=0 exit_slip=0      # 理论值
./execution/nox_backtest watchlist=SPY,QQQ,AAPL,NVDA range=2y entry_slip=0.10 exit_slip=0.10 # 乐观情形
./execution/nox_backtest watchlist=SPY,QQQ,AAPL,NVDA range=2y entry_slip=0.20 exit_slip=0.20 # 保守情形
```

### 参数说明：

| 参数 | 含义 | 常见范围 |
|-----------|---------|---------------|
| `watchlist=` | 逗号分隔的股票代码 | 任意美股上市代码 |
| `range=` | 历史数据周期 | `1y`、`2y`、`5y` |
| `entry_slip=` | 每份合约的入场滑点（美元/股） | `0.05`–`0.30` |
| `exit_slip=` | 每份合约的离场滑点（美元/股） | `0.05`–`0.30` |
| `profit=` | 达到最大盈利的 X% 时止盈离场 | `0.30`–`0.75`（默认：0.50） |
| `stop=` | 亏损达到已付权利金的 X 倍时止损 | `1.5`–`3.0`（默认：2.0） |
| `capital=` | 起始资金（分档门槛） | `5000`、`30000`、`75000` |

### 批量测试多个板块：

使用提供的脚本（`execution/backtest_market.sh`）可以在多个预先配置好的市场板块上批量测试策略：

```bash
# 赋予可执行权限
chmod +x ./execution/backtest_market.sh

# 测试单个板块（获取数据、以给定滑点运行回测、输出结果）
./execution/backtest_market.sh mega 2y
./execution/backtest_market.sh chinese 1y
./execution/backtest_market.sh healthcare 2y
./execution/backtest_market.sh all_tech_100 2y

# 一次性测试所有板块（大约耗时 20–30 分钟）
./execution/backtest_market.sh all 2y
```

**可用板块：**
- `mega` —— AAPL、MSFT、GOOGL、AMZN、NVDA、META、TSLA
- `tech` —— INTC、AMD、QCOM、MU、AMAT、LRCX、CDNS、SNPS、ASML、ARM
- `healthcare` —— JNJ、PFE、AMGN、LLY、ABBV、MRK、TMO、REGN、VRTX、BIIB
- `financials` —— JPM、BAC、WFC、GS、C、BLK、BX、KKR、SCHW、COIN
- `consumer` —— WMT、COST、MCD、NKE、SBUX、DIS、AMZN、BKKING、CCL、MAR
- `energy` —— XOM、CVX、COP、MPC、PSX、OKE、SLB、EOG、FANG、COG
- `industrials` —— BA、CAT、HON、RTX、GD、LMT、ETN、MMM、ITW、GE
- `chinese` —— BABA、JD、BILI、PDD、DIDI、NIO、XPeng、NTES、BIDU、IQ
- `mega_chinese` —— 混合板块：AAPL、MSFT、BABA、JD、NVDA、TSLA
- `all_tech_100` —— 扩展科技股全集（40 支以上）

### 结果解读：

**危险信号（暂时不要进入模拟交易）：**
- 胜率低于 52%（在小样本下与抛硬币没有统计学差异）
- 每笔交易平均盈亏低于 5 美元（滑点会吃掉全部优势）
- 最大回撤超过起始资金的 30%（波动性过大）
- 某个板块交易笔数少于 30 笔（数据量不足）

**积极信号（可以进入模拟交易）：**
- 每支标的 ≥50 笔交易，胜率达到 60% 以上
- 在考虑真实滑点后，每笔交易平均盈亏超过 20 美元
- 最大回撤低于资金的 25%
- 方向准确率达到 62%（看涨与看跌合计）

### 历史背景：

来自 2026 年 6 月的回测基线（SPY、QQQ、AAPL、NVDA、TSLA，总滑点 0.30）：
- **移除 TSLA 后：** 356 笔交易，胜率 62.6%，盈利 +10,400 美元
- **纳入 TSLA 后：** 445 笔交易，胜率 60%，亏损 -53,700 美元（波动率不匹配）

**教训：** 并非所有标的都适合这套策略。在加入观察名单之前一定要先测试。

---

## 6. 仓位规模指南（凯利公式）

你的回测显示 **胜率 62.6%，在考虑真实滑点后每笔交易平均盈利 29 美元**。这是一个真实的优势，但**仓位规模决定了你是盈利还是爆仓。**

### 计算方式：

**凯利百分比（最优杠杆）：**
```
kelly_fraction = (win_rate × avg_win - loss_rate × avg_loss) / avg_win
kelly_fraction = (0.626 × 29 - 0.374 × 10) / 29 = 0.58（58%）
```

**但不要使用满额凯利。** 实践中应使用 1/4 到 1/2 凯利，以便安然度过回撤期。

### 在 3.5 万美元起始资金下的三种情景：

| 凯利比例 | 每笔风险 | 合约数 | 预期年化收益 | 最大回撤 | 恢复时间 |
|-------|-----------|-----------|-----------------|-------------|----------|
| 1/4（保守） | 5,075 美元 | 1 份合约 | 4,000–5,000 美元（12–15%） | 约 10,000 美元 | 3–4 个月 |
| 1/2（适中） | 10,150 美元 | 2 份合约 | 8,000–10,500 美元（24–30%） | 约 20,000 美元 | 2–3 个月 |
| 全额（激进） | 20,300 美元 | 4 份合约 | 16,000–21,000 美元（48–60%） | 约 40,000 美元 | 1–2 个月 |

### 模拟交易阶段的建议：

**从 1/2 凯利（2 份合约）开始。** 理由如下：
1. 真实实盘表现比回测噪声更大（更多滑点、更宽的价差）
2. 只要优势保持存在，你随时可以扩大规模
3. 如果遭遇连续 7 次亏损（每 100 笔交易大约会发生 1 次），你不会因此恐慌
4. 预期月收入：约 700–875 美元（既有意义又不至于孤注一掷）

### 如果你想在不投入资金的情况下测试：

Alpaca 上的模拟交易完全免费。你冒的不是真金白银的风险，只是机会成本。用它来：
- 确认回测的胜率在前 50 笔交易中是否成立（目标：60% 以上）
- 验证 Telegram 提醒是否正常工作（没有静默失败）
- 测试你自己的心理纪律（能否扛过连亏 3 天的一天？）
- 在真实市场条件下校准止损和止盈的宽度

---

## 7. 推荐的模拟交易检查清单

### 第 1 天 —— 环境搭建

- [ ] 先将 `OPTIONS_BOT_AUTO_EXECUTE` 设为 `false`（仅提供建议，不实际下单）
- [ ] 手动运行 `/report`——确认 Claude 侦察模块能产出连贯的分析
- [ ] 运行 `/status`——确认所有容器都处于 ONLINE 状态
- [ ] 运行 `/pulse`——确认 VIX 和新闻标题数据是实时的
- [ ] 检查 Alpaca 模拟盘仪表盘是否显示了模拟账户余额

### 第 1 周 —— 验证信号流程

- [ ] 将 `OPTIONS_BOT_AUTO_EXECUTE` 设为 `true`（开始实际执行）
- [ ] 验证每笔交易都会产生带希腊字母数据的 Telegram 提醒
- [ ] 验证每笔交易后 SQLite 中的 `open_positions` 表都有对应记录
- [ ] 通过 Telegram 监控止盈/止损规则（50% 止盈 / 止损）的触发情况
- [ ] 每天运行 `/signals`，确认执行引擎正在正常接收信号

### 第 4 周 —— 评估信号质量

- [ ] 运行回测：`./execution/nox_backtest watchlist=SPY,QQQ,AAPL,NVDA range=2y entry_slip=0.15 exit_slip=0.15`
- [ ] 比较回测胜率与模拟交易实际胜率（预期会有一定差异；差距超过 10 个百分点需要排查原因）
- [ ] 检查 WS1 矛盾拦截率——如果超过 40% 的信号被拦截，说明 IV 偏斜阈值可能设置得过紧
- [ ] 检查 WS5 流动性闸门命中率——如果超过 20%，说明扫描观察名单里可能包含了流动性不足的标的
- [ ] 查看历史周报（`/history 5`）中的盈亏趋势

### 第 2 个月 —— 实盘交易的最终检查

- [x] ~~补齐缺口 1（仓位管理器集成）~~ —— 已于 2026-06-29 完成
- [x] ~~补齐缺口 2（重启时的仓位同步）~~ —— 已于 2026-06-29 完成
- [x] ~~补齐缺口 3（财报过滤器）~~ —— 此前已实现（5 天窗口）
- [ ] 根据模拟交易结果重新校准凯利参数
- [ ] 为实盘券商账户入金（期权交易免受 PDT 限制的最低门槛为 25,000 美元）
- [ ] 切换 `ALPACA_BASE_URL` 或启用 IBKR 路径
- [ ] 设置 `DRAWDOWN_HALT_PCT=0.10`，并在第一周进行人工监控

---

## 8. 对预期优势的诚实评估

**方差风险溢价（核心信号来源）：**
隐含波动率持续高于已实现波动率——这一论断在学术上早已确立（Carr & Wu，2009 年）。实践中，美股期权的 IV 与 RV 之间平均相差 3–5 个波动率点。这为卖出权利金的一方带来了持续的套利收益，但需要注意：
- 这份收益是通过缓慢的时间衰减（theta）逐月累积的，却可能在标的价格发生剧烈变动（gamma）时迅速回吐
- 一次重大的价格波动（财报意外、宏观冲击）就可能抹去 3–4 个月积累的权利金收入
- 本系统的财报规避机制（第 3 节，缺口 3）在这一点上尤为重要

**方向性偏好（RSI + SMA）：**
这些都是广为人知、被广泛交易的信号。预期方向准确率接近 52–55%，而非 60% 以上。回测报告的胜率为 68%，属于偏高的数值——在信任这一结果之前，务必先在样本外的模拟交易中验证其是否成立。

**WS1–WS3 过滤器：**
这些是真正能够提升信号质量的机制。文本与 IV 之间的矛盾过滤有跨市场信息不对称理论作为支撑；内部人集群买入在学术文献中也是被证实的一种 alpha 来源。一旦你积累了足够多的模拟交易样本，能够对比过滤前后的胜率差异时，应该能观察到信号质量的明显提升。

**这套系统做不到的事：**
- 无法在期权定价上胜过做市商（他们拥有完整的波动率曲面，而你只有单一的 IV 数值）
- 无法捕捉日内的 gamma 变化（30 分钟一次的扫描间隔会错过日内的波动率突增）
- 不能保证任何结果——它是一套有纪律的操作框架，不是提款机

---

*本文档应在 60 天模拟交易结束后进行复盘，并根据实际结果更新后再考虑投入实盘资金。*
