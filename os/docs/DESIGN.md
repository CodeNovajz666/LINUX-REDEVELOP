# Novix 设计文档

Novix 的灵感来自 **Linux 的 PCB（进程控制块）进程管理模型**，
并在此基础之上开发而成。项目起点是 `src/`（Linux 内核模块）：
它以 PCB 为核心模拟进程管理——伪造的 PCB、伪造的上下文切换、
伪造的内存记账。Novix 由该模块演进而来，成为**真正的操作系统内核**：
不再模拟进程管理，而是真正引导、真正切换 CPU 上下文、真正管理页表、
真正从 ring0 进入 ring3 运行用户程序、真正在多个核心上并行调度。

本文描述 v0.8.0 的架构现状。跨版本"定死不动"的契约
（地址空间分区、信号编号、syscall ABI、SMP 不变量）见仓库根目录
[DESIGN.md](../../DESIGN.md)，版本计划见 [ROADMAP.md](../../ROADMAP.md)。

## 1. 目标与非目标

**目标（已达成）**
- x86_64 长模式，Multiboot 1 + a.out-kludge 引导，QEMU `-kernel`
  直启（无需 GRUB/ISO）
- 真实的抢占式多任务：每核 LAPIC timer 100Hz 驱动的完整寄存器
  上下文切换，5 种可切换调度策略（PRIORITY / RR / FCFS / EDF /
  RMS）带老化机制
- **SMP 多核（v0.7.0）**：MP 表拓扑发现、AP 启动、自旋锁 + 锁次序、
  每 CPU 数据（%gs）、TLB shootdown IPI
- 真实内存管理：物理页位图 + 引用计数、内核堆、四级页表
  （**Rust 模块** paging.rs）、按需分页、COW fork、交换
  （kswapd 时钟算法）、匿名 mmap
- ring3 用户态：int 0x80 系统调用（37 个）、ELF64 加载器、
  每进程独立地址空间、sbrk 堆 + 用户态 malloc/free、信号
  （捕获/屏蔽/sigreturn 蹦床）、管道、argv 传递（main(argc,argv)）
- 文件系统：VFS 抽象 + 挂载表（遮蔽/合并/卸载）+ ramfs +
  devfs 设备文件 + **FAT32 持久化根文件系统（v0.8.0，LFN 读写）**
  + 每进程 fd 表 + initrd 打包 /bin + **Unix 权限位与 per-task
  cwd（v0.9.0）**
- **磁盘与驱动（v0.8.0）**：ATA PIO 主盘（LBA28 + MBR 分区）、
  统一驱动模型（probe→init→/dev 发布）、CMOS RTC 墙钟（`date`）
- **用户与登录（v0.9.0）**：uid/gid/ruid 身份贯穿 VFS 权限检查、
  getty→login→sh 登录链（/etc/inittab respawn）、salted SHA-256
  密码（/etc/passwd + /etc/shadow）、SUID exec（/bin/passwd）
- 双路控制台输入（PS/2 键盘 + COM1 串口，轮询兜底）驱动的
  交互 shell（Tab 补全 / 命令历史 / Ctrl+C）
- C + Rust 双语言内核；内建自测试套件（无断言不发布，41 项），
  串口日志可用于 CI

**非目标（v0.9.x）**
- 网络、线程、文件回写 mmap、fsck/日志、ATA 中断模式、慢因子
  密码哈希（以上均在 ROADMAP）

## 2. 从模块到 OS 的映射

| 模块（src/） | Novix（os/kernel/） | 变化 |
|---|---|---|
| pcb.c 模拟 PCB | task.c | 真实内核栈 + 真实寄存器帧 + fd 表 + on_cpu 所有权 |
| scheduler*.c 模拟调度 | sched.c + isr.S | 每核 LAPIC timer 抢占，真实栈切换，全局就绪队列 + sched_lock |
| scheduler_context.c 伪造寄存器 | pt_regs（22 qword）+ iretq | 真实保存/恢复 SS..R15 |
| memory.c 虚拟记账 | pmm.c + heap.c + paging.rs + mm/ | 真实物理页（含引用计数）+ 四级页表 + VMA/按需分页/交换 |
| file_system.c 内存链表 mock | fs/vfs.c + fs/ramfs.c + fs/namei.c + fs/devfs.c | inode/f_op/i_op 分层的真 VFS（v0.2.0）+ 管道（fs/pipe.c，v0.5.0） |
| （模块无对应物） | drivers/ + fs/fat32.c | v0.8.0 全新：ATA 盘 + FAT32 持久根 + 统一驱动模型 + RTC 墙钟 |
| signal.c 打印日志 | signal.c | 真信号：生成/投递/sigreturn 蹦床三段式（v0.5.0）✅ |
| security.c 明文口令 | user/（login/passwd/etc.c）+ sha256.c | 真登录：salted SHA-256 /etc/shadow、getty→login→sh、SUID（v0.9.0）✅ |
| smp.c 空壳 | smp/（mp.c + apic.c + smp.c + trampoline.S）| 真多核：AP 启动/自旋锁/IPI/TLB shootdown（v0.7.0）✅ |
| procman ioctl | syscall.c int 0x80 | 真正跨越特权级，38 个编号 |
| comprehensive_test 无断言 | ktest.c | 全部带断言 + CI 退出码，41 项 |

## 3. 架构

```
QEMU (x86_64, -smp 2, -m 128M, IDE 硬盘 + RTC localtime)
 └─ Multiboot(kludge) → boot.S（长模式蹦床）→ kmain
      ├─ console (VGA 0xB8000 + COM1 串口，整消息原子写出)
      ├─ GDT64/TSS64（每核私有）, IDT64（共享）, IOAPIC/LAPIC（8259 停用）
      ├─ smp/mp.c: MP 表解析（EBDA/基本内存末 KiB/0xE0000 三段扫描）
      ├─ smp/smp.c: AP 蹦床（复制 0x8000）→ INIT-SIPI-SIPI → ap_entry
      │   每核：私有 GDT/TSS → 共享 IDT → %gs 锚 → LAPIC → per-CPU idle
      ├─ percpu.c: struct cpu{}[] + gs:0 自指锚，this_cpu() 走 %gs
      ├─ PMM 位图 + 每页引用计数 [RAM_TOP 内]，boot 期预留 1024 帧 swap 槽池
      ├─ 内核堆（空闲链表分配器）
      ├─ paging.rs (Rust): 四级页表 PML4/PDPT/PD/PT，内核 2MiB 大页
      │   恒等映射，用户 4KiB，invlpg，COW 克隆，PTE 裸读写
      ├─ mm/vma.c: 每进程 VMA 链（FILE/ANON，惰性映射，相邻匿名段合并）
      ├─ mm/swap.c: kswapd 水位触发扫描，时钟算法（PTE_A 二次机会），
      │   换出/抛弃后广播 TLB shootdown IPI
      ├─ fs/: namei.c 路径解析 + 挂载表（8 槽：遮蔽/合并/卸载）|
      │        vfs.c VFS+fd 表 | ramfs.c 堆 backed 文件系统 + initrd
      │        解包 | fat32.c 持久根文件系统（LFN 读写、簇分配、
      │        独立实例可重挂）| devfs.c /dev 发布 | pipe.c ringbuf
      │        管道（EOF/EPIPE）
      ├─ drivers/: driver.c 统一驱动模型（probe→init→/dev）|
      │           ata.c PIO 主盘（IDENTIFY/LBA28/MBR/FLUSH，ata_lock）
      │           rtc.c CMOS 墙钟（UIP/BCD/boot_time 快照）
      ├─ task.c: PCB/内核栈/首帧伪造/僵尸回收/fork(COW)/exec/waitpid
      ├─ sched.c: 全局就绪队列 + sched_lock + on_cpu 所有权协议；
      │   PRIORITY/RR/FCFS/EDF/RMS + aging，每核本地抢占
      ├─ signal.c: 生成/投递（两投递点）/sigreturn 蹦床/sigprocmask
      ├─ isr.c: 异常分流（ring3 杀进程 128+signo / ring0 panic）、
      │   IPI 处理、dump_regs（CR3/CR2/LAPIC id/GS base 分诊行）
      ├─ syscall.c: int 0x80 → 38 个编号（exit..dup2）
      ├─ elf.c: ELF64 登记式加载（FILE VMA 惰性映射）+ argv 压栈
      ├─ ktest.c: 43 项自测（3 early + 19 mt + 5 fs + 3 vm + 4 smp
      │   + 4 disk + 3 uid + 2 osh）
      └─ shell.c: 串口/键盘交互 shell（18 命令 + Tab 补全 + 历史，
          v0.9.0 起 console=debug 调试后门；默认路径是 getty_loop）
用户程序 os/user/（28 个）: crt0 + ulib(malloc/printf/syscall 包装)
  + sha256(用户态孪生) + etc(登录世界共用 /etc 解析)，
  hello, spawner, fault_div, fault_segv, filetest, forker, forkacct,
  execer, malloctest, sigtest, masktest, pipetest, memhog,
  getty, login, osh, id, whoami, who, passwd, useradd, cat, touch,
  uidtest, echo, grep, init, iotest
构建期: tools/mkramfs.py 将用户 ELF64 打包为 NRFS initrd 嵌入内核，
        启动时 ramfs_mount_initrd() 解包到 /bin/；tools/mkfatimg.py
        生成 64 MiB FAT32 镜像（MBR + LBA 2048 分区 + LFN 项）作
        IDE 主盘，-append root=/dev/hd0 时 FAT32 根接管（kmain）
```

## 4. 关键机制

### 4.1 统一中断帧 pt_regs（22 qword）
所有中断/异常/系统调用共用一种栈帧布局（`isr.h`）：15 个 GPR +
int_no/err + rip/cs/rflags/rsp/ss。长模式下 CPU **总是**压入
rsp/ss（i386 的"ring3 才有"判断退役）。`isr_common` 汇编保存全部
上下文后调用 C `isr_handler`，其返回值作为新栈指针装回 RSP——
一次 `iretq` 同时完成"恢复"与"切换"。新任务首帧在 C 中伪造：
rip=入口、rflags=0x202(IF)、内核线程 cs=0x08/ss=0x10、用户进程
cs=0x1B|3/ss=0x23|3+用户栈。

### 4.2 上下文切换与任务所有权（v0.7.0）
`schedule()` 在每核的 tick/阻塞点被调用，全程持 sched_lock：
1. 若 prev 仍归本核所有（`on_cpu == c->id`）：保存 `prev->kesp`
   （当前帧地址）、RUNNING 降级 RUNNABLE、**停泊 prev（on_cpu=-1）**
   ——停泊与选定后继在同一临界区原子完成
2. 从全局就绪队列按策略选出 next（条件：RUNNABLE **且** 已停泊，
   `task_pickable()`）；队列空则退到本核私有 idle
3. **收养 next（on_cpu=c->id）**、`next->page_dir != CR3` 则重载、
   TSS RSP0 换栈、用户任务在此投递 pending 信号（CR3 已装载，
   用户栈可写）
4. 返回 `next->kesp`（旧帧或首帧），epilogue `iretq`

所有权协议是 SMP 正确性的基石：任务置 SLEEPING 后、真正切出前
存在窗口，远端核的 tick/task_wake 可能把它翻回 RUNNABLE——没有
所有权标记时第二个核会"偷"走一个仍在原核栈上执行的任务，
双核共写 kesp/共享内核栈（v0.7.0 开发期 kswapd panic 的根因）。
契约全文见根目录 DESIGN.md §5.2。

### 4.3 同步模型（v0.7.0）
- **自旋锁**（`spinlock.h`）：xchg 忙等 + pause；`_irqsave` 变体
  才动中断标志——cli/sti 只护本核，持锁中断重入同一把锁是 SMP
  经典死锁
- **锁次序**（成文于 spinlock.h 头注）：
  `sched_lock > fs_lock/pipe > pmm_lock/heap_lock > console_lock`；
  pmm/heap/console 是叶子锁，持有时不得再取任何锁
- 持锁禁区：阻塞/睡眠、kmalloc（拿 heap_lock，注意次序）、
  kprintf 拿 console_lock（叶子上可用）
- 控制台整条消息单次持锁写出（多核并发输出不交错）；
  panic 直通模式（持锁者死亡时崩溃信息仍可打印）
- 页引用计数（COW 用）全部在 pmm_lock 内增减

### 4.4 地址空间
- 内核恒等：低 2MiB 大页映射，**共享进每一个用户页目录**——
  syscall 在调用者 CR3 上直接解引用用户指针，无需跨 CR3 拷贝
- 用户分区（根 DESIGN.md §1 为权威）：代码 `0x4000_0000` 起、
  brk 堆至 `0x8000_0000`、匿名 mmap 从 `0x8000_0000` 向上、
  栈顶 `0x7FFF_FFFF_E000`（16 页，无 guard page）
- VMA 惰性映射：ELF 段只登记 FILE VMA；#PF 命中 → FILE 页从
  inode 读入 / ANON 页清零 / 换出标记（PTE 位 10）从槽读回；
  未命中维持 SIGSEGV。**内核态缺页同路径解析**（syscall 在
  调用者页表上解引用用户指针，ring0 #PF on 用户地址是常态）
- 交换：PMM 预留 1024 帧槽池；kswapd 水位触发，脏 FILE 页按
  MAP_PRIVATE 语义换出（绝不回写 ramfs ELF inode），COW 共享页
  跳过；fork 前 swap_prefault 赎回全部换出页
- TLB 语义（页目录单加载不变量）：用户页目录至多同时加载在一核
  （on_cpu 所有权的推论），任务切换必重载 CR3 全量冲刷——改写
  **当前任务** PTE（COW/munmap/缺页）本地 invlpg 即够；唯 kswapd
  改写**他人** PTE 时广播 shootdown IPI（对端按 CR3 匹配）

### 4.5 SMP 启动（v0.7.0）
- `mp.c` 解析 MP Table（`_MP_` 浮动指针三段扫描 + PCMP 表 CPU
  条目；PCMP 头 44 字节，oem_ptr/oem_size/oem_count 三字段易漏；
  SeaBIOS 在 HTT 开启时每封装只报一个逻辑 CPU，QEMU 需
  `-smp N,sockets=N`）
- `trampoline.S` 位置无关蹦床复制 0x8000，复用内核 PML4；
  BSP 发 INIT-SIPI-SIPI，AP 经长模式进 `ap_entry()`
- AP 初始化次序钉死：**私有 GDT/TSS → 共享 IDT → %gs 锚 →
  LAPIC → per-CPU idle → 门前停泊**（先 GDT 后 IDT：蹦床迷你
  GDT 的 0x08 仍是 32 位描述符；GS 最后：此后才可安全读
  this_cpu()）；BSP `smp_release()` 开门后 AP 解屏蔽 LAPIC
  timer 收养 idle 加入调度
- 时钟：每核 LAPIC timer 100Hz tick + 本地抢占；PIT 退居 BSP
  校准基准

### 4.6 控制台输入（双路 + 兜底 + 行编辑）
- PS/2 键盘（IRQ1）：scancode set 1 → ASCII；COM1 串口（IRQ4）：
  `-serial stdio` 终端输入走这条路，CR→LF 归一化
- 两路汇入同一环形队列；`keyboard_getchar()` 轮询时顺带排水串口
  RX（锁保护），即使中断丢失也能收到字符——CI 场景的关键可靠性
- 行编辑：Backspace（0x08/0x7F 双收）、Tab 补全（命令表 +
  `spawn` 参数经 vfs_readdir 实时枚举 /bin，多匹配 bash 式列出）、
  ↑/↓ 历史（16 条环形，串口 ESC[A/B 与 PS/2 0x48/0x50 统一翻译）、
  Ctrl+C → SIGINT 给前台任务

### 4.7 文件系统
Linux VFS 的教学级缩影，核心是两类操作表的分离：

- **`inode_operations`（目录树形）**：`lookup/create/mkdir/unlink/
  truncate/readdir`——路径解析（namei.c）只通过它逐级下钻
- **`file_operations`（打开后行为）**：`read/write/close`——设备、
  普通文件与管道在此统一（"/dev/console 是一个文件"的最小实现）
- **ramfs**：节点树 + 数据缓冲住内核堆，扩容走"分配-复制-释放"
  （堆没有 realloc）
- **fd 表**：PCB 内 `struct file *fd[16]`；fd 0/1/2 = /dev/console；
  `task_exit()` 释放全部 fd（kill 路径由 waitpid 兜底，幂等）
- **管道**：匿名 inode（无路径，f_op 路由）+ ringbuf 4096；空/满
  返回 EAGAIN_RETRY(-2) 由 ulib 包装成阻塞语义；对端全关 →
  EOF 0 / EPIPE -1
- **挂载表**（namei.c，8 槽，v0.8.0）：`vfs_mount` 把任意文件系统
  根钉在父目录名下；lookup 时挂载**遮蔽**底层同名项，`..` 从挂载根
  爬回父目录，readdir 先列挂载点再列本体（`ls /` 里 dev、tmp 与
  FAT 本体同框），`vfs_unmount` 干净拆除
- **FAT32**（fat32.c，v0.8.0）：BPB + 全 FAT 扫描定空闲簇（FSInfo
  只是参考）；`dir_walk` 单遍扫描器统一 lookup/readdir/create/
  unlink 四路径；LFN 读写（8.3 别名恒大写，`needs_lfn` 须大小写
  敏感比较）；写 = 簇分配 + FAT 链接 + dirent 追加/尺寸回写，
  删 = 0xE5（含 LFN 区间）+ 簇链归还；per-instance 锁 + 512B
  bounce 缓冲；inode 按 (父簇, sname) 键控缓存
- **根文件系统切换**（kmain.c）：`root=/dev/hd0` 时 FAT32 根取代
  ramfs 根，ramfs 只留 /dev、/tmp 两个目录钉在 FAT 根下；无 FAT
  卷则诚实回退 ramfs
- **已知限制**：inode 无引用计数、无 rmdir（非空目录）、无 fsck/
  日志；权限位与属主只活 in inode 缓存（FAT 不存 Unix 位，boot 期
  fs_perm_fixup 重建，运行期改动不落盘）

### 4.8 信号
三段式（生成/投递/执行），信号集 1..31，SIGKILL 不可捕获不可
屏蔽：

- **生成**（`signal_send`）：目标可捕获 → 挂 `sigpending` 位
  （屏蔽与否投递时才查，POSIX 语义）；否则立即默认终止
- **投递**：仅两处改写 pt_regs——`isr.c` 中断/syscall 返回路径 +
  `sched.c` 切换到目标任务时（须 CR3 已装载，保存帧写到用户栈）；
  内核帧（cs&3 != 3）跳过；每帧只投一个，余下等 sigreturn 后再投
- **执行**：用户栈压 `[sigrestorer | 保存的 pt_regs]`，帧改写
  rip=handler、rdi=signo；handler `ret` 进蹦床 → SYS_SIGRETURN
  恢复原帧。fork 继承 handlers/mask，不继承 pending（POSIX）

### 4.9 磁盘、驱动模型与墙钟（v0.8.0）
- **统一驱动模型**（driver.c）：`device_driver{name, probe, init}` +
  `device{name, mode, f_op}`；boot 期静态总线遍历 probe→init→注册
  表，devfs 经 `ramfs_mknod` 发布成 /dev 节点（console/null/zero/
  hd0——块设备 `<blk>`，f->pos 计扇区的裸块访问）。注册表 boot 后
  只读无需锁；设备 fops 自带硬件锁（hd0→ata_lock，通道级叶子锁：
  寄存器组是共享状态，双核交叠命令会互踩）
- **ATA PIO**（ata.c）：主通道主盘；先嗅总线（空槽位读回 0xFF 即
  无盘）再 IDENTIFY 取扇区数；LBA28 读写 + 写后 FLUSH CACHE 落盘；
  同步轮询（每扇区 200K 次 status 轮询上限），IRQ 模式留 1.x；
  MBR 分区表解析（首个分区 LBA 2048 起）
- **RTC**（rtc.c）：CMOS 0x70/0x71（索引位 7 顺带屏蔽 NMI）；等
  UIP 更新周期过去、整组读回、秒值复读证明稳定；BCD 按 B 寄存器
  DM 位解码、12 小时制 PM 位兜底；世纪寄存器不通用，年 = 2000+yy。
  启动拍 boot_time 快照；`date` 打本地时间 + unix 秒 + uptime
  （无时区机制，诚实标注 local）
- **断电语义**（ktest fat_powercycle）：另起独立 fat32_mount 实例
  读同一张盘——RAM 状态骗不过重挂，只有真到达盘片的数据幸存；
  读完 vfs_unmount 即"关机"

### 4.10 用户与登录（v0.9.0）
"你是谁"的唯一权威来源是 `task->uid/gid/ruid`（uint16_t）；
契约全文（继承规则、setuid 语义、SUID、权限检查点）见根目录
DESIGN.md §6。

- **登录链**：v0.9.0 由内核 getty_loop 读 /etc/inittab 拉起 getty；
  v1.0.0 起内核 pid 1 改为监督用户态 /bin/init（见 §4.11），init 读
  /etc/init 脚本执行——
  fork+exec /bin/getty（打 /etc/issue、读用户名）→ 同 pid exec
  /bin/login <name>（/etc/shadow 校验 salted SHA-256；错误用户名
  与错误密码同报 "Login incorrect"；3 次失败 3 秒刹车；成功 →
  写 utmp → setgid/setuid 降落 → chdir(home) → exec(sh, "-sh")）。
  链上每个环节同 pid 换程序，shell 退出即整链归零，init 收尸后
  respawn（SysV 规则）；`console=debug` 保留内核 shell 作调试后门
- **权限检查**（vfs.c `vfs_permitted`）：uid 0 全通过；否则按
  inode 属主比对 owner/group/world 三选一（owner 胜 group 胜
  world）。检查点：open（含 O_CREAT 父目录 W、O_TRUNC/APPEND 的
  W）、unlink、mkdir、chdir（目录 X）；新文件属主 = 创建者
- **FAT32 权限补正**（kmain.c `fs_perm_fixup`）：FAT 不存 Unix
  位，boot 期对 inode 缓存一次性补正——/bin 全部 +x、/bin/passwd
  加 S_ISUID、/etc/shadow 收 0600、/home/<name> 按 /etc/passwd
  chown
- **SUID exec**（task_exec/task_create_user 同规则）：二进制带
  S_ISUID 且属主非调用者 → uid 改采文件属主，ruid 留作调用者
  记录（教学简化：单 uid，无 euid 拆分）；/bin/passwd 是系统唯一
  SUID 二进制
- **密码体系**：salted SHA-256 三份同算法——内核 sha256.c、
  ulib 用户态一份、构建期 tools/mkpasswd.py；root/alice 密码每次
  构建随机生成（记录 build/etc-passwd.txt），ktest 用户固定
  "ktest-pass" 做测试锚点。无慢因子（bcrypt/PBKDF2 留 1.x）
- **每进程 cwd**：`task->cwd`（namei 持有 inode 引用），chdir 校验
  X 位；相对路径解析从 cwd 起步；fork/spawn 继承，exec 保持
- **已知限制**：权限补正不落盘（重启重建，运行期 chown/useradd
  同样不持久）；logout 不清 utmp 行；单 tty（COM1）；SHA-256 无
  慢因子

### 4.11 脚本化 shell 与用户态 init（v1.0.0）
- **DUP2（38 号）**：新 fd 并入调用者的打开文件描述（共享偏移），
  目标 fd 隐式 close——osh 管道/重定向的唯一接线方式
- **osh**：fork+exec 每个管道级，父进程持 pipe 两端再逐级 dup2 进
  子进程的 0/1；`$VAR` 在切词前展开；词槽 arena 保证 argv 是规范
  C 字符串；后台作业表 exit 前 kill+reap
- **用户态 init**：内核 pid 1 只做监督（/bin/init 死了就重生）；
  /bin/init 读 /etc/init——一次性行顺序 spawn+wait，首个 respawn
  行监督到底；无脚本时回退 /etc/inittab 契约
- **elf argv 对齐修复**：argv 数组位置钉死在 argc_off+8，v0.9.0 的
  8 字节空洞（arr_beg≡0 mod 16 时 argv[0] 读到未写栈）根除
- **/tmp 0777**：凡人重定向的前提（无 sticky 位，教学取舍）

## 5. 构建与运行
- 编译器：zig cc（clang+lld，`-target x86_64-freestanding`，
  `-mno-red-zone`）+ rustc（`x86_64-unknown-none` staticlib，
  `-C relocation-model=static`）；Windows 下工具链内置于 `tools/`
- `python build.py` 全量构建；`build.py run` QEMU 交互（默认
  2 CPU）；`build.py test [-smp N]` 无人值守自测（串口日志 +
  isa-debug-exit 退出码，1 = 全过）
- `python tools/test_shell.py`：交互式 shell 自动化（71 项检查，
  三会话：console=debug 内核 shell 全量回归 50 项 + 登录/脚本化/
  改密 18 项 + 断电后新密码存续 3 项，会话跑在 2 CPU 上）
- 用户程序构建为 ELF64 后由 `tools/mkramfs.py` 打包 NRFS initrd
  嵌入内核，启动时解包挂到 `/bin/`；`tools/mkfatimg.py` 同步生成
  64 MiB FAT32 镜像（同一批 /bin + novix.txt 标记文件）挂 IDE
  主盘，run/test 以 `-rtc base=localtime -append root=/dev/hd0`
  启动——FAT32 做根，无卷时内核诚实回退 ramfs
- `build.py iso`：grub-mkrescue 生成 novix.iso（宿主缺 grub 工具链
  时如实跳过）；`tools/check_syscalls.py` 随构建比对 docs/syscall.md
  与 syscall.h（ROADMAP 验收 #4）
- 收版硬门槛：`build.py test -smp 2` 与 `-smp 1` 双跑 +
  `test_shell.py` 三者全绿

## 6. 与真实 OS 的差距（v1.0）

Novix 在"机制"意义上是真实 OS：真引导、真抢占、真跨特权级、
真多核。但与 Linux 等生产级 OS 相比仍有实质差距，如实列出
（与当初"诚实移除假子系统"同一精神）：

| 维度 | 真实 OS（以 Linux 为参照） | Novix 0.9 现状 |
|---|---|---|
| 进程创建 | fork()/exec()/COW、线程 | fork/exec/COW 已落地；**无线程** |
| 文件系统 | VFS + 持久化文件系统 | VFS/ramfs/devfs/管道/FAT32 持久根（LFN）+ 权限位 + per-task cwd 已落地；无 rmdir、无 fsck、权限不落盘 |
| 虚拟内存 | 按需分页、COW、交换、mmap、brk、rmap | 前四者已落地；无反向映射、无文件回写 mmap |
| IPC 与信号 | 信号、管道、共享内存、socket | 信号+管道已落地；无 shm/socket |
| 多核与同步 | per-CPU 队列、RCU、可抢占内核 | 单全局就绪队列（教学规模够用）；仅自旋锁 |
| 设备驱动 | 统一驱动模型，磁盘/网络/USB | VGA、COM1、PS/2、PIT/LAPIC、ATA 磁盘、RTC 已统一注册进 /dev；无网络/USB/GPU |
| 系统调用 | 300+ | 37 个 |
| 安全 | 用户/权限/能力/LSM、ASLR | uid/gid/ruid + 登录链 + VFS 权限位 + SUID 已落地；无能力/LSM/ASLR、哈希无慢因子 |
| 平台 | 64 位、UEFI/ACPI、真实硬件 | 64 位 ✅；仅 QEMU -kernel；poweroff 依赖 isa-debug-exit |
| 网络 | 完整 TCP/IP 协议栈 | 无 |
| 时钟 | HPET/RTC 墙钟、时区 | 每核单调 tick + RTC 墙钟（`date` 打本地时间）；无时区机制 |

上述差距即 ROADMAP 的主要来源：v1.0.0 里程碑（ISO 引导、
用户态 sh、init）与 1.x 延伸方向。
