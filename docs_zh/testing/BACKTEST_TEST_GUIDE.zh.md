# 回测引擎测试指南

回测引擎（Backtest Engine）在历史数据上模拟 Nox 交易策略，逐日应用所有实盘策略规则，以此衡量历史表现。

## 测试覆盖的组件

- **CSV 解析器**：历史数据的加载与校验
- **市场状态分类（Regime Classification）**：对历史数据应用的判定规则
- **交易模拟**：开仓/平仓逻辑、盈亏计算
- **参数调优**：用于网格搜索的优化模式
- **输出报告**：交易日志与汇总统计

## 构建回测程序

```bash
cd /root/Nox/backtest-engine
./build.sh

# 或手动构建：
g++ -std=c++17 -o ../build/backtester main.cpp
```

## 单元测试

### CSV 解析器测试
```bash
g++ -std=c++17 -o build/test_csv_parser tests/test_csv_parser.cpp \
  backtest-engine/csv_parser.hpp
./build/test_csv_parser
```

**测试内容**：
- 正确解析合法的 CSV 数据行，不报错
- 拒绝缺少字段的行
- 将字符串日期转换为可比较的格式
- 处理空文件这一边界情况

**预期输出**：
```
✓ CSV Parser: loads valid data
✓ CSV Parser: rejects malformed rows
✓ CSV Parser: computes SMA correctly
```

## 手动集成测试

### 1. 完整回测运行
```bash
# 确认数据文件存在
ls -la ./data/spy_vix_daily.csv

# 使用默认参数运行回测
./build/backtester ./data/spy_vix_daily.csv

# 预期输出：
# === BACKTEST SUMMARY ===
# Total Trades: XX
# Win Rate: XX.X%
# Avg Win: $XXX.XX
# Avg Loss: -$XXX.XX
# Total P&L: $XXX.XX
# Sharpe Ratio: X.XX
```

### 2. 日期区间测试
```bash
# 对指定日期区间进行回测
./build/backtester ./data/spy_vix_daily.csv \
  --start 2023-01-01 --end 2023-12-31

# 验证输出中只包含该日期区间内的交易
# 检查 trades.csv 中不存在 2023-01-01 之前的交易
```

### 3. 参数变化测试
```bash
# 使用自定义的市场状态阈值进行测试
./build/backtester ./data/spy_vix_daily.csv \
  --vix 40 \
  --buffer 0.95 \
  --kelly-fraction 0.20

# 验证新阈值已生效：
# - VIX 阈值改为 40（默认值为 35）
# - SMA 缓冲区改为 0.95（默认值为 0.98）
# - Kelly 仓位系数改为 0.20

# 输出应显示不同的交易数量/盈亏结果
```

### 4. 无界面（优化）模式
```bash
# 输出单行 CSV，供优化脚本使用
./build/backtester ./data/spy_vix_daily.csv --headless \
  --vix 35 --buffer 0.98 --kelly-fraction 0.25

# 预期输出：
# vix,buffer,kelly,trades,winrate,sharpe,pnl
# 35,0.98,0.25,45,0.531,1.23,15230.45

# 该格式为机器可读格式，便于网格搜索使用
```

### 5. 交易日志校验
```bash
# 运行回测
./build/backtester ./data/spy_vix_daily.csv

# 查看生成的 trades.csv
head -20 trades.csv

# 验证格式：
# entry_date,entry_price,exit_date,exit_price,shares,pnl
# 2022-03-15,420.50,2022-03-18,425.30,100,480.00
# ...

# 检查计算是否正确：
# - PnL = (exit_price - entry_price) * shares
# - 开仓/平仓日期按时间顺序排列
# - 不存在负的持仓股数
```

### 6. 市场状态分类一致性测试
```bash
# 在市场状态分类结果已知的历史时段上运行回测
# （例如 2020 年 2 月新冠崩盘期间：应判定为 RISK_OFF）

./build/backtester ./data/spy_vix_daily.csv \
  --start 2020-02-15 --end 2020-03-15

# 查看日志中的状态切换记录
# 确认高 VIX 时段（2020 年 2-3 月）显示为 RISK_OFF 状态
```

### 7. 组合账户核算测试
```bash
# 验证组合价值计算是否正确

./build/backtester ./data/spy_vix_daily.csv

# 检查汇总结果：
# - 起始资金：$100,000.00
# - 若总盈亏为 $+15,230.45
# - 期末资金应为：$115,230.45

# 在 trades.csv 中核对累计盈亏是否合理
```

### 8. 边界情况：参数过于严格
```bash
# 设置极其严苛的过滤条件以减少交易数量
./build/backtester ./data/spy_vix_daily.csv \
  --vix 20 \
  --buffer 0.99 \
  --kelly-fraction 0.10

# 预期：0-5 笔交易（满足条件的情况极少）
# 即使没有任何交易也不应崩溃
```

### 9. 边界情况：参数过于宽松
```bash
# 设置非常宽松的过滤条件以生成大量交易
./build/backtester ./data/spy_vix_daily.csv \
  --vix 50 \
  --buffer 0.80 \
  --kelly-fraction 0.50

# 预期：200+ 笔交易（条件非常宽松）
# 验证统计结果中不存在整数溢出
```

## 网格搜索 / 参数优化

```bash
# 优化循环示例（bash）
for vix in 30 35 40 45; do
  for buffer in 0.95 0.96 0.97 0.98; do
    for kelly in 0.15 0.20 0.25 0.30; do
      ./build/backtester ./data/spy_vix_daily.csv --headless \
        --vix $vix --buffer $buffer --kelly-fraction $kelly
    done
  done
done > optimization_results.csv

# 解析 CSV 找出最优参数
head -1 optimization_results.csv > best.csv
sort -t',' -k5 -rn optimization_results.csv | head -1 >> best.csv
cat best.csv
```

## 数据文件要求

回测程序需要包含以下列的 CSV 文件：
```
date,close,vix,sma_200
2022-01-03,480.50,18.2,465.30
2022-01-04,482.10,17.8,465.80
...
```

**生成示例数据**：
```bash
python3 download_data.py --output ./data/spy_vix_daily.csv
```

## 环境变量

无需任何环境变量。回测程序完全自包含，不依赖外部配置。

## 何时需要更新本指南

出现以下情况时应更新本指南：
1. **CSV 格式变化** —— 新增或删除列
2. **命令行参数变化** —— 新增标志位或默认值变化
3. **交易逻辑变化** —— 开仓/平仓条件被修改
4. **Kelly 公式变化** —— 输入参数或计算方式发生变化
5. **输出格式变化** —— trades.csv 或汇总结果中新增列
6. **参数取值范围变化** —— 新的合法最小/最大值

## 常见故障与排查

| 现象 | 排查方向 | 解决方法 |
|---------|-------|-----|
| `[ERROR] File not found` | CSV 路径是否正确 | 使用绝对路径，或确认文件确实存在 |
| `[ERROR] CSV parse error on row 5` | 数据格式是否符合规范 | 检查日期格式（YYYY-MM-DD）以及数值列 |
| `Sharpe Ratio: NaN` | 交易笔数不足以计算 | 放宽参数以获得更多交易 |
| `Win Rate: 0%` | 策略在所有交易中均亏损 | 检查市场状态阈值是否设置过严 |
| trades.csv 中出现重复记录 | 索引跟踪存在缺陷 | 提供可复现问题的日期区间并上报 |
| 交易笔数多但总盈亏低 | 检查胜率 | 策略可能存在较大的单笔亏损幅度 |
| 参数敏感性表现异常 | 每次只改动一个参数重新运行 | 逐一隔离，确定是哪个参数导致了变化 |

## 部署前测试清单

- [ ] 在完整数据集上运行回测不会崩溃
- [ ] trades.csv 内容有效且按日期排序
- [ ] 汇总统计包含全部指标（夏普比率、胜率等）
- [ ] 不同参数组合会产生不同的结果
- [ ] 无界面模式能生成合法的 CSV 输出
- [ ] 优化脚本能够正确解析结果
- [ ] 起止日期过滤功能工作正常
- [ ] 边界情况（0 笔交易、1000+ 笔交易）均已妥善处理
