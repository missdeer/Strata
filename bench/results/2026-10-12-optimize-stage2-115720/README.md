# 第二批：关闭 lookup 后的 fast I/O ABBA

2026-10-12，Windows 双机，有线 LAN。host16 i9-12900HK、RTX 4060 Ti 16 GiB、约 32 GiB RAM；
host11 i7-10700、约 64 GiB RAM。IQ3_S，候选引擎 0.1.42；128K 上下文、512-token prefill、
36-token 固定提示词、每请求 256 输出 token，temperature=0、seed=42，MTP spec=4/min-p=0.5，
INT8 KV、RAM=0、GPU cache 实际 3107 slots / 5.95 GiB、远端汇总、全部 46.84 GiB 专家预加载。
host16 P-core 池、自旋 50 us；host11 7 worker 加调用线程、原绑核/任务粒度、自旋默认 20 ms。

命令：

```powershell
python -u tools/benchmark_cluster_optimizations.py --stage 2 --engine tmp/dual-host-native/optimize-host16/strata.exe --cases baseline,fast-io --rounds 2 --warmup 1 --repeats 5 --seed 42 --suffix-draft 0 --prefill-cpu-share 0
python tools/analyze_cluster_optimizations.py bench/results/2026-10-12-optimize-stage2-115720
```

顺序 A1/B1/B2/A2，每会话 1 次预热、5 次计入。A 为原传输，B 为两端 `STRATA_NETWORK_FAST_IO=1`。
B1/B2 共用 worker，每会话重新启动 coordinator；A/B 切换重启独立 worker，加载时间不计入 decode。
所有组固定 lookup=0、CPU prompt share=0、prompt-cache=0。lookup=0 是本轮独立测试配置，
与默认 lookup 路径的历史速度不可直接合并；普通服务参数没有改变。
trace、GPU/CPU 阶段 profiling、首 logits 导出和内存诊断采样全部关闭。
用 stdout DONE 的小数毫秒计算速度，并在请求结束后等待 stderr 汇总，避免跨请求误读。

| 会话 | 五个计入样本 tok/s | 中位数 tok/s |
| --- | --- | ---: |
| A1 | 16.180 / 14.558 / 15.547 / 13.754 / 16.039 | 15.547 |
| B1 | 15.818 / 13.617 / 15.625 / 13.408 / 15.702 | 15.625 |
| B2 | 13.345 / 15.747 / 13.201 / 14.763 / 13.425 | 13.425 |
| A2 | 13.569 / 15.833 / 13.438 / 15.629 / 15.537 | 15.537 |

| 合并结果 | 计入次数 | decode 中位数 tok/s | 范围 tok/s | 样本 p95 decode 秒 | prompt+decode 中位数秒 |
| --- | ---: | ---: | --- | ---: | ---: |
| 原传输 | 10 | 15.542 | 13.438–16.180 | 19.0504 | 17.8067 |
| fast I/O | 10 | 14.190 | 13.201–15.818 | 19.3932 | 19.4223 |

本轮 fast I/O 中位数差 -8.70%，p95 多 342.8 ms，没有取得可采用的收益。
B1/B2 自身中位数明显变化；这个有限的短提示词样本不能证明任意环境下 fast I/O 都退化，
也不能把前一轮默认 lookup 的 +3.45% 当作已确认提速。不修改默认开关，不推荐部署。
p95 是整请求 decode 时间的最近秩统计，n=10 时等于最大值，不是逐层 socket 延迟 p95。

24 次生成（含预热）全部输出 `cc4ad7a4a8bb…`，每次 256 token，MTP 171/218，
GPU hits/lookups=29244/146400，远端 requests=4176、entries=117156，发送/接收
151486512/150013824 字节，本地 entries=0。两组的输出、工作量、二进制和缓存对照全部通过。
完整 tokens/文本在各组 `output-N.json`，`prompt.json` 保留输入 tokens 和 seed，
`samples.json` 保存每次 DONE、MTP、路由和网络累计差值；没有按速度或输出哈希删选样本。

引擎 SHA256 `82b111c65a753cb613cc7f7a0b378efe81d2eeb7b45815dbdcc5b3e1e5f61314`；
两端仍是前轮候选，host11 哈希和完整预加载日志在每组 `worker-evidence.json`，
cluster 配置、参数和两端开关在各组 `cluster.json`/`result.json`。
同日硬件、驱动及 profile/布局哈希见 `../2026-10-12-optimize-output-112050/environment.json`。

远端 CPU 时间在会话边界读取，包含 coordinator 启动、预热和所有生成：A1/B1/B2/A2 平均占用
约 5.459/5.361/5.414/5.440 个逻辑 CPU 的时间；它不是 decode-only CPU 百分比。
等待 2 秒后再测约 2.7 秒的空闲区间，四会话均未累计可见 CPU 时间；采样精度有限。
各原始进程 CPU/工作集/私有提交和时间戳均在 `worker_usage` 中，不用它推断逐层 CPU 瓶颈。

本轮只修改测量工具和文档，未修改引擎源码、系统设置、保护余量、部署包或默认开关，未提交/推送。
ABBA 的测试 worker/coordinator 已退出；后续调度对照单独记录。第三、四批性能推进继续暂停。
