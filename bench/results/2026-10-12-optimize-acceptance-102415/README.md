# 第一批固定配置对照、第二批 fast I/O ABBA 复测

2026-10-12，Windows 双机：host16 i9-12900HK、RTX 4060 Ti 16 GiB、约 32 GiB RAM；
host11 i7-10700、约 64 GiB RAM。IQ3_S，候选引擎 0.1.42，MTP spec 4/min-p 0.5，INT8 KV，
128K 最大上下文、512-token prefill、36-token 固定提示词、每次 256 输出 token、temperature=0。
host16 P-core 池、spin 50 us；host11 7 worker、原绑核设置、原任务粒度、自旋默认值。
GPU 专家缓存实际 3107 slots / 5.95 GiB；远端汇总、全部 46.84 GiB 专家预加载，独立端口 18520。

命令：`python -u tmp/run-optimize-acceptance.py`。先第一批 0/1 GiB 对照，再第二批 A1/B1/B2/A2。
A 为原传输，B 为两端 `STRATA_NETWORK_FAST_IO=1`。每个引擎会话 1 次预热、5 次计入；
B1/B2 共享同一个 worker，切换 A/B 时重启测试 worker，模型加载不计入 decode。
本轮固定 `STRATA_PREFILL_CPU_SHARE=0`，prompt-cache=0，关闭 profiling 和诊断内存采样。
未改页文件、内存保护余量、安装配置、运行包或默认开关；未提交、推送、发布。

引擎 SHA256：`82b111c65a753cb613cc7f7a0b378efe81d2eeb7b45815dbdcc5b3e1e5f61314`。
host11 host/backend SHA256：`93acf20478ec7cb785a2c1722fe8414bca5884838f13a5592abb35f1f35564bb` /
`f7db4ff82c48a6f262353bfc919cab172b18d6e6ca2053c38e4b5da5e3f28825`。
各组 `worker-evidence.json` 保留预加载日志和哈希；系统/驱动信息在 `environment.json`，
profile、布局及源配置哈希在 `local-inputs.json`，完整参数及开关在各组 `result.json`。

| 第一批配置 | 计入次数 | decode 中位数 tok/s | prompt 中位数秒 | prompt+decode 中位数秒 | 实际 RAM GiB | 计入本地 entries | 计入输出哈希数 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 GiB | 5 | 13.743 | 1.331 | 19.986 | 0 | 0 | 1 |
| 1 GiB | 5 | 13.807 | 5.434 | 24.045 | 1.00 | 15201 | 3 |

1 GiB 实际参与，decode 中位数差约 +0.47%，但输出、MTP 和路由工作量变化，不能作为相同计算量
的收益。RAM 预算路径使用 unbuffered file tier，0 GiB 路径保留文件缓存，prompt 延长。
不默认启用本地分担，不扩展到 2/4 GiB；需要定位输出变化并消除 prompt 路径代价后重新验收。
内存容量取舍另见 `../2026-10-12-optimize-memory-101138/README.md`。

| 第二批顺序 | 五个计入样本 tok/s（引擎日志的舍入值） | 会话中位数 tok/s |
| --- | --- | ---: |
| A1 原传输 | 16.5 / 13.9 / 16.2 / 13.6 / 16.0 | 16.0 |
| B1 fast I/O | 13.8 / 16.3 / 14.1 / 16.2 / 16.4 | 16.2 |
| B2 fast I/O | 16.0 / 15.7 / 13.3 / 16.5 / 13.7 | 15.7 |
| A2 原传输 | 13.2 / 16.4 / 15.9 / 14.4 / 14.8 | 14.8 |

| 第二批合并结果 | 计入次数 | decode 中位数 tok/s | 范围 tok/s | 样本 p95 decode 秒 | prompt+decode 中位数秒 |
| --- | ---: | ---: | --- | ---: | ---: |
| 原传输 | 10 | 15.356 | 13.235–16.488 | 19.342 | 17.997 |
| fast I/O | 10 | 15.886 | 13.266–16.467 | 19.297 | 17.462 |

合并中位数差约 +3.45%，比组内波动小；A1/A2 自身也有明显变化，样本 p95 只差 45 ms。
每组只有 10 个计入样本，此处最近秩 p95 等于最大值，不是逐层 socket 延迟 p95。
原传输 10 次计入输出都是 `cc4ad7...`；fast I/O 9 次相同，B1 第四次计入为 `2a0faa...`，
另有一次预热也是该哈希。fast I/O 输出一致性未通过，不能宣称可靠提速或调整默认值。
之前无 fast I/O 的内存诊断也出现 `2a0faa...`，目前不能认定变动由传输改变数值造成。
MTP accepted/proposed 分别为 171–172 / 218–220，GPU 命中约 18.5–20.0%；缓存容量相同。
全部样本都保留，没有按输出哈希删选速度样本。

原始报告 `summary.json`，各组 `samples.json`/`engine.log`；按生成提取 prompt、累计网络差值及
本地条目的报告为 `samples-with-network.json`；合并分析为 `analysis.json`。
本轮只保留输出 token 哈希，没有保存 token 全文；工具随后增加了后续运行的 token/文本保存、
MTP/命中记录、固定 prompt share、按轮反转顺序、清除继承的候选开关及诊断观察入口。

验证：Python 编译检查、17 项 cluster-config 测试、候选 DLL 的 9 项输入/输出验证测试通过。
本轮不改引擎 C++/CUDA/HIP/SYCL 源码，无新增后端构建结果。
测试结束后停止本轮启动的 worker 和引擎，保留此前运行包。

下一步先做固定输入的输出稳定性诊断，保存完整 tokens/文本，区分 prompt、MTP 窗口、本地计算和
汇总路径的变化，再复测。任务粒度、自旋、单项传输及组合完整生成验收仍未完成。
第三、四批性能推进保持暂停；长上下文、并发负载、host17 和 IOCP 完整生成未测。
