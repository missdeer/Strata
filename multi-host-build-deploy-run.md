# Windows 多机编译、部署和运行

本文按本分支在 `D:\Projects\Strata` 的现有目录说明操作。命令使用
PowerShell；批处理文件通过 `cmd /c` 或直接调用运行。

当前运行配置为 **16 机（本机 CUDA coordinator）＋11 机（CPU worker）**。
17 机是 HIP 构建目标，尚未加入当前双机运行计划。

## 1. 目录和前提

| 用途 | 目录或文件 |
| --- | --- |
| 本机 CUDA 编译输出 | `build-intel/` |
| 17 机 HIP 编译输出 | `build-cluster/host17-core/` |
| 原生部署包构建器 | `build-cluster/builder/strata-cluster-build.exe` |
| 原生部署包输出 | `dist/cluster-native/host16/`、`host11/`、`host17/` |
| 当前共享双机计划 | `tmp/dual-host-native/cluster.json` |
| 本机 API 配置 | `tmp/dual-host-native/server.json` |
| 本机 worker 共享密钥 | `tmp/dual-host-test/network.key` |
| 日常本机启动入口 | `start-host16-native.bat` |

本机已有 Visual Studio 2022 Community、Intel oneAPI 2026.1、CUDA、
CMake 和 Ninja。HIP 构建使用 `.rocm-win/Lib/site-packages` 下已解包的
`_rocm_sdk_devel` 和 `_rocm_sdk_core`。原生部署包构建器不会安装 SDK。

保留以下离线依赖源码，它们也是现有 CMake 缓存的输入：

- `third_party/llama.cpp`
- `tmp/jsoncons-source/jsoncons-1.7.0`
- `tmp/native-image-deps/libwebp-1.6.0`
- `tmp/native-image-deps/libavif-1.4.2`
- `tmp/native-image-deps/aom-3.14.1`

`tmp/native-text-test-deps` 是 Python 测试依赖；`tmp/stage6-known-hosts`
供现有远程构建驱动验证 SSH 主机。整个 `tmp/` 被 Git 忽略，但不能因此
清空其中仍被构建、启动或测试引用的文件。

## 2. 日常增量编译

先在本机服务窗口按 Ctrl+C，等待本机宿主和引擎退出，再编译同一目录。
已有程序运行时，Windows 可能阻止替换 exe 或 DLL。

```powershell
cd D:\Projects\Strata
cmd /c build\run-consolidated-cuda-incremental.cmd
```

此命令使用现有 CMake 缓存，编译三个目标：`strata`、`strata-host`、
`strata_network_backend`。输出仍在 `build-intel/`。

改了 CMake 配置、需要重新配置现有 CUDA 构建树时，执行：

```powershell
cmd /c build\run-consolidated-cuda.cmd
```

这个脚本重新配置现有构建树，再编译上述目标；它依赖现有缓存中的
编译器和后端设置，不是从空目录开始的安装脚本。

17 机 HIP 的现有构建树可在本机增量编译：

```powershell
cmd /c build\run-consolidated-hip-incremental.cmd
```

其输出在 `build-cluster/host17-core/`。这里的编译不代表已经完成
17 机上的真实 GPU 推理或多机运行验证。

## 3. 编译并生成部署包

使用 `tools/build_cluster_native.bat`。它会按节点编译，并收集运行所需的
DLL、Web、模板、许可证和 HIP 资源。首次运行且构建器不存在时，
该入口会先编译构建器。

```powershell
cd D:\Projects\Strata
.\tools\build_cluster_native.bat `
  --hosts host16 host11 host17 `
  --build-dir host16=build-intel `
  --build-dir host17=build-cluster/host17-core `
  --ggml-dir third_party/llama.cpp `
  --jsoncons-dir tmp/jsoncons-source/jsoncons-1.7.0 `
  --webp-dir tmp/native-image-deps/libwebp-1.6.0 `
  --avif-dir tmp/native-image-deps/libavif-1.4.2 `
  --aom-dir tmp/native-image-deps/aom-3.14.1 `
  --out-root dist/cluster-native `
  --jobs 6
```

只构建当前 16＋11 双机时，将 `--hosts` 改为 `host16 host11`，并删除
`--build-dir host17=build-cluster/host17-core` 这一行。

| 目标 | 编译目录 | 部署包 | 构建参数 |
| --- | --- | --- | --- |
| host16 | `build-intel/` | `dist/cluster-native/host16/` | oneAPI `/O3 /QxHost`，CUDA sm_89 |
| host11 | `build-cluster/host11/` | `dist/cluster-native/host11/` | oneAPI `/O3`，便携 AVX2 |
| host17 | `build-cluster/host17-core/` | `dist/cluster-native/host17/` | ROCm Clang，便携 AVX2，gfx1102/gfx1103 |

host11 的便携包可在本机编译后复制到 11 机。它与 11 机现有的现场
`/QxHost` 专用构建是两个独立版本。本机 host16 包采用本机 CPU 参数，
不能当作其他 CPU 机器的通用包。

在上述命令中添加 `--dry-run` 可检查构建和输出目录；构建器已经存在时，
此选项不编译、不打包、不下载。添加 `--package-only` 则打包已有的
合格编译输出，不重新编译。

检查退出码和 `dist/cluster-native/build-summary.json`，确认需要的节点
均成功。节点包的 `CLUSTER_BUILD.json` 记录编译参数与文件 SHA256。
打包器不会停止服务，并会保留被替换包的 `host*.previous-*` 目录。

## 4. 部署到 11 机或 17 机

1. 停止目标机 worker，保留旧运行目录以便回退。
2. 将对应的整个节点包复制到目标机，例如 `D:\Strata\runtime`。
   保留 DLL、资源和 HIP 包的 `rocm` 子目录结构。
3. 修改共享 `cluster.json` 中该节点的 `library`，例如
   `D:/Strata/runtime/strata_network_backend.dll`。核对该机的模型和 pack 路径。
4. 将修改后的同一份 `cluster.json` 同步到所有参与运行的节点。
5. 在目标机使用新目录下的 `strata-host.exe` 启动 worker。

模型、pack、MTP、图片模型、驱动和共享密钥不随节点包自动部署。
运行机器不需要 Python；需要兼容的 GPU 驱动以及共享计划指定的数据。

本机仍从 `build-intel/` 运行时，无需额外复制 host16 包。若改为从部署包
运行，需要同时调整本机启动入口、共享计划的 `engine`、API 配置的
`lib_dirs`、`web_root` 和模板等实际使用的资源路径。

当前本机和 11 机的共享计划均已将 host16 `engine` 设为
`D:/Projects/Strata/build-intel/strata.exe`。本机 `tmp/stage6` 已删除。
11 机现有的 `D:/Strata/stage6-native-build-20261011/build-native` 是该机
独立的运行目录，与被删除的本机目录不同，仍是旧 worker 启动脚本的目标。

加入 17 机时，可参考 `configs/cluster-3hosts.json`，但必须填写实际 IP、
后端、模型及 DLL 路径，并重新分配专家范围。模板中的地址不是当前配置。
例如，11 机负责 0–255、17 机负责 256–511；所有在线 worker 合计应覆盖
0–511，不能重叠或遗漏。修改后的计划仍需完整同步到所有节点。

计划内容哈希参与 worker 握手。修改任何计划字段后，先停止 coordinator，
再更新并重启各 worker，最后重新启动 coordinator，使所有进程加载同一计划。

## 5. 运行当前 16＋11 双机

先在 11 机启动 worker，等日志显示预加载完成、worker ready，再启动本机。

沿用旧现场构建时，使用 `tmp/dual-host-native/start-host11-native.bat` 在
11 机上的副本。脚本旁的 `cluster.json` 必须是当前共享计划。

若已将新包部署到 `D:\Strata\runtime`，可在 11 机 PowerShell 中运行：

```powershell
$env:STRATA_NETWORK_KEY = (Get-Content D:\Strata\network.key -Raw).Trim()
$env:PATH = "D:\Strata\runtime;$env:PATH"
D:\Strata\runtime\strata-host.exe cluster --config D:\Strata\cluster.json --validate
D:\Strata\runtime\strata-host.exe cluster --config D:\Strata\cluster.json --node host11
```

17 机部署后使用同样步骤，将节点名改为 `host17`，使用该机保存的同一
共享密钥，并将 `D:\Strata\runtime\rocm\bin` 加入 PATH。

本机先验证配置，再启动：

```powershell
cd D:\Projects\Strata
.\start-host16-native.bat --check
.\start-host16-native.bat
```

`--check` 验证共享计划和本机 API 配置，不启动模型或服务。
默认预填充设置为 `auto:8192`，也可用 `--prefill 2048` 等参数覆盖。

| 接口 | 地址 |
| --- | --- |
| Web | `http://127.0.0.1:8080` |
| OpenAI API Base URL | `http://127.0.0.1:8080/v1` |
| 健康检查 | `http://127.0.0.1:8080/health` |

```powershell
Invoke-RestMethod http://127.0.0.1:8080/health
Invoke-RestMethod http://127.0.0.1:8080/v1/models
```

`/health` 的 `loaded` 为 `true` 表示模型已加载。模型名以 `/v1/models`
返回值为准。当前本机入口使用 loopback API，关闭 API key；若将 API
监听地址改为非 `127.0.0.1`，必须启用 API key。worker 的
`STRATA_NETWORK_KEY` 与 API key 是两项独立设置。

停止时先在本机 coordinator 窗口按 Ctrl+C，再停止各 worker。

## 6. 验证范围和历史脚本

2026-10-13，上述三节点构建参数已通过本机原生构建器的 `--dry-run`
检查；该检查不证明编译、部署或三机运行成功。本机启动入口的
`--check` 通过，11 机共享计划与本机计划哈希一致，已有本机服务的
健康检查返回 HTTP 200、`loaded: true`。整理这些步骤时未重新编译、
部署或重启服务。

`build/rebase-*`、`build/stage6-*` 和部分 `build-native-*`、
`build-launcher-*` 保留历史实验过程，可能依赖指定提交、日期、旧目标或
旧报告。日常操作使用本文列出的入口。`tmp/rebase-*` 是历史验收证据，
正常编译和服务运行不需要它们。

更多构建器、协议、配置和验收说明见 `docs/MULTI_HOST.md`；迁移脚本
列表见 `build/README.md`。
