# 第二批：任务粒度、自旋和两端传输开关筛选

2026-10-12，Windows 有线双机，host16 i9-12900HK / RTX 4060 Ti 16 GiB / 约 32 GiB RAM；
host11 i7-10700 / 约 64 GiB RAM。IQ3_S，0.1.42 候选，同日固定配置 ABBA 的同一二进制。
128K 上下文、512-token prefill、36-token prompt、每次 256 输出 token、temperature=0、seed=42，
lookup=0、MTP spec=4/min-p=0.5、CPU prompt share=0、prompt-cache=0、INT8 KV、RAM=0，
实际 GPU cache=3107 slots / 5.95 GiB；远端汇总、46.84 GiB 专家预加载、7 worker 加调用线程。
trace、阶段 profiling 和内存采样关闭；host16 P-core 池、自旋始终为 50 us。

命令：

```powershell
python -u tools/benchmark_cluster_optimizations.py --stage 2 --engine tmp/dual-host-native/optimize-host16/strata.exe --cases baseline,tasks-16,tasks-48,spin-50,spin-500,fast-coordinator,fast-worker,fast-spin-50 --rounds 1 --warmup 1 --repeats 3 --seed 42 --suffix-draft 0 --prefill-cpu-share 0
python tools/analyze_cluster_optimizations.py bench/results/2026-10-12-optimize-stage2-123338
```

按表顺序，每组 1 次预热、3 次计入；组间重启独立 worker/coordinator。加载不计入 decode。
这是候选筛选，未按反序复测，时间漂移未排除，不能用三个样本的中位数宣称可靠提速。
任务粒度对照只修改 `STRATA_NETWORK_POOL_TASKS`；自旋对照只改变 host11 的 `STRATA_POOL_SPIN_US`，
原默认是 20 ms。fast-coordinator/fast-worker 只在对应一端设置 `STRATA_NETWORK_FAST_IO=1`；
fast-spin-50 为两端 fast I/O 加 worker 50 us 自旋。调用线程绑核保持原值。

| 配置 | 三个计入样本 tok/s | 中位数 tok/s | 最长 decode 秒 | 远端会话平均占用逻辑 CPU 数 |
| --- | --- | ---: | ---: | ---: |
| 原模式 | 16.276 / 16.636 / 13.645 | 16.276 | 18.762 | 4.011 |
| tasks=16 | 13.586 / 14.836 / 13.528 | 13.586 | 18.924 | 5.039 |
| tasks=48 | 16.469 / 13.997 / 16.618 | 16.469 | 18.289 | 4.777 |
| worker spin=50 us | 10.093 / 14.798 / 12.768 | 12.768 | 25.364 | 2.200 |
| worker spin=500 us | 15.264 / 13.009 / 14.997 | 14.997 | 19.679 | 3.140 |
| 仅 coordinator fast I/O | 14.004 / 16.276 / 13.894 | 14.004 | 18.425 | 4.024 |
| 仅 worker fast I/O | 13.656 / 16.004 / 13.170 | 13.656 | 19.439 | 4.040 |
| 两端 fast I/O + worker spin=50 us | 12.791 / 15.203 / 12.899 | 12.899 | 20.014 | 2.555 |

tasks=48 中位数高约 1.19%，远小于组内波动，且未复测；其余候选中位数低于基线。
短自旋降低会话 CPU 消耗，但本轮没有提高 decode 吞吐。没有可采用的可靠收益，不修改默认值。
上表最长 decode 等于 n=3 的最近秩 p95，仅描述这些请求，不是 socket/层延迟分布。
会话 CPU 是 worker 进程及其子进程的 CPU 时间差/墙钟差，包含启动、预热和所有请求，
不能解释成 decode-only CPU 使用；每组等待 2 秒后约 2.7 秒空闲区间均未见 CPU 累计，精度有限。

32 次生成（含预热）全部输出 `cc4ad7a4a8bb…`，MTP 171/218、GPU hits/lookups=29244/146400，
每请求网络 requests=4176、entries=117156、tx/rx=151486512/150013824 字节，local entries=0。
输出、工作量、实际缓存和二进制对照全部通过。完整 tokens/文本、DONE、路由差值、两端开关、
worker 预加载日志/哈希和 CPU/内存原始读数保存在各组文件，没有筛除慢样本。
源硬件、驱动、profile/布局哈希见同日 `../2026-10-12-optimize-output-112050/environment.json`。

本轮未测：当前固定 lookup 配置下的 4/6/7 worker 绑核重测、fast+绑核、fast+任务粒度组合，
合并发送/按需 poll/缓冲区复用三个内部动作的分别开关，反序/更多样本，长上下文/并发/Linux。
此前绑核 4/6/7 的默认 lookup 初测不等于本轮固定配置复测。第二批因此仍未全部收口；
第三、四批性能推进暂停。没有启用 IOCP、并行量化或新增硬件测试。

测试进程已退出；未改页文件、内存保护余量、安装配置、部署包或默认开关，未提交/推送。
本轮仅测量工具和文档变更，无新增 C++/CUDA/HIP/SYCL 修改或后端构建结果。
