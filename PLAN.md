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

- [ ] 实现原生 HTTP/SSE 服务及 `/v1/models`、chat completions、Responses、Anthropic messages/count_tokens、健康/状态/用量和现有 web app 路由。
- [ ] 迁移持久引擎进程协议、请求排队、取消、错误恢复、上下文检查、会话缓存和 engine 参数生成。
- [ ] 盘点并迁移多机部署实际启用的 server 依赖（包括工具/MCP 和监控入口）；未支持的配置明确拒绝，不能静默丢弃功能。
- [ ] 复用 web 静态资源；服务端不再启动 Python。
- [ ] API 行为对照与真实引擎短/长提示词、多轮、工具调用和客户端断开验证。

### 5. 原生构建/打包和部署切换

- [ ] 迁移 `build_cluster.py` 的计划、构建、依赖收集、清单、原子替换及失败处理；批处理/ shell 可作为轻量入口。
- [ ] Windows/Linux 包包含原生程序、必要 DLL/共享库、静态资源及许可证；不包含或依赖 Python。
- [ ] 三节点独立编译参数和 CPU ISA 下限不变，host11 不继承 host16 的 `/QxHost`。
- [ ] 清洁环境验证包启动；部署切换前保留可恢复的旧包，不自动停止现有服务。

### 6. 验收和性能

- [ ] 构建实际涉及的 CPU/CUDA/HIP/SYCL 后端；记录无法运行的硬件。
- [ ] 协议和配置交叉兼容、逐专家输出字节、汇总数值误差、tokenizer 和 API golden 对照通过。
- [ ] 同日 Python/C++ 热缓存往返和完整生成对照，记录 p50/p95、decode tok/s、首字时间、通信字节、CPU/RAM/VRAM 和日志设置。
- [ ] 新包在没有 Python 的环境启动并完成真实请求，更新 `docs/MULTI_HOST.md`。

## 完成标准

所有多机节点的部署和运行均不需要 Python；原有集群能力和客户端接口有对应的兼容验证；性能结论有可复现测量。仅完成原生 worker 不视为全部迁移完成。

## 当前记录

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
