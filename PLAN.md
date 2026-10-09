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

- [ ] `strata-cluster --config cluster.json --validate/--node ID`，替代 `cluster.py` 和 `make_network_config.py`。
- [ ] 跨平台子进程生命周期：Windows job containment、Linux 信号转发、停止和异常清理。
- [ ] 保持本地 server 配置及共享配置不被改写；worker 可直接运行；coordinator 在第 4 步完成后直接启动原生 API。

### 3. tokenizer 和请求/输出语义

- [ ] C++ 实现 byte-level BPE、Qwen35 Unicode 预分词、特殊 token、增量 UTF-8 解码和停止规则。
- [ ] 用现有 tokenizer corpus/golden fixture 对照完整 token ID；只验证 round-trip 不足以证明正确。
- [ ] 迁移聊天模板、工具定义/结果、思考开关和结构化输出解析，并对照 `serve/chat_golden.json`、`serve/fixtures` 和 server 测试。

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
