# 第二批：固定输入的输出诊断

2026-10-12，Windows，host16 i9-12900HK / RTX 4060 Ti 16 GiB / 约 32 GiB RAM，
host11 i7-10700 / 约 64 GiB RAM，有线 LAN，IQ3_S，候选引擎 0.1.42。
128K 上下文、512-token prefill、36-token 固定提示词、每请求 256 输出 token、temperature=0、seed=42，
prompt-cache=0、CPU prompt share=0、本地 RAM=0、实际 GPU cache=3107 slots。
两端二进制与前一轮 ABBA 相同；哈希、环境、profile/布局哈希见 `environment.json` 及每组 `worker-evidence.json`。

命令：`python -u tools/diagnose_cluster_output.py`。每组 1 次预热、5 次计入；此表含预热，因为
prompt lookup 的成本策略会跨请求学习。trace、首窗口 logits 和 decode 阶段计时开启，**速度不作验收**。

| 配置 | 生成次数 | 输出哈希数 | 首窗口 logits 哈希数 | 窗口模式数 |
| --- | ---: | ---: | ---: | ---: |
| 原传输，默认 lookup | 6 | 1 | 1 | 2 |
| fast I/O，默认 lookup | 6 | 2 | 1 | 3 |
| 原传输，lookup=0 | 6 | 1 | 1 | 1 |
| fast I/O，lookup=0 | 6 | 1 | 1 | 1 |

fast I/O 第 3 次生成（第 2 次计入）复现之前的 `2a0faae42c05…`，其余为 `cc4ad7a4a8bb…`。
完整 tokens/文本在各组 `output-N.json`，窗口位置/长度在 `windows-N.json`，首窗口 FP32 logits
在 `first-logits.N`。两种输出首窗口 logits 逐字节相同（`42dfdc32641c…`），从输出下标 68
（第 69 个 token）分歧。第一处窗口差异更早位于输入位置 62：基线 T=4，分歧样本 T=3；
位置 103 又分别为 T=1/T=4。输出差异包括说明中的 “returns a list” 与 “returns a new list”，
之后说明内容也分开。不能把它归为无关的日志/计时差异。

关闭 lookup 时，两端传输的全部 12 次输出、首窗口 logits 和窗口列表相同：87 个窗口，
MTP 171/218，GPU hits/lookups=29244/146400，每请求远端 tasks=4176、entries=117156，
发送/接收 151486512/150013824 字节，本地 entries=0。测试入口中固定 PCIe share=0，adaptive tier=0。
这支持按固定窗口配置继续第二批性能对照；不保证任意输入/负载稳定，也未修复默认路径的数值问题。

源码路径：`src/program/generate.cpp` 的 `DraftPolicy::choose/observe` 按实测耗时选 lookup；
`src/kernels/cpu/native_expert.cpp` 的默认单/多 token 分派采用不同舍入；CUDA serve 的
route tail skip 依据窗口内所有 token 的专家 rank 决定跳过。上游已有
[issue #1928](https://github.com/Niko1221/Strata/issues/1928) 和
[PR #1818](https://github.com/Niko1221/Strata/pull/1818)。本轮没有合入 PR 或修改这些默认策略，
也没有单独量化 CPU 舍入和 tail skip 对本例分歧的各自贡献。传输协议/真实专家字节对照的既有结果仍保留。

补充命令：`python -u tools/diagnose_cluster_output.py --cases plain,unreduced --repeats 3`。
结果见 `../2026-10-12-optimize-output-114855/`：普通 decode 移除 MTP、关闭 lookup，4 次输出
`dbaffbdd2037…` 和 256 个单 token 窗口全部一致；非汇总路径 4 次输出 `cc4ad7a4a8bb…`，窗口也全部一致。
普通 decode 的首窗口 logits 与上述四组相同，非汇总路径的首窗口 logits 为另一个稳定哈希
`6b6acb98b8aa…`；汇总与非汇总舍入不同，不能要求所有路径 logits 字节相同。

失败记录不删除：`../2026-10-12-optimize-output-111323/` 在第二次生成后因 stdout DONE 先于
stderr 汇总而误报缺少计时；工具已改为用 DONE 的小数毫秒，并在测量结束后等待 stderr。
本目录完成上述四组后，原 plain 的 `--spec 0` 参数被 native serve 拒绝，随后修正为移除 MTP
并单独补跑两组。`run-status.json` 记录未完成部分，不把失败轮当作性能数据。

验证：17 项 cluster-config、18 项 Python 协议/失败路径、1 项延迟 stderr/移除 MTP 的捕获回归通过；
Python 编译检查通过。仅修改测量工具及文档，无新的 C++/CUDA/HIP/SYCL 源码修改或构建结果。
未改系统设置、保护余量、部署包或默认开关，未提交/推送，未发送 issue/PR 评论。
诊断进程已退出；后续无 trace 的性能样本单独记录。
