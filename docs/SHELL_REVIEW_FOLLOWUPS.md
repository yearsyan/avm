# MacMu Shell Review Follow-ups

2026-07-08 对 `shell/` 的 code review 遗留项。当次 review 发现的四个正确性问题
(seqlock 非原子读、fd 关闭竞态、退出期 use-after-free、display id 槽位泄漏)
已经修复,不在本清单内。

## 已处理

### 重复代码收敛

- `shell/core/unix_listener.h`:抽出 Unix socket listener 创建、唤醒和
  guest pipe 握手逻辑,`GuestControlClient` 与 `GuestInputSender` 只保留各自
  的连接生命周期和日志策略。
- `shell/core/pending_request_table.h`:抽出 pending request 表、timeout sweep
  和 fail-all 逻辑,`ControlChannel` 与 `GuestControlClient` 只保留响应构造差异。
- `shell/core/posix_util.h`:收敛 `path_join` 和 `set_close_on_exec` 小工具。

### qemu fd 继承

已验证本机上 `posix_spawn_file_actions_adddup2(fd, fd)` 不会让带
`FD_CLOEXEC` 的同号 fd 传入子进程,因此没有采用直接无条件 `adddup2(fd, fd)`
方案。

当前实现改为:当 parent fd 与 child fd 相等时,先 `F_DUPFD_CLOEXEC` 复制到一个
临时 fd,再通过 `posix_spawn_file_actions_adddup2(temp, childFd)` 传给 qemu。
这样不需要在父进程短暂清除 `FD_CLOEXEC`,也避免了并发 spawn 泄漏窗口。

### 性能和小项

- `macmu_replace_system_image_from_directory`:优先 `rename` 解压出的系统镜像目录,
  跨卷 `EXDEV` 时再回退递归 copy,避免同卷 APFS 上多拷贝数 GB 数据。
- `macmu_surface_renderer.mm`:按 Metal device + pixel format 缓存 render pipeline,
  避免每个显示窗口重复编译同一段 shader。
- `frame_consumer.cpp`:slot 扫描改为 round-robin 起点,避免高帧率低号 display
  长期压住高号 display。
- `macmu_shell.mm` `importSystemImageArchive:`:将废弃的 `NSTask.launchPath`
  替换为 `executableURL`。

## 仍待处理

### DISPLAY_ADD 请求流程复制粘贴

`macmu_shell.mm` 的 `newDisplay:` 与
`launchSelectedAppInNewDisplayWithProfile:` 的"分配 id -> 组包 -> 请求 -> 失败释放
-> 开窗 -> 校验窗口"流程仍基本相同。

**建议**:提取 `addDisplayWithProfile:completion:`。

### 拆分 macmu_shell.mm(约 1800 行的上帝类)

`MacMuAppDelegate` 目前身兼:状态窗口 UI 构建、apps 表格 datasource/delegate、
显示窗口管理、镜像导入、qemu supervisor 线程、doorbell 线程。收益最大的两块:

- **`QemuSupervisor`(纯 C++)**:把 `qemuMonitorLoop` 拿出来,通过
  status/lifecycle 回调与 UI 解耦。现在 monitor 线程直接回调 ObjC 方法,
  `_qemuPid` / `_qemuGeneration` / `_controlChannel` 三把锁散落在 delegate 里,
  拆出后这些状态有天然 owner。
- **`DisplayWindowManager`**:`_displayWindows` / `_suppressRemoveOnClose` /
  `_displayAppBindings` / `_activeUserDisplayIds` 四个容器的一致性规则目前靠
  散落在多个方法里的注释维持,集中管理后 id 泄漏类 bug 不易再发生。

其余可拆:`MacMuStatusWindowController`、`MacMuAppsListController`。

### ivar 裸指针改 unique_ptr

`_frameConsumer` / `_inputSender` / `_guestInputSender` / `_guestControlClient`
目前是裸 `new`/`delete`。改 `std::unique_ptr` 后 `performRuntimeShutdown` 的
手动 delete 链可以消掉。

### 双输入通道的选路逻辑收敛

host->guest 输入有两条平行路径(fd 继承的 binary `InputSender` + guest agent
文本协议的 `GuestInputSender`),`macmu_input_view.mm` 里每个事件都重复一次
`ready()` 三元分发(共 6 处)。

**建议**:短期抽一个 `InputRouter` 把选路收到一处;长期两条协议可统一 framing。

### 状态窗口布局

状态窗口 UI 用硬编码 frame 坐标 + autoresizing mask 组合,resize 行为脆弱;
若窗口布局继续演进可考虑 Auto Layout。

### shell_options.cpp argv 解析

`*_overridden` bool 群 + 手写 argv 解析仍偏冗长,可改表驱动 option 结构。
优先级低。

### magic fd 文档同步

`kChildFrameDoorbellFd=198`、MMCP fd 197 等 magic fd 号可行但脆弱,已有注释说明,
保持文档同步即可。
