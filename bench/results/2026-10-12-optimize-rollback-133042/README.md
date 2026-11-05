# 优化失败：终止与回滚

2026-10-12，用户要求终止本次优化尝试并回滚相关代码。第一、第二批未取得可重复、可采用的
提速方案；第三、四批取消，不再继续未测项。测量样本和失败记录保留。

回滚范围：恢复 11 个已跟踪的源码、头文件、测试和文档文件至 HEAD；移除本次新增的 fast I/O、
IOCP、调用线程绑核、任务粒度、并行量化及诊断候选代码，移除实验工具和启动脚本。
host16 独立 optimize/profile 测试包已删除；普通部署包、配置和其他工作保留。
host11 测试时替换的 host、backend DLL、checker 三个文件恢复优化前备份，逐个核对 SHA256；
同步恢复本轮发送的相关源码及 checker，使远端源文件与回滚后的头文件一致。

`rollback.json` 保存明确的文件清单、远端恢复前后哈希、本地重建程序哈希和验证结果。
`reverted.patch` 和 `retired-experiment-code.zip` 仅用于历史留档，已从活动源码/工具路径移除。
原始 benchmark 报告描述的是当时的候选，不代表当前代码；其中命令已退役。

回滚后构建通过：CPU host/backend/checker、CUDA 和 HIP engine/host/backend/checker，
SYCL 共享 CPU 池及 host。完整 SYCL 引擎仍未复测，之前的既有错误记录保留。
当前构建中 worker 合入 host，使用 `strata-host` 目标；初次沿用旧 standalone 目标名被拒绝，
随后纠正。编译器在沙箱内不能生成临时文件，沙箱外重建通过；构建警告见原始日志。

17 项 cluster-config、18 项 Python 协议/失败路径、16 项 native worker 配置/协议测试通过。
已核验活动源码无候选开关/ABI，已跟踪文件 diff 清空；本地与 host11 没有 Strata 测试进程运行。
没有修改 Windows 页文件、内存保护余量、正常启动参数，没有启动新性能实验，没有提交或推送。
