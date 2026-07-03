# 求职目标——远程或佛罗里达州杰克逊维尔（Jacksonville, FL）（学位完成之前）

在完成佛罗里达大学（UF）/清华的学业之前，可以现实地、以这个交易机器人作为作品集证据来争取的近期岗位。以下是职位类别/方向，而非确定存在的空缺——请直接在 LinkedIn、公司招聘页面、AngelList-Wellfound 上搜索这些职位名称；不要把文中提到的任何具体公司名当作已确认的空缺职位。

## 学位完成前最有希望拿下的类别（按优先级排序）

1. **初级/助理交易系统或执行工程师（Junior/Associate Trading Systems or Execution Engineer，remote-friendly）**——自营交易公司（prop trading firms）和较小的系统化基金招人更看重实际展示出来的工程能力，而非学历。你做的执行端风控闸门（流动性闸门、名义金额上限）以及交易台账/对账工作，跟这类岗位直接相关。搜索关键词："trading systems engineer"、"execution engineer"、"low latency engineer junior"。

2. **量化/数据工程师，市场数据或另类数据管道方向（Quant/Data Engineer, market data or alt-data pipelines，remote）**——你做的多源新闻兜底方案（Alpaca→NewsAPI→Polygon→RSS）、SEC EDGAR 抓取，以及重试/退避（retry/backoff）可靠性工作，正是这类岗位的日常工作内容。另类数据供应商和对冲基金的数据工程团队会招那些真正搭建过数据摄取管道的初级人才。搜索关键词："data engineer alternative data"、"market data engineer"、"ETL engineer fintech"。

3. **交易或金融科技公司的 DevOps/SRE（remote）**——你处理 docker-compose 事故的经历（静默崩溃 → 上线持久化日志 → 验证恢复）是一个站得住脚的 SRE 案例故事。金融科技/交易公司很看重那些真正在实际故障情况下运维过基础设施的人。搜索关键词："site reliability engineer fintech"、"platform engineer trading"。

4. **回测/研究工程师（Backtesting/research engineer，remote，初级）**——一旦你完成 [[02_quant_evaluation_criteria]] 里提到的净值分析表（tearsheet）工作，这个方向就会变得更有竞争力——搭建和维护回测基础设施（而不是生成alpha本身）比"量化研究员"这种岗位更容易被录用、对学历的门槛也更低。

5. **专门在杰克逊维尔（Jacksonville, FL）本地的通用软件工程师岗位**——杰克逊维尔本地虽然没有专门的量化交易公司，但确实有一个真实的金融科技/保险科技（fintech/insurance-tech）产业集群（大量金融服务后台业务，例如大型银行的区域运营中心、保险公司，以及一个正在成长中的科技圈）。现实可考虑的本地目标：区域银行技术部门、保险科技公司，或金融科技相关初创公司的后端/软件工程师岗位——即使这些岗位不是专门做交易的，也可以把这个机器人项目当作你的系统/后端能力作品集来用。搜索关键词："software engineer Jacksonville FL"、"backend engineer fintech Jacksonville"。

## 如何针对每类岗位定位自己
- 对于 SRE/执行工程师岗位，主打事故应对的经历和风控闸门相关的工程工作——这类岗位的招聘经理很看重"这个人是否真的运维过出过故障的系统"。
- 对于数据工程师岗位，主打数据管道可靠性方面的工作（重试/退避机制、数据不完整时拒绝生成报告）。
- 对于研究工程师岗位，主打回测/净值分析表方面的工作（前提是已经做出来了）——在拿到真实的样本外（out-of-sample）数据之前不要投这类岗位，否则第一个技术问题就会露馅。
- 对以上所有岗位：坦诚说明自己目前学位还没读完，正在完成 UF 的先修课程。把这一点包装成"在已经交付生产级系统的同时，正在积极完成正式学历"——这是一个加分项，而不是需要藏着掖着的短板，尤其是在偏工程、而非偏研究的岗位上，学历门槛本来就没那么严格。

## 现实的时间线
- 0-3 个月：完成净值分析表和胜率（hit-rate）日志记录工作，打磨 [[04_bot_master_guide]] 和 [[03_bot_interview_questions]] 的面试准备内容，开始投递上面第 1-3 类岗位（远程）。
- 3-6 个月：根据面试反馈不断迭代——[[03_bot_interview_questions]] 里 Tier 4 的问题正是会把你筛掉的地方；用被拒的经历来找出接下来该补哪些短板。
- 长期：每隔约 2-3 个月重新检查一遍这份清单，因为初级/远程金融科技招聘周期变化很快，具体公司的空缺情况也会不断变化。
