# 双机第一批：提交占用诊断

2026-10-12，host16：Windows、i9-12900HK、RTX 4060 Ti 16 GiB、31.8 GiB 可见 RAM；
host11：Windows、i7-10700、约 64 GiB RAM。IQ3_S，候选引擎 0.1.42，MTP spec 4，
远端汇总，host11 7 worker、预加载全部 46.84 GiB 专家，独立端口 18520。
设备、驱动及远端二进制 SHA256 在 `environment.json`，引擎 SHA256 和实际参数在各组 `result.json`。

命令：`python -u tools/benchmark_cluster_memory.py`。此次运行的是添加固定 prompt CPU share 参数之前
的脚本版本；各组自动启用了 CPU prompt share，没有 `STRATA_TRACE`。`memory.json` 每 250 ms 采样
Windows 系统提交、物理 RAM、内核分页/非分页池和引擎私有提交，阶段标签使用最近一行引擎日志。
READY/每次生成后另取进程列表和 nvidia-smi。脚本现已允许固定 prompt CPU share，默认关闭。
未改页文件、4 GiB 主机保护余量、安装配置或部署包；测试后停止了本次启动的引擎和 worker。

每组短提示词 36 token，最大输出 256 token，512-token prefill，1 次预热、3 次计入。
这是内存诊断，采样及 CPU prompt share 会影响速度和输出，**不用于速度验收**。

| 最大上下文 | RAM 预算 MiB | 实际 RAM GiB | GPU slots | READY 引擎私有提交 GiB | 生成末引擎私有提交 GiB | 全程最低可用提交 GiB | 累计本地 entries |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 131072 | 0 | 0 | 3107 | 17.272 | 17.673 | 5.679 | 0 |
| 131072 | 1024 | 1.00 | 3107 | 18.390 | 19.599 | 4.430 | 12785 |
| 65536 | 0 | 0 | 3107 | 16.316 | 16.685 | 6.935 | 0 |
| 65536 | 1024 | 1.00 | 3107 | 17.399 | 18.578 | 5.304 | 13431 |

64K 相比 128K，READY 私有提交少 0.956 GiB（RAM=0）或 0.991 GiB（RAM=1 GiB），
生成末少 0.988/1.021 GiB。GPU 专家缓存相同；READY GPU 使用分别约 14966/13988 MiB。
这是上下文容量换提交空间，不能由此推出 decode 提速；普通配置仍保留 128K。
约 1 GiB 的节省也不足以单独覆盖昨日约 3 GiB 的提交缺口。

128K、RAM=0 的启动阶段：加载 pack 后私有提交约 2.745 GiB；MTP 加载后约 8.225 GiB；
GPU cache 填充后约 14.782 GiB；READY 为 17.272 GiB，生成末约 17.673 GiB。
READY 系统提交从启动前 11.841 增至 30.294 GiB，增量约 18.453 GiB，
其中引擎私有提交为 17.272 GiB。系统减进程的余量不能直接归于某个驱动；
进程列表可能漏掉无权限读取的进程，阶段采样也不是逐次分配追踪。
GPU 使用与进程私有提交不能相加当作物理 RAM 消耗。

今天系统提交上限为 36.265 GiB；昨日日志约 35.7 GiB。背景占用、提交额度与昨日不同，
不能把今日缓存成功归功于代码优化。今日启动前系统提交约 11.5–12.6 GiB，
1 GiB 缓存实际参与，证明“保持系统设置”不等于缓存必然无法申请。
128K 的余量仍较小，不保证同时运行其他开发任务后还能保住 4 GiB。

四组输出哈希记录见 `memory-analysis.json`；64K、RAM=0 的四次输出相同，
其他组有变化。不得将它们作为 token 完全相同的速度对照。后续性能测量固定
`STRATA_PREFILL_CPU_SHARE=0` 并关闭内存采样，单独检查输出、MTP 和实际缓存。

第一轮启动失败记录在 `../2026-10-12-optimize-memory-100924/failure.json`；
worker 报可用 RAM/commit 未满足 46.84 GiB 专家加 8 GiB 余量，没有降低余量重试。
之后 host11 可用物理 RAM 约 56.3 GiB 时重试通过。原始样本在 `summary.json`、
每组 `engine.log`/`memory.json`；阶段提取及内存汇总在 `memory-analysis.json`。

未测：128K 长提示词、并发开发负载、精确驱动分配归属、独立 HIP/SYCL GPU 的提交行为。
此次只改 Python 测量工具，没有修改 CUDA/HIP/SYCL 引擎代码。
