# Novix 设计文档

Novix 的灵感来自 **Linux 的 PCB（进程控制块）进程管理模型**，
并在此基础之上开发而成。项目起点是 `src/`（Linux 内核模块）：
它以 PCB 为核心模拟进程管理——伪造的 PCB、伪造的上下文切换、
伪造的内存记账。Novix 由该模块演进而来，成为**真正的操作系统内核**：
不再模拟进程管理，而是真正引导、真正切换 CPU 上下文、真正管理页表、
真正从 ring0 进入 ring3 运行用户程序。

## 1. 目标与非目标

**目标**
- i386 保护模式，Multiboot 引导，QEMU 中运行
- 真实的抢占式多任务：PIT 100Hz 时钟中断驱动的完整寄存器上下文切换
- 5 种可切换调度策略（PRIORITY / RR / FCFS / EDF / RMS），带老化机制
- 真实内存管理：物理页位图分配器 + 内核堆 + 4MB 大页内核映射 + 每进程 4KB 页目录
- ring3 用户态：int 0x80 系统调用、ELF32 加载器、每进程独立地址空间
- 文件系统：VFS 抽象 + ramfs 根文件系统 + devfs 设备文件 + 每进程 fd 表
- 双路控制台输入（PS/2 键盘 IRQ1 + COM1 串口 IRQ4，轮询兜底）驱动的交互 shell
- 内建自测试套件（无断言不发布），串口日志可用于 CI

**非目标（v0.2）**
- 多核 SMP、fork/exec、持久化磁盘文件系统（FAT32）、网络、COW
  （以上均在 ROADMAP 中）

## 2. 从模块到 OS 的映射

| 模块（src/） | Novix（os/kernel/） | 变化 |
|---|---|---|
| pcb.c 模拟 PCB | task.c | 真实内核栈 + 真实寄存器帧 + fd 表 |
| scheduler*.c 模拟调度 | sched.c + switch asm | PIT 抢占，真实栈切换 |
| scheduler_context.c 伪造寄存器 | pt_regs + iret | 真实保存/恢复 EFLAGS..EAX |
| memory.c 虚拟记账 | pmm.c + heap.c + paging.c | 真实物理页 + 页表 |
| file_system.c 内存链表 mock | fs/vfs.c + fs/ramfs.c + fs/namei.c + fs/devfs.c | v0.2.0 落地：inode/f_op/i_op 分层的真 VFS |
| signal.c 打印日志 | （ROADMAP） | |
| security.c 明文口令 | （ROADMAP） | |
| smp.c 空壳 | （ROADMAP） | |
| procman ioctl | syscall.c int 0x80 | 真正跨越特权级 |
| comprehensive_test 无断言 | ktest.c | 全部带断言 + CI 退出码 |

## 3. 架构

```
QEMU (i386)
 └─ Multiboot → boot.S → kmain
      ├─ console (VGA 0xB8000 + COM1 串口)
      ├─ GDT/TSS, IDT, PIC 重映射, PIT 100Hz, PS/2 键盘 + COM1 串口输入
      ├─ PMM 位图 [16MB, RAM_TOP)
      ├─ 内核堆 [4MB, 16MB) 空闲链表分配器（v0.2 扩容，容纳 ramfs 数据）
      ├─ 分页: 内核 4MB 大页恒等映射; 用户进程独立页目录(4KB页)
      ├─ fs/namei.c: 路径解析（namei/namei_parent, "."/"..", 根不出界）
      ├─ fs/vfs.c: VFS 核心（open/read/write/lseek/stat/mkdir/unlink/
      │            readdir + 每任务 fd 表[16], fd 0/1/2=/dev/console）
      ├─ fs/ramfs.c: 堆-backed 文件系统（节点树+数据缓冲），initrd 解包
      ├─ fs/devfs.c: /dev/console /dev/null（S_IFCHR + 自定义 f_op）
      ├─ task.c: PCB/内核栈/pt_regs 初始帧/僵尸回收(init=1)/fd 表
      ├─ sched.c: 就绪集扫描, PRIORITY/RR/FCFS/EDF/RMS, aging, sleep 队列
      ├─ syscall.c: int 0x80 → EXIT/WRITE/GETPID/YIELD/SLEEP/SPAWN/
      │             WAITPID/GETCHAR/SET_POLICY/UPTIME
      │             + OPEN/READ/CLOSE/LSEEK/STAT（v0.2）
      │             + 异常分流: ring3 异常杀进程(128+signo), ring0 才 panic
      ├─ elf.c: ELF32 加载器（镜像来自 ramfs: task_spawn_path("/bin/...")）
      ├─ klog.c: 带时间戳的分级日志(ERR/WRN/INF)
      ├─ ktest.c: 18 项自测（3 early + 10 mt + 5 fs）
      └─ shell.c: 串口/键盘驱动的交互 shell（help/ps/spawn/kill/ls/cat/
                  mkdir/touch/rm/echo >/>>/...）
用户程序 os/user/: crt0 + ulib, hello, spawner, fault_div, fault_segv,
                   filetest（用户态文件 syscall 端到端断言）
构建期: tools/mkramfs.py 将用户 ELF 打包为 NRFS initrd 镜像嵌入内核，
        启动时 ramfs_mount_initrd() 解包到 /bin/
```

## 4. 关键机制

### 4.1 统一中断帧 pt_regs（19 dwords）
所有中断/异常/系统调用共用一种栈帧布局（见 `pt_regs` in task.h），
`isr_common` 汇编保存全部上下文后调用 C `isr_handler`，
其返回值作为新栈指针装回 ESP——一次 `iret` 同时完成"恢复"与"切换"。

### 4.2 上下文切换
`schedule()` 在时钟中断或阻塞点被调用：
1. `current->kesp = 当前帧地址`（保存 A）
2. 从就绪集按策略选出 B；`tss.esp0 = B->kstack_top`
3. `B->page_dir != A->page_dir` 则切换 CR3
4. 返回 `B->kesp`（B 的帧可能是被中断的旧帧，也可能是新任务的首帧）

新任务首帧在 C 中伪造：eip=入口、eflags=0x202(IF)、
内核线程 cs=0x08/ss=0x10、用户进程 cs=0x1B/ss=0x23+用户栈。
iret 权限级变化自动恢复 ring3 的 esp/ss——教科书式 `iret` 双用途。

### 4.3 同步模型
单核 + 仅中断边界抢占：临界区用 `irq_save/irq_restore`（cli/sti）保护，
无需自旋锁（ROADMAP: SMP 时引入）。

### 4.4 地址空间
- 物理恒等：内核 [0, RAM_TOP) 以 4MB 大页映射，所有进程共享低 32 项页目录项
- 用户代码 0x08048000 起，用户栈顶 0xC0000000，均 4KB 页 + USER 位
- 内核堆 [4MB,16MB)（v0.2 扩容 8→16MB，ramfs 数据全部住堆），PMM 管理其余页
- 切 CR3 后内核仍可恒等访问一切（低 32 项在用户目录中共享）→ copy to user 即直写

### 4.5 控制台输入（双路 + 兜底）
- PS/2 键盘（IRQ1）：scancode set 1 → ASCII
- COM1 串口（IRQ4）：QEMU `-serial stdio` 的终端输入走这条路，CR→LF 归一化
- 两路汇入同一个 64 字节环形队列；`keyboard_getchar()` 每 10ms 轮询时
  顺带排水串口 RX（`irq_save` 保护，避免与 IRQ 处理程序竞态），即使
  中断丢失也能收到字符——串口日志/CI 场景的关键可靠性设计

### 4.6 文件系统（v0.2.0）
Linux VFS 的教学级缩影，核心是两类操作表的分离：

- **`inode_operations`（目录树形）**：`lookup/create/mkdir/unlink/truncate/
  readdir`——路径解析（namei.c）只通过它逐级下钻
- **`file_operations`（打开后行为）**：`read/write/close`——设备与普通
  文件在此统一（"/dev/console 是一个文件"的最小实现）
- **ramfs**：节点 = `{name[32], mode, data, size, cap, parent/children/
  sibling, 内嵌 inode}`，数据缓冲住内核堆，扩容走"分配-复制-释放"
  （堆没有 realloc）；树变更全部在 `irq_save` 临界区内，单核下查找
  无锁安全（查找永不阻塞，不会与变更交错）
- **fd 表**：PCB 内 `struct file *fd[16]`；创建任务时 0/1/2 统一
  `open("/dev/console")`——从此 `write(1,...)` 与 `write(fd,...)` 同一条
  代码路径；`task_exit()` 释放全部 fd（kill 路径由 waitpid 兜底，幂等）
- **initrd**：`tools/mkramfs.py` 打包 NRFS 镜像（`{magic,node_count,
  data_size}` + 节点表 `{path[64],offset,size}` + blob）嵌入内核，
  `ramfs_mount_initrd()` 启动期解包建树——用户程序不再焊死在内核里
- **已知限制**（0.3.0 处理）：inode 无引用计数（fd 与 unlink 的竞态）、
  无 rmdir、无权限位检查、无 cwd（路径必须绝对）

## 5. 构建与运行
- 编译器: zig cc (clang+lld, -target x86-freestanding) 或任意 i686-elf-gcc
- `python build.py` 全量构建; `python build.py run` QEMU 交互;
  `python build.py test` 无人值守自测（串口日志 + isa-debug-exit 退出码）
- `python tools/test_shell.py` 交互式 shell 自动化测试（21 项检查）
- 用户程序构建为 ELF 后由 `tools/mkramfs.py` 打包为 NRFS initrd 镜像
  嵌入内核，启动时解包挂到 `/bin/`
- zig 全局缓存固定在 `build/zig-cache`（build.py 内 `ZIG_GLOBAL_CACHE_DIR`），
  构建自包含、可复现

## 6. 与真实 OS 的差距（v0.2）

Novix 在"机制"意义上是真实 OS：真引导、真抢占、真跨特权级。
但与 Linux 等生产级 OS 相比仍有实质差距，如实列出如下
（与当初"诚实移除假子系统"同一精神）：

| 维度 | 真实 OS（以 Linux 为参照） | Novix 0.2 现状 |
|---|---|---|
| 进程创建 | fork()/exec() 从磁盘加载、COW 复制地址空间、线程 | spawn 从 ramfs `/bin` 加载 ELF；无 fork/exec/线程 |
| 文件系统 | VFS + 持久化文件系统（ext4、FAT…） | VFS/ramfs/devfs 已落地；无持久化、无权限检查、无 rmdir、无 cwd |
| 虚拟内存 | 按需分页、COW、交换、mmap、brk | 页错误杀死进程而非按需调页（无 COW/交换）；用户程序纯静态；无 sbrk/mmap |
| IPC 与信号 | 信号、管道、共享内存、socket | 无 |
| 多核与同步 | SMP、自旋锁/互斥锁/RCU、可抢占内核 | 单核；仅 cli/sti；内核不可抢占 |
| 设备驱动 | 统一驱动模型，磁盘/网络/USB/GPU | 仅 VGA 文本、COM1 串口、PS/2 键盘、PIT |
| 系统调用 | 300+（open/read/mmap/brk/kill…） | 15 个 |
| 安全 | 用户/权限/能力/LSM、ASLR | 无用户概念、无权限模型 |
| 平台 | 64 位、UEFI/ACPI、真实硬件 | 32 位 i386、Multiboot；poweroff 依赖 QEMU isa-debug-exit 设备 |
| 网络 | 完整 TCP/IP 协议栈 | 无 |
| 时钟 | HPET/RTC 墙钟时间、时区 | 仅 PIT 100Hz 单调 tick |

上述差距即 ROADMAP 的主要来源：fork/exec、信号与用户堆、
COW/按需分页/交换、SMP、磁盘/FAT32 等，均在后续版本中逐一补齐。
