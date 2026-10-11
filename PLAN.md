# 多机推理的 C++ 运行时迁移

## 目标和范围

将 `feature/multi-host-experts` 的多机部署和运行链路中所有 Python 实现迁移为 C++，提高请求处理效率，并让部署包不依赖 Python 解释器或 Python 包。范围包括 worker、共享配置、启动器、主机 API、tokenizer、提示词模板、流式输出和集群打包入口。现有 C++/CUDA/HIP 专家计算继续复用，不改变模型或专家分工算法。

这里的“全部”指多机功能及其运行、部署依赖；与集群无关的安装器、数据研究脚本和 Python 测试不在迁移范围。测试期间保留 Python 实现作为行为对照，完成所有验收后再移除集群部署包中的 Python 文件。

## 兼容要求

- 现有共享 JSON、专家归属表、排序规则、SHA256 和模型抽样指纹保持兼容，包括 Unicode 路径、重复 JSON 字段拒绝和未知字段拒绝。
- TCP v1 的普通及汇总模式、握手、认证、序列号、有限数检查、大小限制和截止时间保持兼容。断线或计算失败不可返回旧结果。
- worker 继续支持 CPU、CUDA、HIP；新公共实现需确认 SYCL 的接入边界，不能宣称未构建后端已受验证。
- 专家 RAM 缓存有预算，任务期间专家指针保持有效；预加载只有完整分配驻留后才开始监听，并保留物理内存和提交空间检查。
- 原生运行时以新可执行文件显式启用；迁移期间旧入口和默认引擎计算路径保持原样。
- 主机服务迁移需覆盖已有 OpenAI/Anthropic 接口、Responses、工具调用、思考、停止/取消、会话缓存、SSE、错误和用量语义，不能用简化聊天接口冒充完整兼容。
- 对外监听必须配置 API key；worker 保留 coordinator IP 限制和密钥认证。密钥不得写入配置、日志或包清单。
- C++ 重写本身不构成性能证明。只报告同日、同硬件、同模型、同参数的测量。

## 正式目标收敛（2026-10-10）

- 原生运行入口合并为 `strata-host cluster/worker/serve/vision`，角色之间重新启动当前宿主，保留进程隔离及退出清理。
- 保留 `strata.exe` 推理引擎和 `strata_network_backend.dll` 专家后端；原生完整包最多两个 exe、一个项目 DLL。辅助节点仅一个 exe。
- 图片编码静态链接 llama/mtmd，复用 CPU ggml；省略 `vision.exe` 时使用宿主的 `vision` 子命令。外部编码器设置保持兼容；内置编码器不声称支持 GPU。
- Windows 正式打包入口为 `tools/build_cluster.bat --native` / `tools/build_cluster_native.bat`，调用 C++ `strata-cluster-build`；HIP 的 `native-network` 模式也转入该工具。收集原生程序、Web、模板、示例配置和许可证，排除 Python、测试及诊断程序，并检查目标数量。`build_cluster.py` 保留为旧路径及行为对照。
- 旧 Python 安装器及默认打包路径保留；不切换正在运行的服务。下文阶段记录中的旧 exe 名称为迁移历史，当前命令见 `docs/MULTI_HOST.md`。

## 实施顺序

### 1. 原生 worker 和基础配置

- [x] 提供公共 C ABI 声明，复用现有 `strata_network_backend`。
- [x] 实现原生 worker：模型布局/GGUF 校验、映射、受预算约束的 RAM 缓存、预加载、TCP、认证、任务处理和可选计时。
- [x] 迁移共享配置校验及与 Python 一致的 canonical JSON SHA256。
- [x] 增加原生 worker 的 CMake 目标，保持 CPU-only 构建可用。
- [x] 验证旧 C++ coordinator 对新 worker 的逐专家和汇总结果；覆盖坏帧、断线、错误认证、配置不一致、非有限数、缓存淘汰和预加载失败。

### 2. 原生集群启动入口

- [x] `strata-cluster --config cluster.json --validate/--node ID`，迁移 `cluster.py` 的启动和 `make_network_config.py` 的配置生成；旧 Python 入口继续作为对照。
- [ ] 跨平台子进程生命周期：Windows job containment、停止和异常清理已实现；按本轮要求暂不验证 Linux port。
- [x] 保持本地 server 配置及共享配置不被改写；worker 可直接运行；coordinator 在第 4 步完成后直接启动原生 API。当前缺少原生 API 时明确失败，可用 `--export-config` 查看配置。

### 3. tokenizer 和请求/输出语义

- [x] C++ 实现 byte-level BPE、Qwen35 Unicode 预分词、特殊 token、增量 UTF-8 解码和停止规则。
- [x] 用现有 tokenizer corpus/golden fixture 对照完整 token ID；只验证 round-trip 不足以证明正确。
- [x] 迁移聊天模板、工具定义/结果、思考开关和结构化输出解析，并对照 `serve/chat_golden.json`、`serve/fixtures` 和 server 测试。

### 4. 主机 API 和引擎管理

- [x] 实现原生 HTTP/SSE 服务及 `/v1/models`、chat completions、Responses、Anthropic messages/count_tokens、健康/状态/用量和现有 web app 路由。
  - [x] `strata-server` 提供上述生成接口、SSE、静态页面、settings/config、load/unload、slots 和 API monitor。
  - [x] 完整 Prometheus 指标/标签/histogram 对照、GPU 多卡/磁盘监控、`/v1/vram`、磁盘 session save/restore、安全和生命周期设置。
- [x] 迁移持久引擎进程协议、请求排队、取消、错误恢复、上下文检查、会话缓存和 engine 参数生成。
  - [x] 单引擎 FIFO、READY/INFO/GEN/T/PP/RESUME/DONE、STOP 排空、管道写入/静默/启动截止时间、失败后下一请求重启、上下文检查和会话复用已验证。
  - [x] 思考预算、强制工具调用续写、采样参数、用量与缓存计数已对照；续写全程持有同一 FIFO 位置。
  - [x] batch admission、独立取消、solo/batch 切换、短请求让行、reasoning recovery、profile/multi-GPU 参数派生、磁盘 session 和 vram control。
- [x] 迁移 server 可选运行依赖：MCP stdio/Streamable HTTP 和服务端执行轮次、vision 编码器/原生图片转换、环境和 DLL 路径；不依赖 Python 实现这些功能。
- [x] 复用 web 静态资源；服务端及引擎子进程不启动 Python。
- [x] API 行为对照与真实引擎短/长提示词、多轮、工具调用、双槽并行和客户端断开验证（Windows 单机 IQ3_S；真实 LAN API 链路仍待第 6 步）。

### 5. 原生构建/打包和部署切换

- [x] 增加显式原生打包模式和正式二进制数量检查；运行包无 Python 文件。

- [x] Windows：迁移 `build_cluster.py` 的计划、构建、依赖收集、清单、原子替换及失败处理；批处理作为轻量入口，C++ 构建器不进入运行包。
- [x] Windows 包包含原生程序、必要 DLL、静态资源及许可证；不包含或依赖 Python。
- [ ] Linux 构建/打包和共享库收集：本轮按用户要求暂不实施和验证。
- [x] Windows 三节点独立编译参数和 CPU ISA 下限不变，host11 不继承 host16 的 `/QxHost`。
- [x] Windows 精简 PATH（无 Python 入口）验证打包、包启动、CPU worker 任务及 API 原生夹具请求；发布保留可恢复旧包，失败回滚，不自动停止现有服务。未在全新 Windows VM 上测试；真实模型/LAN 迁移切换仍属第 6 步。

### 6. 验收和性能

- [ ] 构建实际涉及的 CPU/CUDA/HIP/SYCL 后端；记录无法运行的硬件。
- [ ] 协议和配置交叉兼容、逐专家输出字节、汇总数值误差、tokenizer 和 API golden 对照通过。
- [ ] 同日 Python/C++ 热缓存往返和完整生成对照，记录 p50/p95、decode tok/s、首字时间、通信字节、CPU/RAM/VRAM 和日志设置。
- [ ] 新包在没有 Python 的环境启动并完成真实请求，更新 `docs/MULTI_HOST.md`。

## 完成标准

所有多机节点的部署和运行均不需要 Python；原有集群能力和客户端接口有对应的兼容验证；性能结论有可复现测量。仅完成原生 worker 不视为全部迁移完成。

## 当前记录

以下按实施顺序保留历史记录；第 4 步最初的“核心路径”限制已由末尾的完整实现记录取代。第 5 步 Windows 实现和包验证已完成；Linux、第 6 步及实际服务切换仍未完成。

- 2026-10-09：已完成范围和兼容要求梳理，开始第 1 步。已有原生 coordinator、CPU/GPU 后端、有限数校验和远端汇总可以复用。
- 当前实测对照保存在 `tmp/network-opt-worker-timing-analysis.json` 和 `docs/MULTI_HOST.md`；新运行时尚无速度测量。
- 第 1 步已完成，新增 `strata-network-worker` 和共享配置/专家缓存模块；默认 `STRATA_BUILD_NATIVE_CLUSTER=OFF`，显式开启后可构建。worker 不启动 Python。
- Windows 的便携 CPU、CUDA sm_89、HIP gfx1102/gfx1103 配置均编译通过。SYCL 工程在顶层 CMake 中提前返回，原来没有网络后端目标，此阶段未给 SYCL 增加网络功能；Linux 尚未实机编译/运行。
- CPU-only 的 15 项新测试全部通过；CUDA 和 HIP 构建的 14 项通用测试通过，测试专用故障 DLL 只在 CPU 构建中验证。所有这些 worker 运行检查使用 CPU 后端，没有运行新的 CUDA/HIP GPU 任务。
- 本机 IQ3_S 的 7 种格式、1/3/8 个计算位置、普通/汇总模式，共 42 组 Python/native 返回字节相同。现有 C++ coordinator 的两种模式各 4 轮对照最大绝对误差为 0。本机测试不是实际 LAN 性能测量。
- 在子进程 PATH 只含 Windows System32 的条件下，原生 worker 完成认证和专家任务；也验证了 Unicode 路径及 UTF-8 密钥。测试驱动本身仍用 Python，不把这项检查算作整套集群无 Python 验收。
- 现有 Python 的配置、预加载和有限数/汇总测试共 36 项通过。现有服务与部署包未切换；主机 API、tokenizer、启动器和打包入口的迁移仍未完成。
- 第 2 步新增显式启用的 `strata-cluster`：共享计划校验、原生 worker 启动、coordinator 配置生成、临时私有配置和原生 API 启动接口。保留 Python 入口及默认计算路径；`STRATA_BUILD_NATIVE_CLUSTER` 仍默认为 OFF。
- coordinator 默认寻找同目录的 `strata-server`，第 4 步尚未提供该服务；此时启动明确报错，不会把现有引擎或简化接口当作原生 API。`--export-config` 只创建新文件，复制现有 server 设置后按共享计划生成参数；`--api-exe` 为原生服务路径覆盖。
- Windows 子进程先挂起创建，加入 kill-on-close job 后才运行；转发 Ctrl+C/Ctrl+Break，10 秒后强制结束不响应的进程树。正常退出和启动失败清理临时配置；launcher 被强制结束时，子进程树由 job 清理，可能留下仅 owner 可读写的临时目录。
- 本轮按用户要求仅验证 Windows；Linux 分支仍需构建和实机生命周期验证，SYCL 的网络接入边界与第 1 步相同。尚未切换服务或部署包，也未测量性能。
- 第 2 步 Windows 便携 CPU、CUDA sm_89、HIP gfx1102/gfx1103 的启动器构建通过。新启动器测试 16 项通过（另 1 项 POSIX 权限检查跳过），覆盖 Python 配置对照、输入不变、Unicode 路径、owner-only ACL、退出码、停止超时、启动失败及 launcher 强制结束后的孙进程清理。真实原生 worker 使用合成专家包和 CPU 后端，在子进程 PATH 仅含 System32 时完成认证和任务；现有共享配置 17 项和原生 worker 15 项回归通过。未运行 GPU 任务或真实主机 API。
- 默认路径检查：现有引擎和 Python 入口源文件未改动，新目标仅在 `STRATA_BUILD_NATIVE_CLUSTER=ON` 时添加；本轮没有改变默认参数或计算代码。
- 第 3 步完成：新增 `strata_text_runtime` 和 `strata-text-check`，提供 pack tokenizer 读取、按 rank/位置排序的 heap BPE、Qwen35 Unicode 预分词、CONTROL/USER_DEFINED token 和普通文字 span、增量 UTF-8、停止字符串、OpenAI/Anthropic 消息归一化、Jinja 模板、思考/工具调用流式解析和结构化输出验证。普通文字 span 使用 UTF-8 字节偏移；与 Python 字符偏移的转换已对照。
- Unicode 和 Jinja 使用现有 llama.cpp 依赖中的实现；JSON Schema 使用固定的 jsoncons v1.7.0，下载校验 SHA256，可通过 `STRATA_JSONCONS_DIR` 使用离线源码。schema 在运行引擎前按其 meta-schema 校验，只接受本地引用；生成未自然停止、JSON 重复字段、非法 JSON 和 schema 失败均报错。许可证复制至构建目录 `native-text-licenses/`，第 5 步需纳入部署包。
- 第 3 步 CPU 对照测试 15 组全部通过，无跳过：727 次完整 token ID 对照包含固定 32 个 golden case、合成词表和真实 IQ3_S 词表；10 个 `serve/chat_golden.json` case、Qwen3-Coder/Qwen3.5 和本机 pack 模板对照通过。984 次 parser 对照覆盖全部 37 个 `rcall_specimens`、跨 chunk 标签、参数内 XML、未完成调用、自然停止/取消、思考区调用、可选恢复及 41 项现有 server parser 回归。另覆盖 23 次请求归一化、131 次结构化输出、84 次停止字符串和 136 次 UTF-8 字节流对照。报告：`tmp/native-text-parity.json`。
- Windows 便携 CPU（IntelLLVM）、CUDA sm_89 配置（IntelLLVM）、HIP gfx1102/gfx1103 配置（ROCm clang）的文本目标均构建通过；SYCL 工程显式 `STRATA_BUILD_NATIVE_TEXT=ON` 的 host 文本目标也构建通过。SYCL 使用 Windows icx-cl，工程既有 `-fp-model=precise` 参数会被该驱动忽略并警告；这里只验证文本目标，未构建/运行 SYCL 引擎或增加 SYCL 专家网络后端。Linux、真实 GPU 推理及 Swift 专属模板尚未验证。
- 原生 worker 15 项和 launcher 16 项 Windows 回归通过，另 1 项 POSIX 权限检查跳过。System32-only PATH 下原生文本程序完成 tokenizer 和模板任务；Python 只用作测试驱动/行为对照。整套集群无 Python 启动仍待第 4/5 步验收。
- 第 3 步没有改动现有 Python 入口、模板文件或引擎计算源文件；普通 CPU/CUDA/HIP 工程仍由 `STRATA_BUILD_NATIVE_CLUSTER=ON` 启用新增目标，SYCL 的新增开关默认 OFF。没有更改默认参数、部署包或运行服务，也没有性能结论；原生主机 API 和引擎管理继续在第 4 步实现。
- 2026-10-10，第 4 步核心路径已实现：`strata-server` 复用第 3 步文本层和固定 llama.cpp 中的 cpp-httplib；cluster 显式构建自动包含该目标，SYCL 通过 `STRATA_BUILD_NATIVE_TEXT=ON` 和 `STRATA_BUILD_NATIVE_SERVER=ON` 构建主机服务。全部默认开关仍为 OFF；没有改动 Python 服务、现有引擎计算源文件、模型模板、默认参数或部署包。
- 原生服务提供 OpenAI chat、stateless Responses（含 namespace/custom tools、reasoning replay）、Anthropic messages/count_tokens、SSE、停止字符串、结构化输出验证、工具/思考输出、用量、timings、缓存计数、健康/状态、web 静态页面、配置备份/原子保存、共享设置和内存 API 请求记录。Windows 配置保存文件为 owner-only ACL；非 loopback 监听必须有 API key，拒绝 foreign Origin 和 loopback 上的不匹配 Host。工具协议输出支持不代表已迁移服务端 MCP 执行。
- Windows engine 先挂起创建并加入 kill-on-close job，显式限制继承的管道句柄，再运行；每次管道写入上限 15 秒。FIFO 内 STOP 排空至本次 DONE；故障或失步会结束子进程，不重放当前请求，下一请求重新读取 READY。思考预算和强制工具调用续写期间不让其他请求插队；自然结束、长度、停止字符串、取消和失步使用独立状态，不能读到旧请求 DONE。正常 unload/退出先发 QUIT，异常路径结束进程树。
- 原生硬件监控用 Windows 系统 API 读取 CPU/RAM，以动态加载驱动 NVML 的方式读取 NVIDIA GPU/VRAM/温度/功率/PCIe；不运行 Python、nvidia-smi 或其他采样子进程。AMD Windows 无读数时与 Python 一样显示缺失；Linux 代码尚未实机验证。完整 GPU 多卡、磁盘采样和 Prometheus histogram/标签等兼容项尚未完成。
- Windows 便携 CPU（IntelLLVM）、CUDA sm_89（IntelLLVM）、HIP gfx1102/gfx1103（ROCm clang）、SYCL 主机服务（icx-cl）均构建通过。SYCL 仍警告忽略既有 `-fp-model=precise`，没有构建/运行 SYCL 推理或新增其网络专家后端。没有运行 HIP GPU 推理，也未验证 Linux。
- 原生 API 21 项测试全部通过，Python 仅作驱动/对照：三种生成接口、完整 JSON/SSE、思考/工具、namespace、reasoning replay、预算续写、forced call、chunked body、重复 JSON 字段、认证、静态页面、停止、长度、上下文拒绝、FIFO、断开后下一请求、失败不重放、下一请求成功重启、静默超时、共享设置/配置保存均覆盖。System32-only PATH 下 launcher→原生 API→原生引擎夹具完成生成。报告 `tmp/native-server-parity.json`。文本层 15 组回归通过（727 次 token ID、984 次 parser 对照等），worker 15 项通过，launcher 16 项 Windows 检查通过，另 1 项 POSIX 权限检查跳过。
- 同日 Windows、i9-12900HK、RTX 4060 Ti 16 GiB、IQ3_S、同一 `build-intel/strata.exe` 0.1.41、context 8192、prefill 512、temperature 0 下，Python/native 真实输出和用量的 8 组对照全部通过。短提示词 18 token、长提示词 2029 token、多轮第 2 轮复用 17 token；真实 echo 工具调用、Responses、Anthropic、SSE 客户端断开后 fresh 请求通过。报告 `tmp/native-server-real-parity.json`；这是单机正确性验证，没有配置远端专家，没有速度结论。
- 核心路径提交 `63c8dfcb` 时，第 4 步尚未全部完成：MCP、vision、batch、生命周期、磁盘会话、VRAM 和完整监控仍待迁移。这是该提交时的历史边界，后续完整实现见下。

### 第 4 步完整实现记录（2026-10-10）

- 剩余主机服务功能已迁移到 C++：MCP stdio/Streamable HTTP(S) 及自动工具轮次、vision、batch admission/独立取消/solo 切换、短请求让行、VRAM 控制、磁盘 SAVE/RESTORE、lazy/idle unload、before_load、空闲 VRAM 门槛、安全设置、参数派生、reasoning recovery、Codex title/compaction、prefix、完整监控和 Prometheus。Python 保留为测试对照；服务自身和引擎子进程不依赖 Python。
- `EngineChannel` 单线程读取引擎输出，分发 control 和每槽 BT/BDONE；异常按进程代际失效所有请求，旧请求不能读到新进程的输出或结束新进程。STOP/BSTOP 排空、协议失步、写入/静默截止时间和 CPU/I/O/GPU 活动检查均保留。普通单请求使用 GEN，重叠时转 BGEN，独立取消不影响其他槽；图片编码与控制操作等待活动请求结束。
- MCP 支持初始化、工具分页、命名冲突处理、独立 server 启动、HTTP 会话/SSE、取消/超时/重启、结果字符上限和执行轮次；调用期间释放引擎位置。vision 保留原有原生 encoder 协议，支持 data/http/local 图片、EXIF 方向、白底透明合成、SHA256 缓存和当前请求 embedding 引用。WebP/AVIF/AOM 使用固定版本及 SHA256，离线源码覆盖和许可证路径见 `docs/MULTI_HOST.md`。
- `/v1/vram` 在模型卸载时记住 reserve，并在下次 READY 后应用；磁盘会话保留 SESSION/SWAIT/SERR 的进度、截止时间、错误类别和 published 标记，文件名校验覆盖 Unicode 和 Windows 设备名。磁盘 session 与 batch 不结合，与 Python 行为相同。
- 安全设置覆盖 allowed_hosts/trusted_origins/CORS、own-page 控制、API 多 key 和 UTF-8/Latin-1 header。子进程环境单独构造，GPU 顺序、split、HIP ordinal、TEMP/lib_dirs 和 expert profile 参数派生已对照。监控覆盖 NVML 多卡、Windows PDH 磁盘、全部 Prometheus 指标族/标签/histogram；API monitor 保留校验失败及执行失败记录，正文按 Unicode 字符截断，列表不带正文。
- 最终 Windows CPU、CUDA sm_89、HIP gfx1102/gfx1103、SYCL host 主机服务均构建通过。SYCL 仍有既有 `-fp-model=precise` 和 x64 stdcall 忽略警告；没有构建/运行 SYCL 专家网络后端，也没有运行 HIP/SYCL 推理或 Linux 构建。
- 原生 API 51 项检查全部通过，无跳过；覆盖上述功能、完整 JSON/SSE、错误重启、单请求/双槽断开、安全和图片格式，以及逐 token reasoning recovery 和 canonical structured output 对照。报告 `tmp/native-server-stage4-final.json`。最终文本层回归 15 组通过，727 次完整 token ID 和 984 次 parser 对照；报告 `tmp/native-server-stage4-text-regression.json`。worker 15 项、launcher 16 项 Windows 回归通过，另 1 项 POSIX 权限检查跳过。
- 同日 Windows、i9-12900HK、RTX 4060 Ti 16 GiB、IQ3_S、同一 `build-intel/strata.exe` 0.1.41、context 8192、prefill 512、temperature 0 下，单请求 8 项、双槽并行 9 项 Python/native 对照全部通过；包括短/长提示词、多轮、工具、Responses、Anthropic、断开后 fresh 请求和同时生成。报告 `tmp/native-server-stage4-real.json`、`tmp/native-server-stage4-real-batch.json`。没有配置远端专家，不构成 LAN 或速度测量。
- MCP/vision 的验证使用原生协议夹具及实际图片转换，没有测试外部 MCP 提供方或真实 GPU vision 编码。第 4 步实现已完成；第 5/6 步的无 Python 打包、部署切换、清洁环境/LAN 验收和性能测量仍保留未完成。没有停止现有服务或替换现有部署包。

### 正式目标收敛验证

Windows CPU/CUDA/HIP/SYCL host 构建通过；统一入口 4 项、worker 15 项、launcher 17 项（POSIX ACL 跳过 1 项）、API 52 项、文本 15 组和打包 13 项检查通过。三个原生包的 exe 数为 host16=2、host11=1、host17=2，项目 DLL 各 1 个，无 Python 文件；子命令在精简 PATH 下通过。CUDA/HIP 包的 launcher→serve 链路在无 Python PATH 下完成测试引擎请求。CUDA 引擎文件 SHA256 保持相同；HIP 当前构建原生开关 ON/OFF 字节相同。本轮没有实际图片编码模型、真实 GPU/LAN 推理、Linux 或性能验收，不切换现有运行服务。

### 第 5 步 Windows 实现和验证（2026-10-11）

- 按本轮要求只处理 Windows。新增独立 C++ 开发工具 `strata-cluster-build`，迁移计划/参数校验、隔离构建环境、构建日志、PE 普通/延迟导入依赖收集、HIP 设备数据和许可证、全部文件 SHA256 清单、暂存发布、旧包备份、失败回滚和逐节点摘要。构建进程使用 Windows job；取消后不发布当前节点、不继续后续节点。
- 原生入口 `.bat --native` 和 HIP `native-network` 不再调用 Python/pip/rocm-sdk；使用已安装 oneAPI/CUDA/Visual Studio 和已解包 ROCm SDK，支持离线依赖源码目录。默认 Python 打包/安装器保留。开发工具及测试程序不进入运行包。原生 HIP 直接发布节点目录，旧 ZIP 安装器模式保留给非原生入口。
- 本机 Windows、i9-12900HK/RTX 4060 Ti，host16 oneAPI `/O3 /QxHost`、CUDA sm_89；host11 oneAPI `/O3`、便携 AVX2；host17 ROCm Clang/MSVC ABI、便携 AVX2、gfx1102/gfx1103，三套完整原生目标构建和打包通过。没有运行 HIP GPU 或 SYCL 网络后端；没有更改引擎计算源码、模板、共享配置、默认引擎参数或现有服务。
- C++ 构建器 28 项测试通过，无跳过，覆盖参数/ISA 拒绝、PE/ELF 文件边界、普通/延迟依赖、递归依赖/缺失依赖、资源和清单、原子发布/回滚、并发锁、取消、Unicode/空格批处理路径、子进程日志、Python 和项目二进制数量限制。旧 Python 打包 13 项回归通过。
- 在打包进程 PATH 仅含 System32 时，通过 `.bat --native --package-only` 将三个最终包发布至 `dist/cluster-native`，没有创建/调用 Python。精简 PATH（包目录、rocm/bin、System32）下通过全部宿主子命令帮助、cluster/worker 配置校验和两个 GPU 引擎帮助。host16/host11/host17 分别验证 45/32/559 个文件 SHA256，exe 为 2/1/2，项目 DLL 均为 1，无 Python 文件。报告 `tmp/stage5-package-verification.json`。
- CUDA/HIP 包各通过 3 项 API/Web/内置 vision 入口检查，其中 launcher→serve→原生测试引擎请求使用 System32-only PATH；报告 `tmp/stage5-cuda-package-api.json`、`tmp/stage5-hip-package-api.json`。CPU 包的 worker 配置/协议检查 14 项通过，测试专用故障 DLL 因运行包不含诊断程序而跳过 1 项；包含认证、普通/汇总任务、缓存、预加载、Unicode 和无 Python PATH 下的真实原生 CPU 内核任务。缺失 SDK 的 HIP 原生入口失败检查也通过，未创建 SDK 或发布包。
- 这些是本机精简 PATH 和原生协议夹具验证，测试驱动可使用 Python；不是全新 Windows VM、真实模型生成、GPU 执行或 LAN 性能验收。新包已可部署，现有运行服务与启动配置未切换；Linux 和第 6 步仍待完成。
