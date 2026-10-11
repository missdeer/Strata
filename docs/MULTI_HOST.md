# 按统一启动配置协作的 Windows 主机（实验版）

分支：`feature/multi-host-experts`。目标模型为 Qwen3.8-Flash-Next IQ3_S。
统一配置方案在启动时固定在线节点和专家范围，之后不自动发现机器、不自动变更分工。
这是同一个模型、同一条生成请求的分工，不是将三个完整模型放在负载均衡器后面。

## C++ 运行时迁移（第一至第四阶段，2026-10-10）

完整迁移计划和进度见 [PLAN.md](../PLAN.md)。目前已增加原生专家 worker、共享配置校验、集群启动器、
tokenizer/模板/输出层及原生主机 API，已完成第 4 步的可选功能。原生运行入口已合并到
`strata-host`；`tools/build_cluster.py --native` 收集不依赖 Python 的运行包。安装器和不带
`--native` 的旧打包路径继续使用 Python；完整真实 LAN 验收仍待第 6 步。

构建时显式设置 `-DSTRATA_BUILD_NATIVE_CLUSTER=ON`，然后构建 `strata-host`。
该选项默认为 OFF，需要现有原生专家和网络后端构建；CPU-only、CUDA 和 HIP 配置都支持该目标。
JSON 和 SHA256 使用项目固定版本的 llama.cpp 中已有的 vendor 源码，不额外下载依赖。
SYCL 的独立工程没有专家网络后端；可显式设置 `STRATA_BUILD_NATIVE_TEXT=ON` 和
`STRATA_BUILD_NATIVE_SERVER=ON` 构建相同主机 API。

辅助节点可以采用新的入口，继续使用相同共享 JSON 和 `STRATA_NETWORK_KEY`：

```powershell
strata-host.exe worker --cluster-config cluster.json --validate
strata-host.exe worker --cluster-config cluster.json --node host11
```

worker 按配置中的 `library` 加载对应后端 DLL/共享库，不自动替换该路径。
原生 worker 要求后端提供有限数校验和汇总入口，缺少时启动失败并要求更新后端。
它保留协议 v1、模型指纹、配置 SHA256、专家归属、认证和 coordinator IP 限制；
可以连接现有 C++ coordinator。该程序只运行 expert-worker，指定 coordinator 角色时明确报错。

它在任务之间复用输入、输出和路由缓冲区，使用 scatter send 直接发送结果。
单会话同步发送结束前不会复用结果缓冲区。RAM 缓存采用 LRU，任务保持所需专家的引用，
避免后续查找淘汰当前计算仍使用的指针。与 Python 缓存相同，预算约束缓存中的专家；
当前任务持有的已淘汰专家、映射页、计算缓冲区和库运行时还会占内存。
完整预加载仍先检查预算、可用 RAM 和提交空间，留 8 GiB 余量，再开始监听；加载过程可取消。

逐任务日志默认关闭，`STRATA_NETWORK_PROFILE=1` 可启用解析、准备、计算、校验、汇总和发送计时。
尚无该原生 worker 的速度对照，不能从减少复制推算生成速度的提升。

验证命令（测试驱动使用 Python，worker 不使用 Python）：

```powershell
python tools/test_native_cluster.py --worker build-network/strata-host.exe --library build-network/strata_network_backend.dll --pack 本机IQ3_S目录 --native 本机第一片GGUF
```

CPU 构建增加 `-DSTRATA_BUILD_TESTS=ON` 并构建 `strata-network-test-backend`，可运行计算失败、
无穷输出和汇总失败的注入检查；该测试 DLL 不应进入部署包。

本机 Windows 验证：便携 CPU、CUDA sm_89、HIP gfx1102/gfx1103 配置编译通过；CPU 的 15 项
配置/协议/预加载/故障测试通过。CUDA、HIP 编译的程序各通过 14 项通用测试，均采用 CPU 后端，
没有执行新的 GPU 任务。IQ3_S 的 7 种格式、1/3/8 个计算位置和两种响应模式共 42 组对照
与 Python worker 返回字节相同；现有 C++ coordinator 的普通/汇总模式各 4 轮最大绝对误差为 0。
PATH 只含 Windows System32 时，原生 worker 完成认证和任务，也通过 Unicode 路径与密钥检查。
这些检查没有测量真实 LAN 吞吐，没有验证 Linux，也没有切换现有部署。

### 原生集群启动器（第 2 阶段，Windows）

在工作目录运行，配置中的相对路径相对于该目录解析；角色程序从启动器所在目录寻找。
本地 server 配置和共享 JSON 不会被改写，配置摘要继续与 Python 一致。

```powershell
build-network/strata-host.exe cluster --config cluster.json --validate
build-network/strata-host.exe cluster --config cluster.json --node host11
build-network/strata-host.exe cluster --config cluster.json --node host16 --export-config coordinator-native.json
```

worker 启动要求环境变量 `STRATA_NETWORK_KEY`，仍直接使用共享配置中指定的后端库。
`--export-config` 迁移 Python 入口的 coordinator 配置生成，可在没有网络密钥时检查配置；
输出文件只允许新建，不能覆盖已有文件，也不能覆盖输入文件或其别名。
输出可能包含本地配置原有的 API key，Windows 为文件设置仅 owner 可读写的 ACL；
网络密钥只通过环境传递，程序不把它写入 JSON 或日志。

coordinator 继续监听 `127.0.0.1`，保留本地 server 设置中的 API 认证。
`--exe`、`--prefill` 和 `--no-api-key` 的含义与 Python 集群入口一致，仅适用于 coordinator；
`--no-api-key` 同时清除子进程环境中的 `STRATA_API_KEY`。
启动 coordinator 时重新运行当前宿主的 `serve` 子命令，worker 使用当前宿主的 `worker` 子命令；
不会按旧名字寻找其他角色 exe。`--api-exe` 仍可指定另一宿主，启动时同样使用 `serve` 子命令。
第 4 阶段已提供该服务；缺少它仍会明确失败。配置导出和测试子进程不能算作真实模型/LAN API 验收。
原生服务的启动接口为 `--engine strata --config 临时配置 --port 端口`。

### 两个 exe 的原生部署包

`strata-host.exe` 提供 `cluster`、`worker`、`serve`、`vision` 四个子命令；另一个正式 exe
是现有推理引擎 `strata.exe`。专家后端仍为唯一项目 DLL `strata_network_backend.dll`。
辅助节点只需要宿主和专家 DLL。测试、设备诊断、网络校验程序不进入原生运行包。
第三方 CUDA/HIP、Intel、MSVC 运行库仍按依赖收集，不计入项目 DLL 目标数。

```powershell
tools/build_cluster.bat --native --hosts host16 host11 host17 --out-root dist/cluster-native
# 复用已开启 STRATA_BUILD_NATIVE_CLUSTER 的三个编译目录：
tools/build_cluster_native.bat --package-only --build-dir host16=build-intel --build-dir host11=build-network --build-dir host17=build-cluster/host17-core --out-root dist/cluster-native
strata-host.exe cluster --config cluster.json --node host16
strata-host.exe worker --cluster-config cluster.json --node host11
strata-host.exe serve --engine strata --config strata-iq3_s.json
strata-host.exe vision --mmproj mmproj.gguf --model model.gguf
```

`tools/build_cluster.bat --native` 显式选择 C++ 构建器；不加该参数的 Python 打包和安装器路径保留。
直接运行 `python tools/build_cluster.py --native` 仅保留为迁移前的打包行为对照。
打包器收集 Web、模板、许可证和
示例配置，并拒绝额外 exe、额外项目 DLL 或 Python 文件。不要把整个测试构建目录复制进部署包。
原生图片配置保留 `mmproj`、`model`，省略 `vision.exe` 时，服务运行自身的 `vision` 子命令。
已有外部编码器配置仍可使用；将 `vision.exe` 指向当前宿主也会自动加入 `vision` 子命令。
内置编码器复用当前 CPU ggml；`--gpu` 在没有 GPU 编码后端时明确失败。图片模型不随程序打包。
SYCL 可构建同一个宿主，但该独立工程仍不生成专家网络 DLL。

本次 Windows 验证：CPU（oneAPI 2026.1）、CUDA sm_89、HIP gfx1102/gfx1103（ROCm
10.2.0a20260930）、SYCL host 配置构建通过。统一入口 4 项、worker 15 项、launcher 17 项
（跳过 POSIX ACL 1 项）、API 52 项、文本 15 组、打包 13 项检查通过。
CUDA 包和 HIP 包的 launcher→serve 链路在 PATH 仅含 Windows System32 时完成测试引擎请求；
三个包的全部子命令及两个 GPU 引擎的 `--help` 在包内运行库加 System32 的 PATH 下通过。
host16/host17 各 2 个 exe、1 个项目 DLL，host11 为 1 个 exe、1 个项目 DLL；三个包均无 Python 文件。
CUDA 引擎与本次改动前的文件 SHA256 相同；HIP 当前构建在原生开关 ON/OFF 下字节相同。
没有运行真实 GPU 推理或完整 LAN，没有图片 projector 可用来验收实际图片编码，没有验证 Linux，
也没有测量性能或切换现有部署服务。

### C++ 构建和发布入口（第 5 阶段，Windows）

`tools/build_cluster_native.bat` 首次用 CMake/Ninja 和 Visual Studio C++ 工具构建
`build-cluster/builder/strata-cluster-build.exe`，随后直接运行该工具。它是开发工具，不进入节点包。
也可自行构建 `tools/cluster_build` 后直接执行；`--dry-run` 在构建器已存在时只读计划，不写文件、
不调用工具、不下载。轻量批处理入口在首次使用时仍需编译构建器。

Windows 上的 host16 保留 oneAPI `/O3 /QxHost`、CUDA sm_89；host11 保留 oneAPI `/O3` 和
便携 AVX2；host17 使用 ROCm Clang/MSVC ABI、便携 AVX2，默认 gfx1102/gfx1103。三个编译目录
互相独立；C++ 工具在打包前检查缓存的后端、编译器、CPU 参数和 GPU 架构，拒绝 host11 的
`/QxHost` 或 ggml AVX-512/AMX 设置。SYCL 没有集群网络后端，不在这些节点包的构建范围。

构建前需安装 oneAPI、CUDA 和 Visual Studio，HIP 需提供已解包的 ROCm SDK。原生构建器不调用
Python、pip 或 `rocm-sdk`，不安装 SDK。`--rocm-root` 指向含 `bin`、`share/doc`、`.kpack` 的 SDK，
`--rocm-compiler-root` 指向含 `lib/llvm/bin/clang++.exe` 的编译器根目录。兼容已有 TheRock 解包目录；
可自动发现 `.rocm-win/Lib/site-packages/_rocm_sdk_devel` 和相邻 `_rocm_sdk_core`，不会运行其中的 Python。
离线依赖源码可用 `--ggml-dir`、`--jsoncons-dir`、`--webp-dir`、`--avif-dir`、`--aom-dir` 指定，
版本和 SHA256 规则仍由项目 CMake 管理。其他 DLL 搜索目录可重复指定 `--runtime-dir`。

```powershell
build-cluster/builder/strata-cluster-build.exe --dry-run --hosts host16 host11 host17
build-cluster/builder/strata-cluster-build.exe --hosts host17 --rocm-root D:/ROCm/sdk --rocm-compiler-root D:/ROCm/compiler
# 原 HIP 入口的 native-network 模式也转入 C++ 构建器：
tools/hip/build_windows.bat native-network
```

原生 HIP 模式发布到 `DIST_DIR/host17`（默认 `dist/cluster-native/host17`）；不再生成旧安装器使用的
`strata-windows-x64-hip.zip`。普通、`tests` 和 `network` 模式保留旧的 SDK 安装及 ZIP 行为。
C++ 工具直接读取 PE 普通/延迟导入表，递归收集 DLL；HIP 同时收集动态加载的 amd_comgr、
rocBLAS/hipBLASLt 设备数据、所选架构的 `.kpack` 和许可证。HIP runtime 的依赖放在 exe 旁，
BLAS 及其相对路径数据保留在 `rocm/bin`。主机配置使用 HIP 引擎时将该目录加入 `lib_dirs`。

输出先在同一输出根目录下的随机暂存目录完成，校验通过后才发布。`CLUSTER_BUILD.json` 记录
编译参数和程序/全部资源的 SHA256，`build-summary.json` 记录每个节点成功或失败。已有输出只有
匹配该节点的清单才可替换；替换前移到 `host*.previous-*`，发布失败尝试恢复。多个节点逐个发布，
一个节点失败不撤回其他节点已成功的包。每节点锁拒绝并发构建；强制结束后需确认构建进程已退出，
再删除空的 `.cluster-lock-host*`，不会自动抢锁。重定向的节点目录和互相包含的构建/输出目录被拒绝。
此工具不停止服务。切换服务前保留旧包，手动停止相应节点后修改启动路径；回退时恢复旧包及原配置。

不需要 GPU、Python 或下载的构建器测试：

```powershell
cmake -S tools/cluster_build -B build-cluster/builder -G Ninja -DSTRATA_BUILDER_TESTS=ON
cmake --build build-cluster/builder
ctest --test-dir build-cluster/builder --output-on-failure
```

本轮按要求只处理 Windows。Linux 构建、共享库收集和发布验证仍未完成；不能将 Windows 的
精简 PATH 启动检查算作 Linux 或真实三机 LAN 验收。

2026-10-11 在本机 Windows、i9-12900HK、RTX 4060 Ti 上，C++ 构建器的 28 项测试、旧 Python
打包的 13 项回归通过；host16 CUDA sm_89、host11 便携 CPU、host17 HIP gfx1102/gfx1103 的完整
原生目标构建及打包通过。`dist/cluster-native` 的三个包分别验证 45/32/559 个文件 SHA256，
项目 exe 为 2/1/2，项目 DLL 各 1 个，无 Python 文件。打包在 System32-only PATH 下完成；宿主
子命令和引擎帮助使用包内 runtime 加 System32 的 PATH，通过全部启动检查。
CUDA/HIP 包各通过 3 项 API/Web/内置 vision 入口检查，launcher 链路用原生测试引擎完成请求；
CPU 包的 14 项配置/协议检查通过，1 项诊断 DLL 检查跳过。未在全新 Windows VM 上测试，
未执行 HIP GPU、真实模型/LAN 推理或性能对照；现有服务继续使用原配置。
报告保存在 `tmp/stage5-package-verification.json` 和 `tmp/stage5-{cuda,hip}-package-api.json`。

### 原生主机 API（第 4 阶段，2026-10-10）

显式开启 `STRATA_BUILD_NATIVE_CLUSTER` 时构建统一的 `strata-host`。
直接运行时可先检查配置，检查不启动引擎、不监听端口：

```powershell
build-network/strata-host.exe serve --engine strata --config strata-iq3_s.json --validate
build-network/strata-host.exe serve --engine strata --config strata-iq3_s.json --port 18085
```

服务需要 pack 的 tokenizer 和 chat template，以及现有 `serve/web` 静态资源；可以用配置
`web_root` 或 `--web-root` 指向 web 目录。模板默认先寻找 tokenizer 目录内的 `chat_template.jinja`，
否则使用 web 目录上级的同名文件；配置 `template` 可以覆盖路径。`--native` 打包收集这些资源和许可证。
cpp-httplib 使用项目固定版本的 llama.cpp vendor。原生图片转换使用固定版本 libwebp 1.6.0、
libavif 1.4.2、libaom 3.14.1，CMake 下载时验证 SHA256；可分别用 `STRATA_WEBP_DIR`、
`STRATA_AVIF_DIR`、`STRATA_AOM_DIR` 指向这些版本的离线源码。许可证和专利文件复制到构建目录
`native-text-licenses`，`--native` 收集进包。Windows HTTP 客户端/图片转换使用 WinHTTP/WIC；
Linux 分支需要 libcurl/libpng，尚未实机构建或运行。

支持 OpenAI chat completions、stateless Responses、Anthropic messages/count_tokens，以及 JSON/SSE、
思考/工具调用、namespace/custom tools、reasoning replay、思考预算、强制工具调用、停止字符串、
结构化输出验证及 canonical JSON、用量、timings 和缓存计数。普通工具调用由客户端执行；
配置 MCP 后，chat 请求可用 `strata_mcp: true` 让服务执行 MCP 工具并继续生成。
`/v1/models`、健康/状态、`/props`、`/slots`、`/v1/vram`、`/mcp`、web 静态路由、settings/config、
load/unload、API monitor 和完整 Prometheus 指标可用。共享设置写入配置旁的 `.shared-settings.json`；
配置编辑仅允许服务已支持的字段，先保存 `.bak`，再以私有临时文件原子替换。

未启用并行时，常驻引擎按 FIFO 一次生成一条请求。取消时 STOP 并读到本次 DONE 后才放行；
配置 `parallel` 后支持 batch admission、每槽独立取消、短请求让行、缓存前缀/LRU 槽选择及
solo/batch 切换，只有一个请求时使用 GEN 路径。断开、协议错误或引擎崩溃不重放当前请求，
下一请求重启引擎；失步前排入的旧 DONE 不会成为下一请求的结果。多段思考/强制工具续写保留请求位置。
原生服务保留引擎自身的 prompt/conversation cache，API 的 cached_tokens 来自本次 DONE。
启动、静默、STOP 和管道写入有截止时间；Windows job 在服务被强制结束时清理引擎子树。

API key 支持配置及 `STRATA_API_KEY`，请求使用 Bearer 或 x-api-key。非 loopback 监听必须配置 key；
密钥不会出现在健康信息、监控响应或配置视图。支持 `allowed_hosts`、`trusted_origins`、
`cors_origins` 和 CORS 预检；无 key 时限制 foreign Origin 和不匹配的 Host。有 key 时按 Python
入口的认证/CORS 规则处理 API；配置编辑、加载、磁盘会话和 MCP 执行仍要求自己的页面，foreign
页面不能选取本地图片文件。

MCP 配置沿用 `mcp_servers`/`mcpServers` 和 `mcp` 选项，支持 stdio、Streamable HTTP(S)、
会话、分页、取消、结果长度限制及工具轮次。每个 MCP server 串行执行自己的 RPC，调用期间释放
引擎位置。`vision` 使用原有原生编码器的 READY/ENC/OK 和 GENI/BGENI 协议，支持 data URL、
HTTP(S) 和本地图片、EXIF 方向、透明像素白底合成及 SHA256 缓存；请求持有其所需 embedding，
超过缓存容量也不会丢失当前图片。服务自身的实现不启动 Python。

支持 `lazy_load`、`idle_unload_s`、`before_load`、`min_free_vram_mib`，以及 GPU 排序、multi-GPU
split、HIP ordinal、expert profile 参数派生、reasoning close/loop recovery、tail effort、
Codex title/compaction 和 `strata_prefix`。用 `slot_save_path` 或 `--slot-save-path` 开启
`POST /slots/0?action=save|restore`，文件名限制为目录内的普通文件名；与 Python 一样不和 batch
结合。`vram_elastic` 开启后，`/v1/vram` 可调整 reserve；模型卸载时设置的值在下次 READY 后应用。
未知配置字段仍在引擎启动前报错。

CPU/RAM 监控使用系统 API，NVIDIA 多卡监控动态加载 NVML，Windows 磁盘监控使用 PDH；不启动
采样子进程。NVML 不可用和 AMD Windows 未有读数时返回缺失值。Prometheus 包含指标族、标签和
histogram；API monitor 内存保留最多 100 条请求，生成历史最多 500 条，列表不返回请求/输出正文。

验证（Python 只作驱动和行为对照）：

```powershell
python tools/test_native_server.py --exe build-network/strata-host.exe --engine build-network/strata-server-test-engine.exe --report tmp/native-server-stage4-final.json
python tools/test_native_server_real.py --exe build-network/strata-host.exe --config tmp/dual-host-test/server.json --report tmp/native-server-stage4-real.json
python tools/test_native_server_real.py --exe build-network/strata-host.exe --config tmp/dual-host-test/server.json --parallel 2 --port 18088 --report tmp/native-server-stage4-real-batch.json
```

第一个命令需 CPU 测试构建的 `STRATA_BUILD_TESTS=ON`，并构建 `strata-server-test-engine`、
`strata-server-batch-test-engine`、`strata-mcp-test-server`、`strata-vision-test-encoder` 和
`strata-server-check`；测试夹具不进入部署包。图片对照需要测试环境中的 Pillow。
后两个命令顺序启动两套隔离的 loopback 服务，使用同一真实引擎；临时配置将 context 设为 8192、
prefill 设为 512，不改输入配置或停止已有服务。它不产生性能结论。

Windows 本机 51 项 API 检查、15 组文本回归、15 项 worker 和 16 项 launcher 检查通过；launcher
另跳过 1 项 POSIX 权限检查。System32-only PATH 下 launcher→原生 API→原生引擎夹具完成生成。
CPU、CUDA sm_89、HIP gfx1102/gfx1103 和 SYCL 主机服务目标均构建通过；SYCL 不包含新专家网络后端。
同日 i9-12900HK、RTX 4060 Ti 16 GiB、IQ3_S、同一引擎 0.1.41 下，单请求 8 组、双槽 9 组
真实检查通过：短/长提示词、多轮、工具调用、Responses、Anthropic 的输出与用量相同，
SSE 断开后下一请求成功，双请求并发输出与 Python 相同。
长提示词为 2029 token，多轮复用 17 token。MCP 和 vision 使用原生夹具验证协议、轮次及图片转换，
没有测试外部 MCP 提供方或真实 GPU vision 编码。没有运行 HIP/SYCL 推理、Linux 或真实 LAN API 性能测试，
没有切换部署包，也没有无 Python 的完整部署包验收。

Windows 角色进程先挂起创建，加入 kill-on-close job 后再运行，避免创建孙进程时逃出 job。
Ctrl+C/Ctrl+Break 转发给角色进程组；10 秒不退出则结束整个 job。
角色退出后也清理仍存活的孙进程，角色退出码传回调用者。
正常退出和启动失败会移除私有临时配置；强制结束启动器时 job 仍清理进程树，
但可能留下仅 owner 可读写的临时目录。程序直接传递 argv，不通过 shell。

启动器的测试使用原生角色/孙进程夹具和合成稀疏专家包，不需要模型下载或 GPU：

```powershell
# CPU 测试构建开启 STRATA_BUILD_TESTS，并构建 strata-cluster-test-role。
python tools/test_native_launcher.py --launcher build-network/strata-host.exe --role build-network/strata-cluster-test-role.exe --library build-network/strata_network_backend.dll
```

本轮 Windows CPU、CUDA sm_89、HIP gfx1102/gfx1103 配置已构建启动器。
CPU 构建的 16 项启动器测试通过，另 1 项 POSIX 权限检查跳过；
覆盖配置对照、输入文件不变、Unicode 路径、ACL、退出码、启动失败、停止超时和孙进程清理。
真实 worker 的合成专家任务在子进程 PATH 仅含 System32 时完成认证与计算，采用 CPU 后端；
共享配置 17 项和原生 worker 15 项回归通过，没有执行 GPU 任务或真实主机 API。
Linux port 按本轮要求暂不验证，SYCL 独立工程仍无网络运行时目标。
现有 Python 启动入口和部署包继续保留；本阶段没有真实性能测量。

| 节点 | 地址 | 分工 |
| --- | --- | --- |
| 主机：Windows 11、i9、32 GB RAM、RTX 4060 Ti 16 GB | 172.16.0.16 | API、调度、注意力、DeltaNet、KV、共享专家、路由、输出头、MTP、本地热门专家、长提示词处理 |
| CPU 辅助机：Windows 10、i7-10700、64 GB RAM | 172.16.0.11:18500 | 使用 AVX2 专家池，范围由共享配置决定；两机时负责全部 miss |
| 笔记本：AMD CPU、32 GB RAM、RX 7600M XT 8 GB 外置显卡 | 172.16.0.17:18500 | 配置为在线时，用 HIP 内核和有上限的显存缓存计算指定范围 |

每层把两台 worker 的任务同时提交，与主机 GPU 的热门专家计算重叠。每个 worker 接收一份 FP32
激活和自己负责的路由条目；权重从 worker 的本地文件读取，不通过网络搬运。空任务不发包。
默认返回各专家的 FP32 输出，由主机按原路由顺序加权一次。
`network_reduce: true` 可改为远端加权汇总，每个计算位置返回一个向量，见下文的第二批优化。
CPU 和 GPU 内核之间仍可能有舍入差异，不能把保持合并顺序当成整个异构系统逐位一致的保证。

## 统一启动配置（推荐）

每台机器启动 `tools/cluster.py` 时读取同一份 JSON，只执行自己 `--node` 指定的角色。
共享配置包含各节点的在线状态、IP、CPU/GPU 后端、线程数、RAM/VRAM 缓存预算、本地模型/库路径及专家范围。
主机生成本次启动的引擎参数，worker 从同一文件取参数。共享文件和已有单机 server 配置都不会被改写。
密钥仍通过各机的 `STRATA_NETWORK_KEY` 设置，不写进共享 JSON。

三份已提供的方案：

| 文件 | 在线主机 | 每一层未命中主机 GPU 缓存的专家分配（闭区间） |
| --- | --- | --- |
| [cluster-2hosts.json](../configs/cluster-2hosts.json) | 16、11；17 明确为 offline | 11：0–511 |
| [cluster-3hosts.json](../configs/cluster-3hosts.json) | 16、11、17 | 11：0–255；17：256–511 |
| [cluster-4hosts.example.json](../configs/cluster-4hosts.example.json) | 16、11、17、新机器 | 11：0–255；17：256–383；新机器：384–511 |

专家 ID 范围对所有层生效，区间端点包含在内。主机缓存命中优先在 4060 Ti 计算；只有 miss 按范围发送。
这些范围是明确的初始分配，尚未基于节点吞吐量校准。四机例子的 `172.16.0.18`、CPU 后端和预算都是
示例值，新机器加入时必须改成实际参数；硬件不同可以分配不同数量的专家，多段范围也可以放到一个节点。

比如 17 暂时不参与：停掉当前 Strata 进程，采用两机配置，将 17 的 `online` 设为 false、11 的范围设为
`[[0,511]]`，把修改后的完整文件复制到所有参与机器，再按下面步骤启动。不能只关闭 17 而留下无人负责的范围。
启动期间或运行期间更改 JSON 都不影响已运行进程，必须停止并重启参与节点才能切换配置。

配置校验会拒绝：未知字段、重复节点 ID/IP、多个主机、没有主机、在线 worker 的范围重叠、专家遗漏、
非法预算和指定为离线的本机。最多 16 个在线 worker 加 1 个 coordinator。
节点按稳定 ID 排序决定网络编号；无需手工维护 index/count。
连接握手校验共享配置的 SHA256 和完整专家归属表，以及原有模型指纹。
仅格式、缩进或 JSON 对象字段顺序变化不影响 SHA256；路径、预算、online、范围、revision 等内容变化会影响。
配置不同的节点拒绝连接。声明在线的节点实际没启动或不能连接时，主机启动失败，不会偷偷改用其他分工。

先修改所选文件里的本地路径，并确认 AMD device 编号，然后将**整份相同文件**复制到各机，统一命名
`cluster.json`。路径是该节点本地的绝对路径，因此同一 JSON 可以包含三台机器各自不同的目录。
`host16.server_config` 指向已安装的单机 IQ3_S 配置，用来读取 tokenizer、模型和 MTP 参数；
`host16.engine` 必须是本分支编译的引擎。模型文件在各机器本地，不需要网络共享目录。

每台机器都先验证（不启动引擎、不访问模型、不连接网络）：

```powershell
python tools/cluster.py --config cluster.json --validate
```

三个进程设置同一个 `STRATA_NETWORK_KEY` 后，先启动配置中的 worker：

```powershell
# 172.16.0.11
python tools/cluster.py --config cluster.json --node host11
# 172.16.0.17，仅配置 online=true 时启动
python tools/cluster.py --config cluster.json --node host17
# 172.16.0.16，最后启动
python tools/cluster.py --config cluster.json --node host16
```

每机均打印相同的 `config_sha256`。两机方案只需启动 host11 和 host16；四机方案先启动三个 worker，
再启动 host16。API 保留 localhost 监听。worker 只接受配置中的 coordinator IP。

增加机器时：给 `nodes` 增加稳定 ID、IP、本地模型和库路径、后端和预算，标明 `online: true`，
从其他 worker 的范围划出它的 `expert_ranges`，让 0–511 恰好各有一个在线 owner，再更新 revision，
验证、分发同一文件、全部重启。无需修改或重新编译调度算法。

## 首版边界

- 原生 IQ pack、单路 `--serve` 解码，支持最多 8 个计算位置的 verify window 和 MTP。
- 网络 worker 不分配 KV、注意力状态、输出头或 PLE 表；旧 Intel 核显、笔记本集显和 NPU 不参与计算。
- 长提示词仍由主机原有 prefill 路径处理，读取主机本地模型文件；主机自动使用 `--mmap-experts`，
  不先申请完整 50 GB 专家 arena。初期用 8K 或 16K 上下文、512-token prefill 块。
- 网络模式强制 decode 的 PCIe miss share 为 0，关闭路由 lookahead 和 short-read，避免主机为远端任务
  额外读取专家。主机已有专家缓存和自适应缓存仍可使用。
- 暂不支持与同机 layer split、peer/helper GPU、batch slots 或 pipeline-windows 同用。
- 任一节点超时、断线、任务编号错误或结果无效时，当前推理报错。不会重用旧结果，也不会在当前层重试。
  重启引擎后重新认证连接；保持 worker 运行即可。还没有自动发现、故障接管、动态负载分配或跨主机 prefill。

## 模型文件与缓存

三台机器各自需要同一套 IQ3_S 的 `native_experts.txt` 和它指向的 GGUF 文件。
如果 pack 有完整 `experts.bin`，辅助机可直接使用它，不必再提供 GGUF。
没有 `experts.bin` 时提供 `--native` 的第一片路径，worker 根据表中的每个角色定位正确的分片，
并检查 GGUF 的张量名、类型、形状、偏移和文件边界。

辅助机使用映射文件和有上限的 RAM 缓存，不申请完整权重 arena。RAM 缓存默认 512 MiB，显存缓存默认
4096 MiB，都可调。映射文件的系统页缓存也占 RAM，不能把这些缓存预算当成整个进程的内存上限。
CPU 节点先试 24576 MiB RAM 缓存；笔记本先试 8192 MiB RAM 缓存和 4096 MiB VRAM 缓存。
这些是初始预算，不是性能测量。显存缓存按最近使用淘汰，未命中的专家经笔记本本地链路上传 GPU。

连接时校验版本、层数、专家数、分片编号和模型抽样指纹；统一配置启动还校验配置 SHA256 和归属表。
模型指纹包含每层格式及首末专家的首尾 4096 字节；
能发现不同量化文件和常见部署错误，但不是完整文件的加密校验。部署时仍应对复制的文件做完整 SHA256 对照。
同一模型的文件移动目录不会改变模型指纹。统一配置中的路径变化会改变配置 SHA256。

## 构建

### 一个入口构建三台机器并分别打包

在 16 本机的仓库目录运行：

```bat
tools\build_cluster.bat
```

默认顺序为 host16、host11、host17，每个节点使用独立 CMake 目录和部署目录：

```text
build-cluster/
  host16/                      oneAPI /O3 /QxHost + CUDA sm_89
  host11/                      oneAPI /O3、AVX2，无 /QxHost
  host17/                      ROCm Clang + MSVC ABI，gfx1102 + gfx1103
dist/cluster/
  host16/                      主机引擎、设备工具、网络后端、依赖 DLL、API 与启动脚本
  host11/                      CPU worker 后端、校验工具、worker 脚本
  host17/                      HIP 引擎与 worker、两个目标的 runtime/设备库、脚本及 HIP zip
  _logs/                       每次运行、每个节点的编译日志
  build-summary.json           本次运行各节点的成功/失败结果
```

每个部署目录包含 `CLUSTER_BUILD.json`（编译器、参数、目标架构、程序 SHA256）和 `RUN.txt`。
打包前检查 CMake 的目标配置，拒绝将带 `/QxHost`、AVX-512 通用代码或 native ggml 的产物标成 host11。
16 的 CUDA、Intel 数学库和 MSVC 依赖按 PE 导入表递归收集；17 沿用 HIP 包的 runtime、设备库和许可证打包。
部署目录不包含 Python 或模型；16 需要 `requirements.txt` 中的 Python 包，另外两机的 worker 只用标准库。
公共配置的路径仍需按实际部署位置编辑，并分发同一份 JSON。

再次构建会先完成新包，然后替换对应节点目录；原包保留在 `host11.previous-时间-编号` 等目录中。
已有目录不是本脚本生成的节点包时，脚本拒绝替换。一个节点失败会继续尝试其他节点，最终返回非零状态，
不会把失败节点的旧产物当成新构建成功。

常用选项：

```bat
tools\build_cluster.bat --dry-run
tools\build_cluster.bat --hosts host11
tools\build_cluster.bat --hosts host17 --hip-archs gfx1102
tools\build_cluster.bat --jobs 4 --build-root build-cluster --out-root dist\cluster
tools\build_cluster.bat --no-install-rocm
```

`--dry-run` 只显示计划，不创建目录、不编译、不下载。
17 第一次构建会安装私有 ROCm SDK（约 7 GB）；`--no-install-rocm` 在工具链缺失时直接报错。
默认同时编译 gfx1102 与实验性的 gfx1103；多目标代码不意味着网络 worker 已启用双设备调度。
`--runtime-dir DIR` 可以补充编译器或 CUDA 的 redistributable DLL 搜索目录。
`--ggml-dir DIR` 可指定项目固定版本的 llama.cpp 源码；默认优先复用仓库中已经存在的源码。

已有本分支编译目录也可复用，仍分别放入独立部署目录：

```bat
tools\build_cluster.bat --hosts host16 host11 --build-dir host16=build-intel --build-dir host11=build-network
tools\build_cluster.bat --hosts host16 host11 --package-only --build-dir host16=build-intel --build-dir host11=build-network
```

`--package-only` 仅支持已经编译好的 Intel 节点；HIP runtime 由 host17 的构建脚本一起生成。

本机验证：13 项构建/打包检查通过；host16、host11 已生成各自部署包。
host16 的 `--help` 和设备列举在 PATH 仅保留 Windows 系统目录时通过，确认随包依赖可加载；
host11 的部署包通过真实 IQ3_S 专家对照、1–4 个 worker 的静态分配及共享 JSON 启动检查。
统一入口的 host11 编译已验证 `/O3`、无 `/QxHost`。host17 的不安装模式已验证在 SDK 缺失时
返回失败并保留错误日志。host17 的完整 HIP 包现已构建通过，见下面的记录。

2026-10-08，本机已安装项目私有 HIP/ROCm SDK：`.rocm-win`，版本 `10.2.0a20260930`，
包含 core、devel、libraries、gfx1102 和 gfx1103 设备包。AMD Clang 24.0.0git 编译并链接了一个
同时包含 gfx1102/gfx1103 代码的 HIP 样例；Strata 的 `build-cluster/host17` 已通过 HIP CMake 配置。
安装与初始化没有修改显卡驱动或全局 CUDA/oneAPI 配置；本机没有进行 AMD GPU 内核运行测试。
可直接执行 `tools\build_cluster.bat --hosts host17 --no-install-rocm` 复用已装 SDK 编译。
SDK 根目录可用 `.rocm-win\Scripts\rocm-sdk.exe path --root` 查询。

完整 HIP 构建验证（2026-10-08）：`tools/build_cluster.py --hosts host17 --no-install-rocm --jobs 6`
完成全部 144 个构建步骤并打包，未修改推理源码。结果目录为 `dist/cluster/host17`，
压缩包 `strata-windows-x64-hip.zip` 为 153338286 字节（约 146 MiB）；包内运行文件约 429 MiB。
编译器 AMD Clang 24.0.0git，MSVC ABI，`-O3 -DNDEBUG`，CPU 使用便携 AVX2 基线；
ROCm 10.2.0a20260930、hipBLASLt 100500。`strata.exe` 与 `strata_network_backend.dll`
的 offload bundle 均包含 gfx1102/gfx1103，附带两个架构的设备库与完整 runtime DLL。

部署包的 `strata.exe --help` 在 PATH 只包含包内目录和 Windows 系统目录时成功；
16 项配置检查、真实 IQ3_S 的 CPU 专家对照、1–4 个 worker 的静态分配及从共享 JSON 启动 worker
均用该 HIP 包通过。这里的专家对照使用 CPU backend，没有执行 AMD GPU 内核；
外置卡/集显的正确性、完整 GPU 推理与多机千兆性能仍需在 17 及真实 LAN 验证。

### 分别编译的底层入口

按目标节点分别编译，不能把本机优化的 DLL 当作通用 DLL 复制给其他机器：

| 节点 | 编译器与参数 | 默认构建目录 |
| --- | --- | --- |
| 16（本机） | 主机 C++：Intel oneAPI `/O3 /QxHost /fp:precise`；CUDA：nvcc，CUDA host compiler 为 MSVC | `build-intel` |
| 11（i7-10700） | Intel oneAPI `/O3 /fp:precise`，无 `/QxHost`；`STRATA_PORTABLE=ON`、AVX2 基线 | `build-network-cpu` |
| 17（AMD CPU + RX 7600M XT） | AMD ROCm Clang 编译 C++/HIP，配合 MSVC 头文件、CRT、链接器；GPU 目标 gfx1102，CPU 使用便携 AVX2 基线 | 单独的 HIP 构建目录 |

16、11 的批处理入口会分别明确设置 Release 参数，避免复用 CMake 缓存时继承 `/QxHost`。
11 的专家专用 AVX-512 文件仍保留，运行时能力检查只在支持的 CPU 上选择它们；通用代码与 ggml 保持 AVX2 基线。

```bat
tools\build_cluster_intel.bat host16
tools\build_cluster_intel.bat host11
```

`STRATA_ONEAPI_VARS` 可指定 oneAPI 环境脚本；`STRATA_CLUSTER_BUILD_DIR` 可指定当前节点的构建目录，
不要让两个节点共用同一构建目录。`STRATA_BUILD_JOBS` 控制并行编译数，默认 6。
已有构建目录保留其原编译器路径，避免 oneAPI 的 `latest` 路径与版本目录之间切换导致 CMake 清掉参数；
`STRATA_ICX_COMPILER` 可以为新的构建目录明确指定 `icx-cl.exe` 路径。
CPU worker 不需要 CUDA、HIP、独显或完整引擎。如需自己配置，在 oneAPI 环境中运行：

```bat
cmake -S . -B build-network-cpu -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=icx-cl.exe -DCMAKE_CXX_COMPILER=icx-cl.exe "-DCMAKE_C_FLAGS_RELEASE=/O3 /DNDEBUG /fp:precise" "-DCMAKE_CXX_FLAGS_RELEASE=/O3 /DNDEBUG /fp:precise" -DSTRATA_PORTABLE=ON -DSTRATA_ENABLE_CUDA=OFF -DSTRATA_ENABLE_HIP=OFF -DSTRATA_BUILD_TESTS=OFF
cmake --build build-network-cpu --target strata_network_backend strata-network-check --parallel 4
```

`STRATA_PORTABLE=ON` 为 i7-10700 保留 AVX2 基线；不要使用在 i9 上带 `/QxHost` 编译的库替代它。
原生专家的 ggml 源码使用项目固定版本。离线构建可加 `-DSTRATA_GGML_DIR=本地llama.cpp源码目录`。

主机在已有 CUDA 构建目录中编译三个目标：

```bat
cmake --build build-intel --target strata strata_network_backend strata-network-check --parallel 4
```

AMD 节点按 [AMD_HIP.md](AMD_HIP.md#windows) 构建 HIP 引擎；此分支额外提供网络目标。
可以在本机 16 上离线构建给 17 使用的 Windows x64 包，不要求本机有 AMD GPU。
这是为另一块 GPU 指定目标代码，不是将 Intel CPU 指令翻译成 AMD CPU 指令。
主机 CPU 代码仍使用可移植基线，不使用 `/QxHost` 或 `-march=native`。
HIP 内核必须由 AMD Clang 编译；当前 CMake 的 Windows HIP 工程要求 C、C++、HIP 使用一致的 Clang
工具链，不能把 C++ 改成 `cl.exe` 再与 HIP Clang 混用。MSVC ABI、标准库和链接器仍参与构建。
参考 [AMD 的主机/设备编译说明](https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/hip_porting_guide.html)。

Windows 构建脚本的 `network` 模式会将后端 DLL 和校验程序放入 HIP 包：

```bat
set STRATA_HIP_ARCHS=gfx1102
set BUILD_DIR=D:\Projects\Strata\build-host17-hip
set DIST_DIR=D:\Projects\Strata\dist\host17
tools\hip\build_windows.bat network
```

`gfx1102` 是外置 RX 7600M XT 的目标，不是 HIP 唯一允许的目标。一个包可以包含多个目标架构。
如果 17 的集显确认是 Radeon 780M / 760M（gfx1103），可以尝试：

```bat
set STRATA_HIP_ARCHS=gfx1102;gfx1103
set BUILD_DIR=D:\Projects\Strata\build-host17-hip-dgpu-igpu
set DIST_DIR=D:\Projects\Strata\dist\host17-dgpu-igpu
tools\hip\build_windows.bat network
```

HP 的 [EliteBook 845 G11 官方规格](https://support.hp.com/cn-zh/document/ish_10397852-10415593-16)
列出 8840HS/8840U 等多种 Ryzen 8000 CPU；[32 GB、R7-8840HS 的具体配置](https://support.hp.com/us-en/product/product-specs/hp-elitebook-845-14-inch-g11-notebook-pc/model/2102139312?sku=AB1R1PT)
明确标注 Radeon 780M。AMD 的 [GPU 规格表](https://rocmdocs.amd.com/en/develop/reference/gpu-specs.html)
将 Radeon 780M 标为 gfx1103。因此用户描述的 2024 年锐龙战 X 很可能适用 gfx1103，但未读取 17 的实际硬件，
不能据此认定具体 CPU/SKU。最终在 17 用 `strata-device.exe` 确认 GPU 名称、架构和 HIP 编号。
编译工具和 runtime 的设备包必须覆盖两个目标。
项目 CMake 接受 gfx1103，但它是实验目标；Windows setup 没有为普通 780M/760M 默认启用支持。
加入编译目标只保证尝试生成相应代码，不能据此断言 17 的驱动能执行它或它能加速推理。

Strata 本身已有同机多 GPU 的 layer split 和辅助专家缓存/计算路径，见 [MULTI_GPU.md](MULTI_GPU.md) 及
`RemoteExperts`。本分支新增的轻量网络 worker 复用专家内核，但尚未接入多设备调度：当前共享配置的
每个 worker 只有一个 `device`，17 默认使用外置卡。这是网络适配器的现状，不是 Strata 引擎的多卡限制。
多架构包不会自动把任务分到两块 GPU。外置卡和集显同时参与时，应复用现有专家内核、设备 context、
stream 和辅助专家路径，让 17 对外保持一个网络节点，对内按配置分配到两个计算设备，各自有缓存预算。
17 只承担专家计算，原生完整 layer split 的注意力/KV/输出头状态不是这个 worker 的任务。
当前配置不能用两个相同 IP 的 node 绕过这一限制。
Windows HIP 的 setup 文档仍将 `--gpus` 标为 Linux-only；不能直接把已有 Linux 多卡启动命令当成
外置卡 + gfx1103 集显在 Windows 上已验证的方案。
集显从 17 的 32 GB RAM 中使用共享内存，不是增加独立显存容量；两个设备一起工作是否有收益需单独测量。

脚本会在本机的私有 `.rocm-win` 中安装 ROCm 开发包（约 7 GB），不用给本机安装 AMD 显卡驱动。
目标是 `dist/host17/strata-windows-x64-hip.zip`。复制到 17 后保留包内 runtime DLL、`rocm/` 和 worker 脚本，
在 17 上使用 AMD 显卡驱动运行。编译成功只能验证代码生成和链接，GPU 运行测试必须在 17 做。
已有正式版 `strata.exe` 不包含本分支的网络后端。
RX 7600M XT 的真实 HIP 运行、驱动版本与外置链路仍需在笔记本验证。

各机器需要本分支的源码，才能使用统一的 `tools/cluster.py` 入口。
独立 worker 所需文件是 `strata_network_backend.dll`、`tools/network_worker.py`、`tools/gguf_reader.py`
和 `tools/cluster_config.py`；只使用 HIP 包中的 worker 脚本时，可直接运行
`python tools/network_worker.py --cluster-config cluster.json --node host17`。
CPU 库采用静态 MSVC runtime；GPU 库还需要相应 CUDA/HIP runtime。HIP 包中的 DLL 和 `rocm/bin`
应保留在库的旁边。Linux 对应库名为 `libstrata_network_backend.so`。

## 密钥及旧版命令行启动

先生成一个至少 16 字节的随机密钥，将相同的值设到三个进程的 `STRATA_NETWORK_KEY`。建议生成 32 字节：

```powershell
python -c "import secrets; print(secrets.token_hex(32))"
$env:STRATA_NETWORK_KEY = '把同一个生成值填到三台机器'
```

TCP 协议使用密钥认证和客户端 IP 限制，尚未加 TLS；输入、结果和认证信息只应走可信私有局域网。
不要将 worker 端口转发到公网。Windows 防火墙只为主机 172.16.0.16 放行辅助机的 TCP 18500。
worker 的 `--api-key` 可以代替环境变量；未设置密钥时即使只监听 localhost 也拒绝启动。

在 **172.16.0.11** 上，把下面两条路径换成它本地的模型目录和第一片 GGUF：

```powershell
python tools/network_worker.py --pack D:/Strata-data/packs/iq3_s --native D:/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf --library build-network-cpu/strata_network_backend.dll --host 172.16.0.11 --allow-client 172.16.0.16 --index 0 --count 2 --backend cpu --workers 7 --ram-cache-mib 24576
```

在 **172.16.0.17** 上，使用解压 HIP 网络包中的库，`--device` 选择 RX 7600M XT 的 HIP 编号
（先用包内 `strata-device.exe` 列卡，不能假定它是 0）：

```powershell
python tools/network_worker.py --pack D:/Strata-data/packs/iq3_s --native D:/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf --library engine-hip/strata_network_backend.dll --host 172.16.0.17 --allow-client 172.16.0.16 --index 1 --count 2 --backend gpu --device 0 --gpu-cache-mib 4096 --ram-cache-mib 8192
```

以下是仍保留的旧版手工命令行模式，不校验统一配置；不要与 `tools/cluster.py` 启动的 worker 混用。
若先排查网络，可将笔记本改成 `--backend cpu`。要使用一个 worker，两端改为 `--count 1 --index 0`，
主机只列一个地址。三个 worker 时取专家编号模 3，三端的 count 与顺序必须一致。

在主机创建独立配置，保留现有单机配置：

```powershell
python tools/make_network_config.py --source strata-iq3_s.json --out strata-iq3_s-network.json --exe build-intel/strata.exe
python -m serve.server --engine strata --config strata-iq3_s-network.json
```

生成器默认指向 `172.16.0.11:18500,172.16.0.17:18500`，并把上下文设为 131072、prefill 设为 512。
API 仍监听 `127.0.0.1`。如需将 API 提供给其他机器，必须同时配置独立的 API key。
辅助机的专家密钥不自动充当文本 API 的密钥。

## 验证和测量

不需要 GPU 或模型的协议测试：

```powershell
python tools/test_network_experts.py
python tools/test_cluster_config.py
```

在同一台机器上启动两个临时 worker，并用真实 IQ3_S 前四层比较网络结果与本地专家池：

```powershell
python tools/test_network_experts.py --build build-network-cpu --pack D:/Strata-data/packs/iq3_s --native D:/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf
```

加 `--gpu-device N` 后第二个临时 worker 使用该 CUDA/HIP 设备；`--build` 要指向 GPU 构建目录。
加 `--engine-config strata-iq3_s.json` 会再执行完整引擎生成；`--engine-spec 0` 移除 MTP，
`--engine-spec 4` 保留 MTP。它会用 4096 上下文、512 个本地专家槽和很小的 worker 缓存制造网络 miss，
因此这个测试的速度不代表正常缓存下的性能。
校验程序输出最大绝对误差和参考最大值；当前接受条件为 `max_abs <= 1e-5 + 0.02 * reference_max`，
这是专家向量的原型检查，不是模型质量保证或 greedy token 一致性验收。

两台辅助机启动后，可以从主机直接检查实际 LAN：

```powershell
build-intel/strata-network-check.exe D:/Projects/Strata-data/packs/iq3_s 172.16.0.11:18500,172.16.0.17:18500 48 - D:/Projects/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf
```

上面的直接校验命令默认使用旧版取模握手。要检查统一配置启动的 worker，先用相同配置导出该校验工具的
归属表和配置 ID；引擎正常启动时 `tools/cluster.py` 已自动完成这些步骤：

```powershell
$clusterValues = python -c "import sys; sys.path.insert(0, 'tools'); from cluster_config import load_cluster; p=load_cluster('cluster.json'); print(p.assignment); print(p.digest); print(p.endpoints)"
$env:STRATA_NETWORK_ASSIGNMENT = $clusterValues[0]
$env:STRATA_NETWORK_CONFIG_ID = $clusterValues[1]
build-intel/strata-network-check.exe D:/Projects/Strata-data/packs/iq3_s $clusterValues[2] 48 - D:/Projects/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf
```

这个工具使用 1 秒任务 deadline；引擎默认为 5 秒，可用 `--network-timeout-ms` 调整。
worker 默认给半包接收和结果发送 60 秒，空闲连接可以保留。一个 worker 同时只服务一个 coordinator。

引擎每次完成请求会记录每个 worker 的累计任务数、路由条目、收发字节与往返时间；worker 单独记录
每层计算与返回耗时。往返时间包含计算和等待，不应再与 worker 的计算时间重复相加。
真实验收应分别记录 prefill tok/s、首字时间、decode tok/s、MTP 接受率、通信等待及各节点 RAM/VRAM。
先移除 `--mtp`（原生 IQ 路径仍保留 `--spec 2` 或更大的 verify window）测 8K/16K、固定 prompt 和
至少 256 个输出 token，再启用 MTP 对照；不要用回环网络的数字
承诺三机千兆链路的速度。

FP32 激活每 token 每 worker 10240 字节；首版响应是该 worker 的远端专家条目数乘 10240 字节。
例如每层共有四个远端专家，两个 worker 都有任务，一个位置跨 48 层的有效负载约为
`48 × (2 + 4) × 10240 = 2949120` 字节，另加元数据和 TCP 开销。
这比 [双机预研](../MULTI_HOST.md) 的远端先合并方案更大。是否改为合并响应和按实测耗时分配任务，
应在这一版完成数值及链路验证后决定。

### worker 输入、输出校验与计时（2026-10-09）

新版 worker 使用后端 DLL 的 `strata_network_all_finite` 检查 FP32 输入和输出，
拒绝 NaN 和正负无穷；不再在 Python 中逐个遍历浮点数。旧 DLL 没有这个入口时保留原来的
Python 校验，启动时打印实际采用的路径。协议、每专家输出顺序和数值计算不变。
路由、输入和输出的 ctypes 缓冲区在任务之间复用；返回结果仍是独立的 bytes，后续任务不会覆盖它。

在 16 的 i9-12900HK、Windows 上，用 host11 的便携 AVX2 DLL、7 个 pool worker、
本地 IQ3_S 权重做小工作集热缓存对照：5 种层格式、每种 1/3/4 个位置，共 15 组，
每组预热 5 次后测 40 次。3 个位置、18 条专家输出的完整 worker 调用中位数从
5.3–6.9 ms 降到 1.3–2.9 ms；新版输出校验中位数为 0.03–0.04 ms。
15 组优化前后的输出 SHA256 全部相同。此记录是同机 worker 调用，不是 11 的内核耗时、
真实 LAN 往返或完整模型的生成速度。

设置 `STRATA_NETWORK_PROFILE=1` 后启动 worker，每条任务日志追加 `parse_ms`、`prepare_ms`、
`native_ms`、`validate_ms`、`serialize_ms` 和 `send_ms`。
`parse_ms` 包含输入校验；`prepare_ms` 包含专家缓存查找和缓冲区准备；
`native_ms` 是后端调用；`validate_ms` 是输出校验；`serialize_ms` 是独立结果快照；
`send_ms` 是发送调用。原有 `compute_ms` 包含准备、后端调用、输出校验、快照和发送，
不包含输入解析和日志打印。发送调用不等于整个链路传输时间，不能把各节点计时重复相加。

新增校验检查可运行 `python tools/test_network_validation.py --library 路径/strata_network_backend.dll`，
覆盖 IEEE-754 有限数、NaN/Inf、非对齐输入、旧 DLL 回退、结果快照和失败后拒绝继续计算。

同日已通过 Windows 文件共享更新 11 的后端和 worker，保留原文件备份，并在实际双机有线 LAN
复测。两端配置保持 128K 上下文、11 的 7 个 pool worker、48 GiB 缓存和全部专家预加载。
同一脚本、相同任务顺序和专家 ID 的 708 次任务，从 5.52 s 降到 2.54 s。
优化前逐任务写控制台，优化后分段计时写文件，因此这个对照也包含日志输出方式的变化。

| 测量项 | 优化前 | 优化后 |
| --- | --- | --- |
| 3 个位置、18 个不同专家，5 种层格式的热态往返中位数 | 8.37–9.15 ms | 3.33–4.37 ms |
| 48 层、每层 18 条结果、6 个共享专家，3 次整轮耗时 | 405–449 ms | 173–185 ms |
| 48 层、每层 18 个不同专家，3 次整轮耗时 | 484–550 ms | 226–295 ms |
| 同一测试最后 29 条任务的 worker `compute_ms` 平均值 | 6.36 ms | 2.63 ms |

优化后最后 29 条任务的平均值：后端调用 2.27 ms、输出校验 0.029 ms、输入解析 0.027 ms。
真实 LAN 专家对照仍为最大绝对误差 0。
接着在 16 启动临时完整引擎，用 36-token 的代码提示词生成 256 个 token：
prefill 3912 ms，decode 27738 ms，即 9.2 tok/s；MTP 接受 173/225，GPU 专家命中率 23.2%。
这是一次短提示词生成，没有相同提示词的优化前完整引擎对照，不能据此给出整机加速倍数。
该次启动报告仅 91 MiB 显存余量，仍需在实际长对话中验证 WDDM 的内存余量和稳定性。

### 远端汇总和主机 RAM 分担（2026-10-09）

coordinator 节点新增两个可选设置，旧配置的默认值分别为 false 和 0：

```json
"network_reduce": true,
"network_local_ram_mib": 1024
```

`network_reduce` 对应引擎参数 `--network-reduce`。worker 收到每条路由的权重，按路由顺序
在 C++ 中加权汇总；每个位置返回 2560 个 FP32 数，而不是每条专家路由各返回一个向量。
主机将汇总结果合入本地 GPU/CPU 和共享专家的结果，远端项只加权一次。
这会改变浮点累加顺序，因此不承诺与逐专家返回路径逐位一致。
两个本机 CPU worker 的真实 IQ3_S 前四层、1/3 个位置对照，最大绝对差为 `2.33e-10`。
协议通过新增握手确认 worker 和 DLL 支持汇总，旧逐专家协议仍可用；不支持汇总的 worker 拒绝启动连接。

`network_local_ram_mib` 对应 `--network-local-ram-mib`，范围 0–65536 MiB。
主机按现有 profile 把热门 GPU miss 放入独立 RAM 缓存，受可用 RAM、提交空间和系统余量限制。
该选项沿用 resident budget 的 4 GiB 余量，并可用 `STRATA_RESIDENT_HEADROOM_GIB` 调整。
GPU 命中先算，已在这个 RAM 缓存的 miss 由主机 CPU 算，其余按共享配置发送到 worker；
解码分工不会为了本地 CPU 任务临时读取不在缓存的专家。
日志中的 `strata network local: entries=...` 记录累计本地 CPU 路由条目数。
在 16 上使用两个 localhost CPU worker、1 GiB 主机 RAM 缓存，完整 48 层 MTP 两次各生成 8 个 token，
主机 CPU 共处理 318 条专家路由。此检查验证路径和实际参与，没有用回环速度推断 LAN 性能。

两个设置都会改变共享配置 SHA256；两端需更新同一配置并重启。启用汇总还需更新两端二进制和 worker。
`STRATA_NETWORK_PROFILE=1` 的日志新增 `reduce_ms`，记录远端 C++ 汇总及其输出校验时间。

实际双机对照（16：i9-12900HK、RTX 4060 Ti 16 GB、32 GB RAM；11：i7-10700、64 GB RAM；
有线 LAN）：128K 最大上下文、512-token prefill、MTP `spec 4`、相同 36-token 代码提示词，
每次生成 256 个 token。为控制 GPU 缓存差异，对照固定 `--expert-cache 2400`，
原生 pack 实际装入 3107 个槽位、5.95 GiB；显存预留 1200 MiB。

| 模式 | decode tok/s | 生成期间返回字节数 | 主机 CPU 路由条目数 |
| --- | --- | --- | --- |
| 第一批优化后的逐专家返回 | 9.2 | 1.20 GB | 0 |
| 远端加权汇总，两次对照 | 14.8、15.3 | 0.15 GB | 0 |
| 汇总 + 1 GiB 主机缓存、性能核，两次调参对照 | 16.3、15.5 | 0.15 GB | 每次 4496 |

逐专家返回与汇总的第一组对照，256 个输出 token 完全相同；两个 worker 的汇总只保证前述数值误差范围。
1 GiB 本地模式两次调参的 256-token 输出也完全相同。此处没有将不同 CPU 的舍入或所有提示词视为逐位一致。
4 GiB 本地缓存的全核/性能核布局分别为 14.1/14.7 tok/s，本机最终选择 1 GiB。

最终双机配置为 `network_reduce: true`、`network_local_ram_mib: 1024`，
16 的专用 server 配置使用 `--pool-affinity p-cores`（5 个 pool worker 加主机线程）、
`STRATA_POOL_SPIN_US=50`、上述固定 GPU 缓存和显存预留。
LAN 模式默认保留提示词的文件缓存读取；显式 `STRATA_UNBUFFERED_LOAD` 设置仍生效。
最终配置两次 256-token 验证为 15.3/15.0 tok/s，每次由 16 处理 4496 条专家路由；
提示词处理约 3.5–4.0 s，整次请求约 20.6–20.8 s（不含引擎启动）。
这些是短提示词测量，仍需用实际长对话验证吞吐和内存余量。

## 本地验证记录（2026-10-08）

在本机 RTX 4060 Ti 16 GB、32 GB RAM、Windows 上，用已安装的 IQ3_S 两片 GGUF 做回环验证；
IntelLLVM 2026.1.1，CUDA 13.4，GPU 目标 sm_89。CPU-only 库另以 `STRATA_PORTABLE=ON` 和无 `/QxHost`
的通用参数构建成功；主机 CUDA 引擎和 GPU worker 库构建成功。

- 14 项 worker 协议测试通过：认证、模型/分片身份、分段 TCP 包、行顺序、旧任务编号、非法路由、
  非有限输入、超大包、半包超时和内核错误传播。
- C++ 调度端的 7 项异常测试通过：认证错误、模型错误、任务编号错误、断线、超时、NaN 结果、短结果。
- 两个 CPU worker 的真实 IQ3_S 前四层（1/3 token）与本地 CPU 专家池结果逐位一致，主机命中行未被覆盖。
- 一个 CPU worker、一个本机 CUDA worker 的相同向量检查通过；最大绝对误差 `5.28558157e-5`，
  对应参考最大绝对值 `0.0125296768`。CPU/GPU 舍入不同，这不是逐位一致。
- 完整 48 层引擎：两个 CPU worker、移除 MTP；以及 CPU + CUDA worker、MTP `--spec 4`，
  都完成两次 8-token 代码提示词生成，两个 worker 每次都执行了任务。

这是小规模功能检查。没有测量三机千兆链路，没有在 i7-10700 或 RX 7600M XT 上验证二进制，
此后已完成本分支 HIP 包的离线构建和 CPU 路径验证（见构建章节）；
仍没有完成 8K/16K、256-token 的性能和质量验收。
不据此承诺 15 TPS 或稳定的三机加速。

## 实际双机启动检查（2026-10-08）

16：RTX 4060 Ti 16 GB、i9、32 GB RAM、Windows；11：i7-10700、64 GB RAM、Windows 10。
使用有线 IPv4 地址 192.168.233.136（16）和 192.168.233.126:18500（11），
没有使用 172.16.0.x 的 Nebula 虚拟网络。11 的模型与 pack 位于本地 `D:/Strata-data`，
worker 库位于 `D:/Strata/strata_network_backend.dll`。

IQ3_S、8192 上下文、512-token prefill、MTP `--spec 4`、`--spec-min-p 0.5`，
关闭 prompt cache 和自适应交换；主机缓存自动分配 4621 个专家（8.81 GiB），
11 使用 7 个 pool worker、40960 MiB RAM 专家缓存，负责全部主机 GPU miss。
API 在 16 的 `127.0.0.1:18080` 启动，并启用认证。

首先通过真实 LAN 做前四层的 1/3-token 专家对照，最大绝对误差全部为 0。
然后 API 请求设置 `temperature=0`、`reasoning_effort=none`、`max_tokens=64`：

| 提示词 | 返回结果 | 实际输入/输出 token | 提示词处理 | 解码 | HTTP 总耗时 |
| --- | --- | --- | --- | --- | --- |
| 请直接回答：2+2等于几？ | `4` | 21 / 2 | 3412 ms | 7624 ms，约 0.3 tok/s | 11.058 s |
| Write a Python function that returns the square of a number. No explanation. | Python `square(n)`，返回 `n ** 2` | 27 / 17 | 1599 ms | 18925 ms，约 0.9 tok/s | 20.541 s |

第一请求主机缓存命中率 24.2%，向 11 提交 96 个层任务、1820 个专家条目。
两请求结束累计 432 个层任务、10079 个专家条目，证实辅助 CPU 实际参与；第二请求命中率 21.8%。
这次检查证明配置、模型身份、网络通信和完整生成路径可用，性能仍明显偏低。
远端往返耗时包含计算、文件读取、调度和通信，尚未分离这些成本；不是稳定性能验收，也未达到 15 TPS。
原始响应保存在本机 `tmp/dual-host-test/api-results.json`，引擎记录在 `logs/cluster-host16.log`。

### 用真实 agent 接入当前双机服务

在 11 保持 `D:\Strata\start-host11.bat` 运行，在 16 的仓库根目录双击或执行 `start-host16.bat`。
脚本使用上述已验证的双机配置和辅助机密钥，打印 OpenAI-compatible Base URL 和模型名。
本机 API 默认不要求 key，仅监听 `127.0.0.1`；辅助机连接仍使用共享密钥认证。
如果相同模型、上下文和认证设置的服务已经运行，会确认可访问而不再启动第二个引擎。
`start-host16.bat --check` 只检查准备状态并显示接入参数；`--check` 放在其他参数之后也会直接退出，
保留检查的退出码。`--help` 同样直接退出；双击普通启动则在服务停止后保留窗口。

真实 agent 对话应保留提示词缓存（`--prompt-cache 6`，引擎默认值）；`--prompt-cache 0`
会让后续轮次也从头计算全部输入，只适合不复用缓存的对照测量。
当前 `start-host16.bat` 使用 `--prefill auto:8192`，主机按显存选择最多 8192-token 的提示词块。
可在该脚本后加 `--prefill 2048` 等参数覆盖；`tools/cluster.py` 也接受同一参数。
此参数只覆盖主机的运行设置，不改变共享配置指纹，因此无需重新加载辅助机专家。

2026-10-09 在上述 host16/host11、IQ3_S、128K 上下文上重放 omp 的 `hi` 请求：
12 个工具定义与项目上下文共 10258 个输入 token，最多生成 128 token。
原配置为 512-token 分块、关闭缓存；修复后为自动分块（实际 8192）、6 个检查点。

| 请求 | 原配置提示词计算时间 | 修复后提示词计算时间 | 修复后复用 / 新读 token |
| --- | --- | --- | --- |
| 首轮 `hi` | 258.2 s | 74.8 s | 0 / 10258 |
| 同一会话下一句 | 288.9 s（原请求 10349 token） | 1.7 s（本次 10330 token） | 10313 / 17 |
| 新会话重放相同 `hi` | 未单独测量 | 0.44 s | 10253 / 5 |

修复后三次请求连同输出分别为 80.4、4.8、4.3 s；原始响应、用量和引擎计时在
`tmp/host16-prompt-fix-results.json`。这是单次真实 agent 重放，不代表所有长度或配置的吞吐。

同日处理 16 的主机内存压力：专用专家缓存虽然只有 1 GiB，提示词读取的 GGUF 映射页仍会填满 RAM。
当前专用 server 配置保留 `STRATA_FILE_RELEASE=1` 和 `STRATA_UNBUFFERED_LOAD=1`：前者在 GPU
上传完成后释放可回收的文件映射页，后者让专家文件读取绕过系统文件缓存。主机 RAM 缓存、提示词
检查点和 GPU 缓存继续使用；辅助机专家预加载配置没有改变。
仅开映射页释放时，同一请求组结束后 16 仍占用约 28.8 GiB，故又启用显式 unbuffered 读取。

在上述同一硬件和 10258-token 请求上，处理前空闲时总 RAM 占用 31.3/31.8 GiB；两项设置开启后，
三个请求结束时总占用分别为 9.61、9.73、9.61 GiB，可用 RAM 为 22.16、22.04、22.15 GiB。
首轮提示词计算 75.7 s（之前自动分块测得 74.8 s）；后续两轮复用 10313、10253 token，
提示词计算分别为 4.4、1.4 s，连同输出分别为 7.4、5.4 s。
这些是单次重放及请求结束后的快照，没有测量全部运行期间的内存峰值。原始记录为
`tmp/host16-before-memory-fix.json`、`tmp/host16-memory-fix-results.json`。

### 辅助 CPU 的任务批量对照（2026-10-09）

11 只参与生成阶段的专家计算，主机预填充期间没有远端专家任务；短问候的生成阶段只有几秒。
当前 CPU worker 的 `workers: 7` 是 7 个后台线程，调用线程也参加计算，并非只启动 7 个计算线程。
CPU 占用率不表示整机吞吐；单会话的每一层还需等待主机路由、GPU 结果及网络返回。

在上述同一双机、IQ3_S、128K、1 GiB 主机专家缓存、unbuffered 读取设置下，
用代码生成和二分查找解释两个提示词，每次最多输出 192 token，temperature=0，不启用思考。
每组各重复两次，比较引擎报告的生成速度，剔除预填充时间；初始 `spec 4` 的三种阈值采用相反顺序重复。
增大 MTP 窗口和降低 draft 概率阈值会改变原生量化计算的批量，输出文字可能不同。

| MTP 窗口 / draft 阈值 | 生成速度中位数 tok/s | 远端任务平均位置数 |
| --- | ---: | ---: |
| 4 / 0.5（保留） | 14.85 | 3.41 |
| 4 / 0.25 | 13.45 | 3.89 |
| 4 / 0 | 12.90 | 3.94 |
| 6 / 0.5 | 13.30 | 4.76 |
| 6 / 0.75 | 14.20 | 3.47 |

共 20 次测量请求，另有 3 次预热。较大任务未提高整体吞吐，因此恢复 `spec 4`、`spec-min-p 0.5`。
这组实验没有测量 11 的实时 CPU 百分比或比较不同 worker 线程数；不能据此断言 7 是所有负载的最优值。
原始用量、生成计时及远端任务计数在 `tmp/host11-batch-tuning.json`、`tmp/host11-spec6-tuning.json`、
`tmp/host11-spec6-high-confidence.json`。主机预填充和辅助机线程数需分别测量。

当前 agent 参数：Base URL `http://127.0.0.1:8080/v1`，模型 `qwen3.8-flash-next-iq3_s-network`，
API key 可填客户端要求的占位值。客户端请求超时先设为 300 秒或更长，输入和输出合计不超过 131072 token（128K）。
这是后续修改的启动设置；上面的双机测量记录仍对应当时的 8192 上下文和 18080 端口。
上下文属于共享配置内容，切换后 11、16 都需重启以读取一致的配置。
当前双机配置给 11 的专家 RAM 缓存上限为 49152 MiB（48 GiB）。
现已启用 `preload_experts: true`：`start-host11.bat` 启动时先将它负责的全部专家读入私有 RAM 缓存，
每层打印进度；完成后才显示 `worker ready` 并监听端口。双机方案下是全部 48 层、24576 个专家。
这套 IQ3_S 表记录的完整专家权重为 50292326400 字节（约 46.84 GiB），48 GiB 缓存可容纳其全部专家，
首次文件读取集中在启动预加载阶段，之后专家任务直接使用缓存；并不是只让 Windows 文件缓存预读。
系统页缓存、Python 和线程缓冲区还会占内存；64 GB RAM
不应全部分给私有专家缓存。该预算提高尚未进行冷/热缓存性能对照，上面的测量仍对应 40 GiB 上限。

加载前会确认所有分配专家都能同时放入 `ram_cache_mib`，并检查当前可用物理 RAM 和 Windows 可用提交空间，
为系统留至少 8 GiB 余量。预算不足、内存不足或读取失败则启动报错，释放部分缓存，不开始接收请求；
不会逐个加载又逐个淘汰而声称已经全部预加载。3/4 机配置可按需给 worker 设置该布尔选项，
只加载它在共享配置中负责的范围；旧版命令行对应 `--preload-experts`，配置模式只接受共享 JSON 中的设置。
Python 缓存采用普通进程内存，没有锁定物理页；系统内存压力仍可能导致换页。
修改预加载设置也会改变配置指纹，两台机器都要重启。
接口提供 `/v1/chat/completions`，也保留现有 `/v1/responses` 和 `/v1/models` 等 API。
此入口监听 localhost，供 16 上的 agent 使用；原有单机 `run-iq3_s.bat` 仍使用自己的配置。

统一配置追加验证（同一本机回环环境）：16 项配置/归属/握手检查通过；真实 IQ3_S 以 1、2、3、4 个
CPU worker 按显式范围运行，全部通过 C++ 专家结果对照；两个 worker 直接从同一 JSON 启动并通过对照。
11 机的 CPU 后端另以 IntelLLVM 2026.1.1、`/O3 /DNDEBUG /fp:precise`、无 `/QxHost`、
`STRATA_PORTABLE=ON` 重编译并重复通过上述配置与真实专家对照；未在 i7-10700 本机运行。
主机通过 `tools/cluster.py` 读取同一 JSON 启动 API，完成 8 个输出 token 的真实生成。
显式范围的 CPU + CUDA worker 也完成两次 8-token MTP 生成。仍不代表真实 LAN 的性能或质量验收。

重复完整启动验证可运行（会临时启动 localhost worker 和 API，使用很小的缓存预算）：

```powershell
python tools/test_cluster_config.py --build build-network-cpu --pack D:/Strata-data/packs/iq3_s --native D:/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf --engine-config strata-iq3_s.json
```

### 原生文本层（迁移第 3 步）

显式开启 `STRATA_BUILD_NATIVE_CLUSTER=ON` 后可构建 `strata-text-check`，它读取 pack 的
`tokenizer/` 目录和模型的 Jinja 模板，提供原生分词、消息归一化、模板渲染、思考/工具调用解析、
增量 UTF-8、停止字符串和 JSON Schema 输出验证。它是仅在 `STRATA_BUILD_TESTS=ON` 时构建的 JSON-lines 诊断程序，第 4 步的
`strata-host serve` 复用该文本层；`strata-host cluster` 启动自身的 `serve` 子命令提供主机 API。

文本层复用 llama.cpp 的 Unicode/Jinja 实现，并使用 jsoncons v1.7.0（Boost Software License 1.0）。
CMake 下载固定版本并校验 SHA256；离线构建可传 `-DSTRATA_JSONCONS_DIR=/path/to/jsoncons-1.7.0`。
构建目录的 `native-text-licenses/` 保存依赖许可证，打包时需带上。没有 Python 运行依赖。
SYCL 工程可单独开启 `STRATA_BUILD_NATIVE_TEXT=ON` 构建相同 host 文本目标；这不增加 SYCL 专家网络后端。

Windows CPU 对照的重复命令（测试驱动需要 Python、regex、jinja2；完整 schema 对照还需要 jsonschema）：

```powershell
python tools/test_native_text.py --exe build-network/strata-text-check.exe --tokenizer D:/Projects/Strata-data/packs/iq3_s/tokenizer --report tmp/native-text-parity.json
```

2026-10-09 本机 CPU 对照 15 组通过，无跳过，包含 727 次完整 token ID 对照、10 个聊天 golden case、
37 个工具调用 specimens 和 41 项现有 parser 回归。Unicode 路径、普通文字中的特殊 token、
非法/跨 token UTF-8、跨 chunk 停止字符串和结构化输出错误均有对照；System32-only PATH 下原生
程序完成分词和模板任务。CPU、CUDA sm_89、HIP gfx1102/gfx1103 配置和 SYCL host 文本目标构建通过。
本轮没有运行 GPU 推理、验证 Linux 或 Swift 专属模板，也没有测量吞吐、延迟或切换部署服务。
