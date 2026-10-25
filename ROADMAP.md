# Novix 版本路线图

本文档规划 Novix 从 v0.1.0 走向"真正的操作系统"的完整开发大纲。
每个版本包含：**背景（现状代码依据）→ 目标 → 技术设计 → 任务分解 →
测试与验收 → 依赖与风险**。

> 版本号约定：`主.次.修订`——次版本号递增 = 交付一个完整子系统；
> 修订号递增 = 稳定性/小改进，不新增子系统。每个次版本打 git tag，
> 并在 `CHANGELOG.md` 记录。

## 总览与依赖关系

```
0.1.0 ──► 0.1.1 ──► 0.2.0 ──► 0.3.0 ──► 0.4.0 ──► 0.5.0 ──► ... ──► 1.0.0
 已完成    已完成    已完成   已完成   已完成   已完成           里程碑
             │         │         │         │         │
             │         │         │         │         └─► 管道依赖 fd 表（0.2.0）
             │         │         │         └─► fork/COW 深拷贝 pt_regs、exec 走 ELF64——
             │         │         │            帧格式与页表格式依赖 0.3.0 先定死
             │         │         └─► 后续全部版本跑在 x86_64 + Rust 工具链之上
             │         └─► spawn 改造依赖异常杀进程稳定（0.1.1）
             └─► 所有后续版本的异常安全前提

0.6.0 ──► 0.7.0 ──► 0.8.0 ──► 0.9.0 ──► 1.0.0
按需分页    SMP     磁盘/FAT32 用户与登录   里程碑
             │                  │            │
             │                  ├─► 登录链依赖 fork/exec（0.4.0）+ ELF 加载（0.2.0）
             │                  └─► /etc/passwd 持久化依赖 FAT32（0.8.0）
             │                                 └─► 注册依赖登录链（0.9.0）+ 持久化（0.8.0）
             └─► SMP 依赖 0.6.0 的 TLB shootdown 语义
```

| 版本 | 主题 | 相对规模 | 核心交付 |
|---|---|---|---|
| 0.1.0 | 真实内核（已完成） | — | 引导、抢占多任务、分页、ring3、shell、10 项自测 |
| 0.1.1 | 健壮性（已完成 2026-08） | S | 异常杀进程而非 panic；kill 命令；分级日志；自测扩至 13+12 |
| 0.2.0 | ramfs + VFS（已完成 2026-08） | M | VFS/ramfs/devfs、文件系统调用、fd 表、spawn 走 `/bin`、shell 文件命令、自测扩至 18+21 |
| 0.3.0 | x86_64 长模式 + Rust（已完成 2026-09） | L | 四级页表、长模式蹦床、ELF64、C/Rust 双语言；18+21 测试原样全绿 |
| 0.4.0 | fork/exec + COW（已完成 2026-09） | L | fork/exec/写时复制、页引用计数、pid 复用（slot 绑定）、COW 缺页；自测 22/22 |
| 0.5.0 | 信号与用户堆（已完成 2026-09） | M | sbrk+malloc/free（合并式空闲链）、信号三段式（生成/投递/sigreturn 蹦床）+sigprocmask 屏蔽+Ctrl+C SIGINT、管道（ringbuf+EOF/EPIPE）、waitpid WNOHANG；自测 27/27、DESIGN.md 定契约 |
| 0.6.0 | 按需分页与交换（已完成 2026-09） | L | VMA 地址空间 + 按需分页（内核态缺页同路径解析）+ 交换（PMM 预留槽池 + kswapd 时钟算法）+ 匿名 mmap/munmap + `mem -v`；自测 30/30、shell 42 |
| 0.7.0 | SMP 多核（已完成 2026-10） | L | MP 拓扑解析、AP 启动蹦床、自旋锁+锁次序、全局就绪队列+on_cpu 任务所有权、LAPIC timer/IPI、TLB shootdown；自测 34/34、shell 45/45 |
| 0.8.0 | 磁盘与驱动框架（已完成 2026-10） | L | ATA PIO（LBA28+MBR）、FAT32 读写（LFN 读写）、统一设备模型（/dev 注册表）、RTC 墙钟+`date`、`root=/dev/hd0` rootfs 切换（挂载表+遮蔽+readdir 合并）、断电语义测试（独立实例重挂）；自测 38/38、shell 50/50 |
| 0.9.0 | 用户与登录（已完成 2026-10） | M | uid/gid/ruid 身份层 + 11 系统调用、VFS 权限检查（root 旁路+POSIX 三选一）、FAT32 权限补正、SUID exec、getty→login→sh 登录链（inittab respawn）、salted SHA-256 密码三份同算法、id/whoami/who/passwd/useradd 命令；自测 41/41、shell 66/66 |
| 1.0.0 | "真实 OS" 里程碑（已完成 2026-10） | M | osh 脚本化 shell（管道/重定向/后台/变量 + DUP2）、用户态 init 读 /etc/init、/etc/motd、ISO 目标（grub-mkrescue）、文档四件套、**开箱注册**（出厂零账户镜像、/bin/register 首启注册向导 root 密码+首个用户、getty 空账户分支、ktest fixture 自建）；自测 43/43、shell 78/78 |

规模：S ≈ 数天、M ≈ 1-2 周、L ≈ 2-4 周（个人业余节奏）。

---

## v0.1.1 —— 健壮性：让一个进程的死亡不再等于整机死亡

### 背景（现状代码依据）
`isr.c`/`syscall.c` 的 `isr_handler()` 中，任何非 0x80、非已知 IRQ 的
中断走 `dump_regs("kernel panic") + panic()`——包括用户态除零（int 0）
和用户态页错误（int 14）。也就是说：**任何一个用户程序 bug 都会
kernel panic**。这是 0.2.0 之后一切工作的前提——没有异常隔离，
后续每个子系统的崩溃都难以定位。

### 目标
用户态异常 → 杀死当前进程并完整回收资源；仅内核态异常才 panic。

### 技术设计

1. **异常分类**（`isr_handler()` 改造）：
   - 判据：`r->cs & 3 == 3`（来自 ring3）
   - 用户态 + `int_no ∈ {0,4,5,6,13,14}` → `task_kill(current, int_no)`
   - 其余（内核态异常、double fault）→ 保留 panic
2. **`task_kill()`**（新函数，`task.c`）：
   - 设置 `exit_status = 128 + int_no`（对齐 Unix 信号退出码语义）
   - 复用 `task_exit()` 的回收路径（唤醒 waitpid 父进程、孤儿 reparent）
   - 页错误时先读 CR2 打印 `fault addr`，便于诊断
3. **回收加固**（`task_waitpid()` 现状已能回收页目录/内核栈，
   需补充）：
   - 被杀进程若有 SLEEPING 子进程，reparent 后由 init 收割
   - `task_dump_table()` 增加 SIG 退出原因列
4. **日志分级**（`console.c`/`kprintf`）：
   - 新增 `klog_err/klog_warn/klog_info`，格式 `[  12.34] ERR: ...`
   （tick 时间戳 + 级别），串口输出不变，CI 可 grep

### 任务分解（✅ 全部完成，2026-08）
- [x] `task.c`：新增 `task_kill()`/`task_kill_task()`；`exit_status`
      语义区分自愿/被杀（`kill_signo` 字段）
- [x] `syscall.c`：`isr_handler()` 异常分流（用户态判定 + int 14 CR2 读取；
      异常→POSIX 信号映射 SIGFPE/SIGSEGV/SIGILL/SIGTRAP）
- [x] `klog.c`：`klog_err/warn/info` 分级日志（时间戳 `[  2.01] ERR:` 格式）
- [x] `ktest.c`：新增测试「fault_div_isolation」——断言退出码 = 128+SIGFPE
- [x] `ktest.c`：新增测试「fault_segv_isolation」——读地址 0x1，
      断言 SIGSEGV 路径（128+11）+ `kill_signo` 记录
- [x] `ktest.c`：新增测试「kill_other_task」——杀 SLEEPING 状态任务
- [x] `os/user/`：新增 `fault_div.c`、`fault_segv.c` 两个故障注入程序
- [x] shell：`ps` 输出增加 EXIT 列（正常退出显示码、被杀显示 sigN）；
      新增 `kill PID` 命令（含守卫：不能杀 shell 自身）
- [x] `test_shell.py`：8 项检查扩至 12 项（fault_div/fault_segv 隔离、
      kill 无效 pid、kill shell 守卫）

### 验收标准
1. ✅ 用户程序除零后：shell 无 panic、继续可交互；`ps` 中该进程为 ZOMBIE
2. ✅ `python build.py test`：13/13 通过（3 early + 10 mt），QEMU 退出码 1
3. ✅ 串口日志含时间戳与级别，能定位"哪个 pid 在哪个时刻因何死亡"
4. ✅ 空闲页计数在杀进程前后一致（无泄漏）

### 依赖与风险
- 依赖：无（当前 0.1.0 基线即可开工）
- 风险：task_exit 在异常帧上下文中被调用，需确认
  `for(;;) kyield()` 的路径在 int 14 帧下仍成立；若不成立，
  需要在 `isr_handler` 返回前手工切换到目标任务的 kesp

---

## v0.2.0 —— ramfs 文件系统：程序不再"焊死"在内核里

### 背景（现状代码依据）
用户程序由 `tools/embed_user.py` 在**构建期**嵌入内核镜像
（`elf.c` 的 `elf_find_image()` 查表），`sys_spawn()` 只认嵌入表。
没有 fd 表、没有路径、没有 inode——`sys_write()` 只接受魔数 fd 1/2。
这决定了 0.4.0 的 exec 无处加载、0.5.0 的管道无 fd 可挂。

### 目标
内存文件系统 + Unix 风格文件系统调用；`spawn` 改为从 `/bin/` 加载。

### 技术设计

1. **VFS 抽象**（新文件 `fs/vfs.c` + `include/vfs.h`），
   参照 Linux VFS 的分层但大幅简化：
   ```c
   struct inode {
       uint32_t ino;              /* 自增编号 */
       uint16_t mode;             /* 目录/文件/设备 */
       uint32_t size;
       void *fs_priv;             /* 具体 fs 的私有数据 */
       struct file_operations *f_op;
   };
   struct file {                  /* fd 表项 */
       struct inode *inode;
       uint32_t pos;              /* 读写游标 */
       uint16_t flags;             /* O_RDONLY/O_WRONLY/... */
   };
   ```
2. **ramfs**（新文件 `fs/ramfs.c`）：
   - 节点 = `{name[32], children 链表, data 指针, size}`，目录即节点链
   - 数据从内核堆分配，扩展用 realloc 式"分配-复制-释放"
3. **路径解析**（`fs/namei.c`）：
   - 绝对路径 `/` 分隔逐级查找；拒绝 `..` 越过根（简单防御）
4. **fd 表进 PCB**：`struct task` 增加
   `struct file *fd[TASK_MAX_FD(16)]`；fork 时共享（0.4.0）
5. **系统调用新增**（编号续接，`syscall.h`）：
   `OPEN(11) READ(12) CLOSE(13) LSEEK(14) STAT(15)`，
   既有 `WRITE(2)` 改走 fd 表
   - `open` 返回最小可用 fd；fd 0/1/2 预指向 `/dev/console`
   - `write(1,...)` 语义不变（写 console 特殊文件）
6. **控制台设备文件**：`/dev/console` 挂到 ramfs 树，`f_op` 指向
   console 的读写字符函数——首个"设备即文件"的实例
7. **构建改造**（`tools/embed_user.py` → `tools/mkramfs.py`）：
   - 用户 ELF 打包为自定义 ramfs 镜像（头部 + 节点表 + 数据 blob）
     嵌入内核；`kmain` 启动时解包建树，`/bin/hello` 就位
8. **spawn 改造**：`sys_spawn(name)` → 打开 `/bin/<name>`，
   `read` 全量到堆，`elf_load` 从内存缓冲加载——内核不再持镜像表

### 任务分解（✅ 全部完成，2026-08）
- [x] `include/vfs.h` + `fs/vfs.c`：inode/file/f_op/i_op 抽象、open/read/
      write/lseek/stat/mkdir/unlink/readdir、fd 分配与安装
- [x] `fs/ramfs.c`：mkdir/create/write/read/lookup/unlink/truncate/readdir
      + initrd 镜像解包（`ramfs_mount_initrd`）
- [x] `fs/namei.c`：路径解析（`namei`/`namei_parent`，`.` 与 `..` 支持，
      根的 `..` 停在根）
- [x] `fs/devfs.c`：`/dev/console`（可读写）+ `/dev/null`（只写）注册
- [x] `task.h`：`fd[TASK_MAX_FD=16]` 字段；任务创建时 fd 0/1/2 指向
      `/dev/console`；`task_exit` 与 `waitpid` 均关闭全部 fd（幂等）
- [x] `syscall.c`：OPEN(11)/READ(12)/CLOSE(13)/LSEEK(14)/STAT(15) +
      WRITE(2) 改走 fd 表 + SPAWN(6) 走 `/bin` 路径 + 用户缓冲区校验
- [x] `tools/mkramfs.py`：构建期打包（NRFS 格式：头 + 节点表 + blob），
      `build.py` 接线，`embed_user.py` 退役
- [x] `elf.c`：`elf_find_image` 退役；`task_spawn_path()` 从 VFS 全量读入
      后 `elf_load`
- [x] shell：`ls`（含类型/大小）、`cat`、`mkdir`、`touch`、`rm`、
      `echo TEXT > FILE` / `>>` 重定向；`spawn` 支持绝对路径
- [x] `ktest.c`：新增 fs 测试组 5 例（create_rw / lseek / mkdir_readdir /
      fd_exhaust / devfs_initrd），自测扩至 18
- [x] `os/user/filetest.c`：用户态文件 syscall 端到端验证程序（21 项断言）
- [x] `tools/test_shell.py`：shell 检查扩至 21 项（文件命令 + 重定向 +
      filetest）
- [x] 内核堆扩容 8MiB → 16MiB（`HEAP_END`），boot 日志含堆容量与页数

### 验收标准
1. ✅ `spawn hello` 走 ramfs 路径加载（`task_spawn_path("/bin/hello")`，
   initrd 解包日志 + `vfs_create_rw`/`devfs_initrd` 测试覆盖）
2. ✅ shell `mkdir /tmp/dir1`、`echo novix fs > /tmp/a.txt`、
   `cat /tmp/a.txt` 全链路可用（test_shell.py 自动化验证）
3. ✅ fd 用尽后 `open` 返回 -1，关闭后按最低位复用（`fd_exhaust` 测试）
4. ✅ 进程退出时（`task_exit`）即关闭全部 fd；被 kill 路径由 `waitpid`
   兜底关闭（幂等）
5. ✅ 自测 18/18 通过（3 early + 10 mt + 5 fs），shell 检查 21/21 通过

### 依赖与风险
- 依赖：0.1.1（文件系统代码自身的异常需被隔离）
- 风险：内核堆只有 4MiB（`HEAP_START..HEAP_END`），ramfs 数据可能
  压力堆——0.2.0 内将堆扩至 16MiB 并加入"堆水位"日志
- 设计取舍：inode 无引用计数（fd 与 unlink 的竞态 0.2.0 不处理，
  记入已知限制，0.4.0 补 i_count）

---

## v0.3.0 —— x86_64 与 Rust：给操作系统换 64 位地基

### 背景（现状代码依据）
内核整体是 i386：两级页表 + PSE 4MiB 恒等映射（`paging.c`）、
19 dword 的 `pt_regs`（`isr.h`）、ELF32 加载（`elf.c`）、
Multiboot 1 引导（`boot.S` 42 行）。两个矛盾决定移植宜早不宜迟：
0.4.0 的 fork/COW 要**深拷贝 pt_regs**、要**在 PTE 位上做页表魔法**——
帧格式与页表格式先定死，才能避免写两遍；同时借此版本引入
Rust（`no_std`）：`paging.c` 反正要从两级重写为四级，正好作为
首个 Rust 模块落地，存量 C 一律不动。

### 目标
`qemu-system-x86_64` 引导的 C+Rust 双语言内核；现有全部测试
**原样**通过。

### 技术设计

1. **长模式蹦床**（`boot.S` 重写，42 → ~150 行）：
   - GRUB/QEMU 的交接点仍是 32 位保护模式，内核自查
     `CPUID.80000001H.EDX.LM`，无则 panic
   - `.bss` 静态区手工搭 PML4 → PDPT → PD，内核低区 2MiB 大页
     恒等映射（"用户页目录共享内核低区"的设计原样保留）
   - `CR4.PAE=1` → `CR3=表基址` → `WRMSR EFER(0xC0000080).LME=1`
     → `CR0.PG=1` 同时远跳 L=1 代码段 → 64 位模式
   - 重载 64 位 GDT（TSS64 需 16 字节对齐）、设 rsp、调 `kmain`
2. **引导载体**（阶段 0 spike 已定夺）：**方案 A 定版**——
   Multiboot 1 + a.out-kludge 加载 ELF64，保留 QEMU
   `-kernel` 直启（spike 实测通过，无需 GRUB/ISO）；
   EAX magic 需兼容 `0x2BADB002`/`0x2BADB003`（QEMU 11.1
   实际传后者）
   - ~~方案 B：Multiboot 2 + `grub-mkrescue` ISO~~（备胎，
     仅当 QEMU 行为在后续版本回归时启用）
3. **陷阱路径**：
   - `pt_regs` 拓宽为约 23 qword（16 个 GPR + rip/cs/rflags/
     rsp/ss + int_no/errno）；**统一帧 + 单次 iret 完成切换的
     设计原样保留**
   - IDT 门 8 → 16 字节，`iretd` → `iretq`；内核编译加
     `-mno-red-zone`；TSS64 权限栈只剩 RSP0（TSS 偏移 4）
   - PIC/PIT/PS2/VGA/COM1 在长模式下照常工作——驱动层零改动
4. **四级页表（首个 Rust 模块 `kernel/paging.rs`）**：
   - `no_std` staticlib，target `x86_64-unknown-none`；
     PML4/PDPT/PD/PT 四级、内核 2MiB 大页、用户 4KiB 页、
     48 位规范地址检查；PTE 位域用类型安全的标志封装
   - 对 C 只导出 `extern "C"` 函数；`novix.h` ↔ `novix.rs`
     类型手工对齐（POD 结构，不跨边界传 Rust 集合类型）
5. **Rust 工具链接入**（`build.py`）：
   - 前置：`rustup target add x86_64-unknown-none`
     （✅ 已安装，2026-09-22 随阶段 0 完成）
   - `rustc --crate-type staticlib` 产出 `.a`，与 C 目标文件一起
     经 LLD 链接；`panic_handler` 落 klog；起步阶段关 LTO
6. **系统调用**：保留 `int 0x80`（长模式合法），ABI 改
   rax=编号 + rdi/rsi/rdx 传参；`syscall/sysret`（LSTAR MSR）
   记为 0.7.0 SMP 版的优化项
7. **ELF64**：`elf.c` 适配 `Elf64_Ehdr/Phdr`（EM_X86_64）；
   用户入口 0x08048000 → 0x400000、栈顶 0xC0000000 →
   0x7FFF_FFFF_E000；`user.ld`/`crt0.S`/`ulib.c` 同步改
8. **类型拓宽**：`novix.h` 地址/长度类型 → 64 位；`pmm` 帧号
   u64；0.1.x 为 i386 freestanding 缺 `__udivdi3` 打的 32 位
   除法补丁随之退役（64 位除法原生可用）
9. **暂不做（记入已知限制）**：内核仍恒等映射低区（非
   high-half），>4GiB 物理内存与 KASLR 留 1.x；NX 位不启用

### 任务分解（✅ 全部完成，2026-09-24：五里程碑 A-E 全绿，18+21 测试网原样通过）
- [x] 阶段 0（半天 spike）：验证 QEMU `-kernel` 对
      ELF64+a.out-kludge 的加载行为，定方案 A/B
      **✅ 已完成（2026-09-22），方案 A 定版**：
      QEMU `-kernel` 可直接引导 ELF64 + a.out-kludge，无需
      GRUB/ISO。实测三变体全部通过（spike 产物：
      `os/spike/{spike64.S, spike64.ld, run_spike.py}`）：
      ① ELF64+kludge ② flat-bin+kludge ③ ELF32 经典
      Multiboot（对照）。关键观测：QEMU 11.1 在 EAX 传
      `0x2BADB003`（非标准文档值 0x2BADB002），boot.S 需
      同时兼容二者；isa-debug-exit（0x501）退出码 103
      验证通过。附带结论：kludge 的 `load_addr` 需按
      文件内段偏移修正（`@DELTA@` 模板方案，见
      `run_spike.py`）
- [x] `boot.S`：长模式蹦床 + `.bss` 静态页表区
      **✅ 已完成（2026-09-22，里程碑 A 点亮）**：kludge 头 +
      32 位入口（QEMU magic `0x2BADB003` 归一化为 spec 值）+
      CPUID 自查 LM → `.bss` 静态 PML4/PDPT/PD（低 1 GiB 恒等
      映射，2 MiB 大页）→ EFER.LME → CR0.PG → 远跳 64 位 →
      `kmain`。实测教训：`rep stosl` 清 `.bss` 会冲掉 %edi，交接
      寄存器改用 %ebp/%esi。QEMU 退出码 1（阶段 A 门禁通过），
      banner/magic/内存探测全绿
- [x] `gdt.c`/`gdt_flush.S`：GDT64 + TSS64（RSP0 语义）
      **✅ 已完成（2026-09-22，里程碑 B）**：5 描述符 + 16 字节
      TSS64 描述符（占 2 槽，`SEL_TSS`=0x28 不变）；TSS 只用
      RSP0（偏移 4），IST 全 0；flush 改 far-return 重载 CS
- [x] `isr.S`：64 位 stub + iretq；`isr.h`：pt_regs 拓宽
      **✅ 已完成（2026-09-22，里程碑 B）**：统一帧 22 qword
      （15 GPR + int_no/err + rip/cs/rflags/rsp/ss）；长模式下
      CPU **总是**压 rsp/ss，i386 的"ring3 才有"判断退役；
      `iretd`→`iretq`；段寄存器不再保存（长模式忽略 base/limit）。
      附带重构：`isr_handler`/`dump_regs`/`exc_name` 从
      `syscall.c` 拆出为独立 `isr.c`（B 范围派发：异常→panic、
      IRQ0→timer、其余→eoi；ring3 杀进程与调度切换尾巴随 D 回归）
- [x] `kernel/paging.rs`（Rust）：四级页表全量重写 + C ABI 导出
      **✅ 已完成（2026-09-23，里程碑 C 点亮）**：`no_std`
      staticlib（`-C relocation-model=static`，规避 PIC/GOT 空跳）；
      PML4/PDPT/PD/PT 四级管理、内核 2MiB 大页共享、用户 4KiB
      按需建表、`invlpg` 冲刷。实测三处关键教训：① rustc 默认
      对该 target 生成 GOT 间接跳转，bare-metal 链接不填 GOT，
      首症状是 `call *GOT` 跳 0（rip=3 #UD）；② 用户栈在
      PML4[255]，建目录不能只预建 PML4[0] 链——`paging_map_user`
      改为各级按需分配；③ 页表漫游**每一级**都要 USER 位，
      只给叶子页设 USER 会让 ring3 在取指时就 #PF
- [x] `build.py`：`-target x86_64-freestanding`、`-mno-red-zone`、
      rustc staticlib 步骤、QEMU 换 `qemu-system-x86_64`
      **✅ 已完成（2026-09-23）**：目标三元组、`-mno-red-zone`、
      `qemu-system-x86_64`、链接两遍法烘 `@DELTA@`
      （`elf_load_delta()` 校验 PT_LOAD 线性性）、源清单按里程碑
      裁剪（C 阶段 25 个 C/asm 目标 + `libpaging_rs.a`）、
      rustc staticlib（`relocation-model=static`）全部就位；
      另含用户程序构建（5 个 ELF64）与 mkramfs initrd 打包
- [x] `elf.c`：ELF64；`user.ld`/`crt0.S`/`ulib.c`：64 位 ABI
      **✅ 已完成（2026-09-23）**：`Elf64_Ehdr/Phdr`（EM_X86_64）、
      用户入口 0x40000000、栈顶 0x7FFF_FFFF_E000；
      user.ld/crt0.S/ulib.c 同步 64 位化，syscall 编号 1-15
      与内核侧一致
- [x] `syscall.c`/`ulib`：寄存器 ABI 换 rax/rdi/rsi/rdx
      **✅ 已完成（2026-09-23）**：int 0x80 保留，`rax=编号 +
      rdi/rsi/rdx 传参`；`syscall_dispatch` 按新 ABI 取参，
      返回值写回 rax
- [x] 里程碑推进：A 长模式点亮（kprintf+klog）→ B 中断+定时+
      early 组测试绿 → C 四级页表 + ELF64 + ring3 hello →
      D mt 组全绿 → E fs 组 + 21 项 shell 检查 + poweroff 收官
      **进度：A ✅ B ✅ C ✅ D ✅ E ✅（2026-09-24，v0.3.0 收官）**。
      C 实测：GATE-C-OK（early 3/3：pmm/heap/paging + ring3
      hello 从 ramfs 加载、syscall 写串口、sleep/yield、
      exit(42)、用户页全量回收，QEMU 退出码 1）。
      C 附带：isr.c 调度尾巴在多任务未启动时丢弃 resched 请求
      （sti 后 PIT 抢跑 vs `task_create_idle` 竞态）；
      ramfs.c 指针算术去 uint32 截断。
      D 实测：GATE-D-OK（mt 10/10 **一次通过**：priority/RR/
      FCFS/EDF 四策略调度序、sleep/wake 时序、aging 反饥饿、
      spawner 从 ring3 构建进程树（孙进程各 exit 42）、
      fault_div #DE→SIGFPE(136)、fault_segv #PF→SIGSEGV(139)、
      kill 睡眠任务 SIGTERM；整套跑完空闲页零泄漏，QEMU
      退出码 1）。mt 组在 idle 任务上下文运行：每个用例
      阻塞在 waitpid，调度器轮转子任务——isr.c 的"无任务时
      丢弃 resched"补丁正覆盖此形态。
      E 实测（2026-09-24）：GATE-E-OK + **21/21 shell 检查 +
      check_run_exit 通过**——18/18 自测原样全绿
      （KTEST SUMMARY: 18/18 passed + NOVIX-BOOT-OK），交互
      shell 跑在 init(pid 1) 任务里（"kill 1"命中 shell 守卫）。
      E 结构改动：① early 组移回 sti 之前（paging 测试来回换
      CR3，被抢占的 tick 恢复的目录会错——原 i386 版本即此
      顺序）；② 门禁 C/D/E 全部移入 init 任务，kmain 只做
      硬件初始化后进 idle 循环；③ isr.c 杀进程消息恢复
      i386 格式（`killed by sig%d` / #PF 带 fault addr 的
      `killed by SIGSEGV`），串口测试网原样匹配；④
      test_shell.py / check_run_exit.py 的 QEMU 路径切
      qemu-system-x86_64（检查项零改动）；⑤ gate E 校验
      PMM 页而非堆相等——ramfs 无 rmdir，fs 套件故意留下
      /tmp 与 /tmp/kdir（几百字节堆驻留是设计内行为）

### 验收标准
1. `qemu-system-x86_64` 引导进 shell；`poweroff` 经 isa-debug-exit
   正常退出（`check_run_exit.py` 原样通过）
2. 自测 18/18 **原样**通过（3 early + 10 mt + 5 fs）——测试零
   新增零修改是移植完成的最硬证据（实现层寄存器名可改，
   断言语义不改）
3. shell 检查 21/21 通过（串口自动化不受架构影响）
4. fault_div/fault_segv 在 ring3 被杀，退出码 128+signo
   （#DE/#PF 语义在长模式不变）
5. 内核含至少 1 个 Rust 模块（paging）与 C 共链成功
6. 空闲页计数与 i386 版同量级（无泄漏）

### 依赖与风险
- 依赖：0.2.0（18+21 测试网是移植验收的根基）；无新增外部
  依赖（qemu-system-x86_64 与 zig 均已在 tools/ 内）
- ~~风险（高）：引导载体未定——QEMU 内置 multiboot 加载器对
  ELF64 的行为不保证~~ **已解除**：阶段 0 spike 实测方案 A
  可行（见任务分解首项）；方案 B（ISO）保留为备胎
- 风险：Rust/C 混链的符号可见性与 LTO——`#[no_mangle]` +
  起步关 LTO 规避；`panic_handler` 先落 klog，分级后续再说
- 风险：SysV ABI 16 字节栈对齐与手工构造中断帧的配合
  **已解除**（2026-09-24）：mt 组 10/10 全绿（含 ring3 进程树
  与信号杀进程），手工帧的 cs/ss/rsp/rflags 语义经四策略
  调度实测无偏差

---

## v0.4.0 —— fork/exec 与 COW：Unix 进程模型的灵魂

### 背景（现状代码依据）
`task.c` 的进程创建只有两条路：`task_create_kernel`（内核线程）和
`task_create_user`（全新 ELF）。pid 由 `next_pid++` 单调递增
（永不复用）；子进程地址空间总是全新分配——没有"复制"这个概念。
而 fork/exec + COW 恰是 Unix 区别于"程序加载器"的分水岭。

### 目标
`fork()` 写时复制复制地址空间；`exec()` 从 ramfs 加载替换镜像；
pid 支持回收与安全复用。

### 技术设计

1. **fork 的三件事**（对照 Linux `copy_process` 的拆分）：
   - `dup_mm()`：新页目录（PML4）；遍历父地址空间用户区
     （0x400000 起，边界由 0.3.0 分区表定死），
     父子 PTE 同时清 `PTE_RW`（只读），并置保留位 `PTE_COW`
     （借用 PTE 位 9，x86 保留位）；物理页本身**不复制**
     - COW 页需要引用计数：`pmm.c` 增加每页 `refcount[]` 数组
       （uint8_t，RAM_TOP_MAX/PAGE_SIZE = 32KiB 项，可承受）
   - `dup_files()`：fd 表浅拷贝（file 结构共享，pos 共享——Unix 语义）
   - `copy_thread()`：把父进程当前 `pt_regs` 深拷贝为子进程首帧，
     子帧 `rax = 0`（fork 返回 0），父帧 rax 返回子 pid
2. **COW 缺页处理**（挂在 0.1.1 的 int 14 路径上）：
   - 写只读页 + PTE 有 COW 位 → `refcount == 1` ？恢复本方 RW 即可：
     分配新页、复制、映射新页、refcount--；缺页即内核态，全程安全
   - 无 COW 位且在 VMA（0.6.0 前用静态区间表）外的写 → 真段错误杀进程
3. **exec**（`do_execve()`，`task.c` 或新 `exec.c`）：
   - 打开 ramfs 路径 → 读 ELF 到临时堆缓冲 → **先校验 ELF 头**
     （防坏文件毁掉唯一地址空间）→ 释放旧用户页 → `elf_load` 新镜像
     → 重置栈与 pt_regs（rip=entry）→ 保留 pid、fd、ppid
   - 失败路径：任一步出错且旧空间已毁 → 杀进程（Unix 语义本就如此）
4. **pid 复用**：
   - `pid_bitmap[TASK_MAX]` + `find_first_zero_bit`；
   - 防止 waitpid 混淆：pid 位图按 slot 对齐（pid = slot 永久绑定，
     复用即 slot 复用；ZOMBIE 未收割的 slot 不释放）——
     简化方案，偏离 Linux 的 pid+generation 但语义安全
5. **用户栈与返回**：fork 后子进程首次被调度即"从 fork 返回"，
   复用 0.1.0 的伪造帧机制，无需新汇编

### 任务分解
- [x] `pmm.c`：页引用计数 `pmm_ref_inc/pmm_ref_get`；`pmm_free` 改 dec 语义
      （共享页减引用不释放，归零才回收；`paging_destroy_user_dir` 走同一语义）
- [x] `paging.rs`：`paging_dup_user_dir()`（页表深拷贝 + 父子 PTE 只读化
      + `P_COW`（位 9）标记）、`paging_cow_fault()`（复制/复用判定）
- [x] `task.c`：`task_fork()` = dup_mm（COW）+ dup_files（f_count 共享）
      + copy_thread（pt_regs 深拷贝，子帧 rax=0）
- [x] `syscall.c`：`SYS_FORK/SYS_EXEC` 接线；COW 判定接入 `isr.c` 缺页路径
      （int 14 → `paging_cow_fault` 成功即重执行，失败才 SIGSEGV）
- [x] `exec`：`task_exec()`（实现在 task.c，未拆 exec.c）——先临时页目录
      加载校验 ELF，成功才替换当前空间；保留 pid/fd/ppid
- [x] `task.c`：pid 复用（pid == slot 永久绑定，ZOMBIE 未收割不释放 slot）
- [x] `os/user/spawner.c`：改写为真 fork+exec+waitpid（两轮父子）
- [x] `os/user/forker.c`（原计划 forktest.c）：fork 后子写父读，验证隔离
- [x] `os/user/forkacct.c` + `os/user/execer.c`：COW 账目/exec 替换证明程序
- [ ] shell：`spawn` 内部改为 fork+exec——**推迟**：shell 是内核线程，
      内核栈（活跃 C 调用帧）不可深拷贝，`task_fork` 正确拒绝内核任务；
      待 shell 迁移用户态（远期）后落实
- [x] `ktest.c`：测试 19「COW 隔离」20「COW 账目」21「exec 替换」
      22「pid 复用」全绿（22/22）
- [x] `ulib`：`ufork()/uexec()` 声明与实现；`uprintf` 补 `%ld/%lu/%lx`
- [x] 附带修复：linker.ld 把 `.got`/`.data.rel.ro` 纳入 Multiboot kludge
      加载范围（Rust GOT 间接调用跳到未加载地址导致内核态 #PF rip=1）；
      ktest pid_reuse 轮询由 kyield 改 sleep（init@PRIO_MAX 会饿死子进程）

### 验收标准
1. ✅ `forker`：父 g=111 / 子 g=222，写后互不可见（COW 正确触发）
2. ✅ `forkacct`：安静 fork 仅 8 页（纯页表）；子写 4 页恰好多耗 4 页
      （delta=4）；两子收割后空闲页精确复原（零泄漏）
3. ✅ `execer` → `/bin/hello`：pid 不变（2）、镜像替换、exit 42
4. ✅ `pid_reuse`：僵尸占 pid 2 期间新进程取 pid 3；收割后 slot 复用
5. ✅ 自测 22/22 通过（GATE-C/D/E 全绿、页零回退）；`test_shell.py`
      新增 forker/forkacct/execer 冒烟（21→25 项检查全绿）

### 依赖与风险
- 依赖：0.2.0（exec 需从 ramfs 读文件）、0.1.1（COW 缺页路径）、
  0.3.0（四级页表与 64 位帧格式）
- 风险（高）：COW 的父子 PTE 同步——父进程也只读化后，若父先写、
  子后写，两方 refcount 操作必须原子（单核 + irq_save 已够，
  但 0.7.0 SMP 时是重灾区，须写注释标记）
- 风险：fork 在 shell（内核线程）中调用 → 仅用户进程可 fork，
  内核线程调用返回 -1 并打印警告

---

## v0.5.0 —— 信号与用户堆：接近 Unix 的运行时

### 背景（现状代码依据）
用户程序无堆（ELF 静态 BSS 撑死）、无信号（`sys_sleep`/`waitpid`
是仅有的阻塞原语）、进程间零通信。原模块 `signal.c` 只有打印日志
的假信号。

### 目标
`sbrk` 动态堆；最小信号集（SIGKILL/SIGSEGV/SIGTERM/SIGINT）；
管道通信。

### 技术设计

1. **sbrk**：
   - `elf_load` 记录镜像最高地址 → `task->brk` 初始化为页对齐值
   - `SYS_SBRK(18)`：入参增量，返回旧 brk；向下扩 VMA 映射匿名页
     （区间上限：`USER_HEAP_LIMIT = 0x4000_0000`，与栈顶
      `0x7FFF_FFFF_E000` 之间留足空隙，也为 0.6.0 mmap 留地址空间）
   - `ulib` 实现简易 malloc：空闲块链表（first-fit + 合并），
     边界标记法；`free` 写桶测试
2. **信号**（对照 Linux 信号的三个阶段：生成/投递/执行）：
   - `task` 增加 `uint32_t sigpending`（32 信号位图）+
     `sighandler_t handlers[32]`
   - 生成：`SYS_KILL(19)`；异常杀进程（0.1.1）改为投递 SIGSEGV
     等价物，默认动作"终止"保持不变
   - 投递点：`isr_handler` 中 syscall/中断返回**前**检查
     `sigpending & ~blocked`——这是唯一需要碰 pt_regs 的时机
   - 执行：用户栈压入伪造信号帧（`handler(sig)` 返回地址指向
     ulib 的 sigreturn 蹦床，`SYS_SIGRETURN(20)` 恢复原帧）——
     完整复刻 Linux rt_sigreturn 机制的教学版
   - 无 handler 或 SIGKILL → 默认终止
3. **管道**：
   - `struct pipe {ringbuf 4096, read_waiter, write_waiter}`
   - `SYS_PIPE(21)` 返回两个 fd（挂 VFS，`f_op` 为 pipe 读写）
   - 满 → 写者 SLEEP，空 → 读者 SLEEP；对端关闭 → 唤醒并返回 EOF/EPIPE
   - 0.2.0 的 fd 表天然承载，无需改 PCB
4. **waitpid 扩展**：`WNOHANG` 标志位（0 = 阻塞原语义）

### 任务分解
- [x] `elf.c`：记录镜像 end → `task->brk`（页对齐）
- [x] `syscall.c`：`SYS_SBRK(19)` + 匿名页扩展/收缩（增长走
      `pmm_alloc_zero`+`paging_map_user`，回缩 `paging_unmap_user` 释放整页；
      上限 `USER_HEAP_LIMIT = 0x8000_0000`，为 0.6.0 mmap 留上半区）
- [x] `os/user/ulib.c`：malloc/free（first-fit 空闲链 + 前后合并；
      尾部空闲块与 sbrk 增量衔接，反复小分配 O(1) 次系统调用）
- [x] `task.h`：sigpending/sigblocked/sighandlers[32]/sigrestorer/brk 字段
      （信号集实现为完整 1..31，SIGKILL 永不可捕获——比计划的最小集更完整）
- [x] `signal.c`：投递 = 用户栈压伪造帧（蹦床地址 + 保存的 pt_regs），
      改写中断帧跳 handler（rdi=signo）；`SYS_SIGACTION(21)` 注册
      handler+蹦床地址，`SYS_SIGRETURN(22)` 从用户栈恢复原帧——
      投递点两处：`isr.c` 中断返回路径 + `sched.c` 切换目标任务时
      （CR3 已装载才可写用户栈；内核帧 cs&3!=3 跳过）
- [x] `SYS_KILL(20)`（SLEEPING/WAITING 目标唤醒为 RUNNABLE，EINTR 语义）；
      shell `kill PID [SIGNO]`（默认 SIGTERM，带信号合法性检查）
- [x] `fs/pipe.c`：ringbuf 4096（单调读写指针取模）+ 阻塞语义
      （空/满返回 `EAGAIN_RETRY(-2)`，ulib 读写包装自旋重试；
      对端全关 → EOF 0 / EPIPE -1）+ VFS 匿名 inode（无路径，f_op 路由）
- [x] `SYS_PIPE(23)`；`ulib` 暴露 `upipe/uread/uwrite_fd`
- [x] `waitpid` WNOHANG（`task_waitpid(pid, flags)`，0 = 阻塞原语义）
- [x] `os/user/pipetest.c`：fork + 双管道双向通信 + EOF + 收割
      （1KiB 数据父写子校验、"ok" 回执、exit 33）
- [x] `ktest.c`：23「malloc 往返与 free 合并」（malloctest 256x4K +
      1MiB 无新 sbrk）、24「kill 杀死 SLEEPING 进程」、
      25「handler 捕获后恢复执行」（sigtest 双轮投递，exit 77）、
      26「管道 EOF」（pipetest，exit 66）全绿（26/26）
- [x] 附带修复：`pipe_create` 两次 `fd_alloc` 均在 install 前调用导致
      读/写端拿到同一 fd 号互相覆盖（先装读端再分配写端 fd）；
      `sys_sigreturn` 取帧多加 +8（handler 的 ret 已消费蹦床地址，
      保存帧恰在 trampoline 的 rsp 处）——错位帧令 iretq 把 rflags
      当 CS 加载，#GP err=0x244 即用户 rflags 值
- [x] 收尾补全（2026-09-26）：`SYS_SIGPROCMASK(24)`（SIG_BLOCK/
      UNBLOCK/SETMASK，SIGKILL 位强制清零不可屏蔽；解屏蔽的
      pending 信号在该 syscall 返回路径立即投递）；sys_kill 的
      投递核心提取为 `signal_send()` 供 shell Ctrl+C 共用；
      shell 键盘 0x03 → SIGINT 发给最近 spawn 的前台任务
      （`^C` 回显 + 行缓冲丢弃）；用户程序 `masktest`
      （SIGKILL 不可屏蔽 → 屏蔽中 kill 自已存活 5 轮 yield →
      解屏蔽即投递，exit 44）；ktest 第 27 项 `sig_mask`；
      `DESIGN.md` 落笔（地址分区表/信号表/syscall 表/测试账目，
      兑现本节风险条目的承诺）
- [x] 行编辑补全（2026-09-28）：Backspace 接受 0x08(BS) 与
      0x7F(DEL)（串口终端按 Backspace 发送 DEL，此前被静默丢弃）；
      `console_putchar` 补 `\b` 光标回退分支（VGA 侧 `"\b \b"`
      擦除序列此前打印乱码）；空行退格无害
- [x] Tab 补全与命令历史（2026-09-28）：Tab 补全当前词——第一词
      对命令表（唯一匹配补全+空格，多匹配 bash 式列出后重绘
      提示行）、`spawn` 参数经 `vfs_readdir` 枚举 /bin 实时补全
      （无硬编码程序表）；上/下箭头浏览 16 条环形历史（串口
      ESC[A/B 与 PS/2 0x48/0x50 统一翻译为 ESC 序列，键盘层
      新增非阻塞 `keyboard_trychar`），回车入史（跳过连续重复），
      召回行可继续编辑；附带修复 `shell_parse` 尾随空格裁剪
      （Tab 补全附加的空格曾令 `spawn forker ` 解析失败）；
      shell 检查 38 项（含 history recall / tab forker rerun
      两个出现次数检查——子串匹配无法证明补全与召回真实发生）

### 验收标准
1. ✅ 用户程序 malloc 1MiB 并全量写读正确——malloctest：256x4KiB
      全写读 + 释放合并后 1MiB 分配零 sbrk（内核空闲页不动）+ 全量
      校验，exit 55
2. ✅ `kill <pid>` 杀死任意状态进程，资源零泄漏——kill_sleeping
      杀 SLEEPING 的 hello；gate D 页账目全绿（26 项跑完空闲页复原）
3. ✅ 自定义 SIGTERM handler 被调用且程序继续运行至正常退出——
      sigtest 双轮捕获（syscall 返回路径 + 调度器投递路径），
      exit 77；handler 捕获路径不杀进程
4. ✅ 管道：写端关闭后读端得 EOF；父子经管道传 1KiB 数据校验一致
      ——pipetest 双向（1KiB 下行 + "ok" 回执）+ EOF + 收割 exit 33/66
5. ✅ 自测 26/26 通过（GATE-C/D/E 全绿、页零回退）；`test_shell.py`
      新增 malloctest/sigtest/pipetest/kill 坏信号冒烟（25→29 项检查）
6. ✅ 信号可屏蔽：屏蔽中的 SIGTERM 挂 pending 不投递（5 轮 yield
      存活），解屏蔽在该 syscall 返回路径立即投递；SIGKILL 不可
      屏蔽；shell Ctrl+C 发 SIGINT 给前台任务——masktest exit 44
      （ktest 第 27 项 sig_mask）+ shell 检查 32 项（29→32：
      MASKTEST-OK / ^C / sent SIGINT）
7. ✅ `DESIGN.md`：地址空间分区表（代码 0x40000000 / brk 堆 ≤
      0x80000000 / mmap 保留区 / 栈 0x7FFF_FFFF_E000）+ 信号
      编号表 + syscall 总表 1-24 + 版本测试账目

### 依赖与风险
- 依赖：0.2.0（fd 表）、0.4.0（fork——pipetest 需要）
- 风险（高）：信号帧构造——RSP 指向用户栈，必须在用户地址空间
  合法区间内写（复用 0.1.0 copy 机制 + 边界检查）；蹦床地址写死在
  ulib 固定符号，构建须保证不被 ld 脚本重排（`user.ld` 加 KEEP）
- 风险：sbrk 与未来 mmap 的地址空间冲突 → 现在就定死分区表
  （代码 0x400000 / brk 堆 / mmap 区 / 栈 0x7FFF_FFFF_E000——
  64 位分区，0.3.0 移植定基）并写入 DESIGN.md

---

## v0.6.0 —— 按需分页与交换：内存不再"编译期注定"

### 背景（现状代码依据）
`elf_load` 一次性把所有 PT_LOAD 段全部映射；物理内存上限
`RAM_TOP_MAX = 128MiB`，进程稍多即 OOM（`pmm_alloc` 返回 0，
路径上多数直接失败或 panic）。没有 VMA 概念，缺页即错。

### 目标
ELF 惰性映射（缺页才分配）；物理紧张时换出到 ramfs swap 区；
匿名 mmap。

### 技术设计

1. **VMA（进程地址空间描述符）**：
   ```c
   struct vma {
       uint64_t start, end;        /* 页对齐 */
       uint8_t  prot;              /* R/W/X */
       uint8_t  flags;             /* ANON / FILE / COW */
       struct inode *file;         /* FILE 时非空 */
       uint64_t foff;              /* 文件内偏移 */
       struct vma *next;
   };
   ```
   `task->mm = {pgd, brk, vma_list, stat}`——对照 Linux `mm_struct`
2. **按需分页**：
   - `elf_load` 只登记 VMA（FILE 型），不映射页；栈/BSS 建立即映射
   - int 14：查 VMA 命中 → FILE 页从 ramfs 读入再映射；ANON 页
     清零映射；未命中 → SIGSEGV（0.4.0 语义）
3. **页缓存**（简化版 page cache）：
   - ramfs 文件页即缓存本体（ramfs 天然常驻）；换入换出统计挂
     `mm->stat`
4. **swap**：
   - ramfs 预留 `/swapfile`（构建期生成，4MiB）
   - 物理页低于水位（如 free < 16 页）时 kswapd 式内核线程扫描：
     找"可抛弃"页（匿名且非 COW 共享）写 swap 区，PTE 置换出位
     （PTE 位 1 清 0 + 位 0 清 0，物理帧号字段存 swap 槽号）
   - 换入：缺页 + 换出位 → 从 swap 读回
   - 策略：时钟算法近似 LRU（PTE accessed 位 + 扫描指针）
5. **mmap**：`SYS_MMAP(22)` 仅 `MAP_ANONYMOUS`——登记 ANON VMA，
   返回起始地址；`munmap` 释放

### 任务分解
- [x] `include/mm.h` + `mm/vma.c`：VMA 插入/查找/删除（sbrk 增量与相邻
      匿名 VMA 合并；`vma_remove_range` 处理整段删除与跨段截断）
- [x] `elf.c`：改登记式加载（PT_LOAD → FILE VMA 惰性映射，bss 尾单独
      ANON VMA；栈顶页仍急切映射——NULL argv 终止符要写进去）
- [x] `syscall.c`/`isr.c`：int 14 → COW → VMA 命中判定 → FILE 读入/
      ANON 清零/swap 换回三路处理；**内核态缺页同路径解析**（syscall
      在调用者页表上直接解引用用户指针，管道写未触碰的 .rodata 会
      在 ring0 缺页——v0.5.0 时代页全急切映射掩盖了这条路径）
- [x] `mm/swap.c`：swap 槽位图、换出扫描线程（0.1.0 内核线程机制）、
      时钟算法（PTE_A 二次机会）
- [x] 交换后端（计划修订）：不用 ramfs `/swapfile`，改为 boot 期从 PMM
      预留 1024 帧（4 MiB）作槽池——同为物理 RAM 但免一次 4 MiB 连续
      堆分配，`mkramfs.py` 无需改动
- [x] SYS_MMAP(25)/SYS_MUNMAP(26)（计划里写的 22 已被 sigreturn 占用）
      + ulib `ummap/umunmap`
- [x] shell：`mem -v`（file-in/anon/swap-in/cow/swap-out/file-drop +
      槽位占用）
- [x] `ktest.c` 测试 28-30：demand_file_paging（spawn 后 file 缺页
      计数 > 0，加载器确实惰性）/ mmap_anon（256 页零页 + 无压力不
      换出）/ swap_roundtrip（`swap_set_watermark` 旋钮强制水位，
      换出 > 0 且换回 > 0，页与槽位零泄漏由 gate F 把关）
- [x] `os/user/memhog.c`：1 MiB 匿名 mmap 填充→睡眠→校验→munmap，
      exit 88（64 MiB 吃满方案在 -m 128 下无意义，压力改由水位
      旋钮注入；程序刻意零全局零 malloc，ELF 文件页保持干净）

### 验收标准（实测）
1. ✅ spawn 后空闲页几乎不变（demand_file_paging：hello 跑完 exit 42，
   全部代码页走 #PF 文件路径）
2. ✅ 强制水位下 memhog 1 MiB 换出再换回，字节级校验通过（swap_
   roundtrip；"分配超物理内存"由水位旋钮等价模拟，SMP 版本再上真
   实 -m 限制）
3. ✅ `mem -v` 计数与测试预期一致（换出 > 0 且能换回）
4. ✅ 自测 30/30 通过、shell 42 项全绿、gate F 页与槽位零泄漏

### 依赖与风险
- 依赖：0.4.0（COW 已成熟）、0.5.0（SIGSEGV 语义）
- 风险（高）：swap 与 COW 交互——COW 共享页不可直接换出
  （refcount > 1 跳过）；换出页被 fork 引用的引用计数一致性
- 风险：kswapd 与缺页路径的死锁（缺页持有资源时又等 swap 页）——
  缺页路径内自旋等待 + 水位调高规避，SMP 版本再正式解决
- 设计取舍：无 per-VMA 反向映射（Linux rmap）；换出扫描按页表
  遍历，进程多时 O(n)——记录为已知限制

---

## v0.7.0 —— SMP 多核：原空壳 smp.c 的真身

### 背景（现状代码依据）
所有临界区都是 `irq_save/irq_restore`（cli/sti）——单核假设刻在
`novix.h` 注释里；原模块 `src/smp.c`（11KiB）是空壳。

### 目标
QEMU `-smp 2/4` 下真并行；锁模型从"关中断"升级到自旋锁/每 CPU。

### 技术设计

1. **AP 启动**：
   - ACPI/MADT 或 MP Table 解析（QEMU 两者都有，取 MP Table 更简单）
     找 APIC id 列表
   - `boot.S` 增加实模式蹦床（复制到 0x8000）：`INIT-SIPI-SIPI`
     序列 → AP 跳蹦床 → 复用 0.3.0 长模式序列 → 跳 `ap_entry()`（每 AP 独立栈）
   - BSP 用 `xchg` 标记计数等待所有 AP 就绪
2. **每 CPU 基础设施**：
   - `struct cpu`（current 指针、idle 任务、runqueue、lapic id）
   - GS 段基址指向本 CPU 结构（GDT 项 + `wrmsr 0xC0000101`）——
     `current_task()` 从 `gs:0` 读，替代全局变量
3. **锁**：
   - `spinlock`：`xchg` 忙等 + `pause` 指令；持锁期间关中断
     （"关中断自旋锁"，Linux 早期同款，足够教学）
   - 现有 `irq_save` 临界区逐个审计替换：调度器就绪集、PMM 位图、
     堆、task_list、控制台滚屏
4. **每 CPU 就绪队列 + 负载均衡**：
   - 移植 `sched.c` 就绪集到 per-cpu；时钟 tick 只触发本核重调度
   - 周期性（每 100ms）均衡：偷最忙核的队尾任务
5. **IPI**：
   - `TLB shootdown`：改共享页表（COW 恢复可写、munmap）时
     向其他核发 INVALIDATE_TLB 向量，对端 flush——0.6.0 埋的雷
     在这里排掉
   - `RESCHEDULE` 向量：kill/yield 跨核唤醒用
6. **时钟**：LAPIC timer 替代 PIT 作为每核 tick 源（PIT 保留给
   uptime 基准）

### 任务分解（✅ 全部完成，2026-10）
- [x] `smp/mp.c`：MP Table 解析——`_MP_` 浮动指针三段扫描（EBDA /
      基本内存末 KiB / 0xE0000-0xFFFFF）+ PCMP 表 CPU 条目收集；
      无表优雅单核。实测两坑：PCMP 头是 **44 字节**
      （oem_ptr/oem_size/oem_count 三字段易漏，按 38 字节解析则
      条目全部错位）；SeaBIOS 在 HTT 开启时每封装只报一个逻辑
      CPU，QEMU 须 `-smp N,sockets=N` 显式拓扑才会全数上报
- [x] `smp/trampoline.S` + `smp/smp.c`：AP 蹦床（位置无关代码复制
      0x8000，复用内核 PML4）+ INIT-SIPI-SIPI + `ap_entry()` 每核
      独立引导栈；AP 初始化次序钉死：私有 GDT/TSS → 共享 IDT →
      GS 基址 → LAPIC → per-CPU idle → `smp_gate` 门前停泊
      （先 GDT 后 IDT：蹦床迷你 GDT 的 0x08 还是 32 位描述符，
      共享 IDT 一引用即炸；GS 最后：此后一切代码才可安全读
      `this_cpu()`）
- [x] `smp/apic.c`：LAPIC MMIO 重映射（任意核可访问）、per-CPU
      初始化、IPI 发送（RESCHED/TLB_SHOOTDOWN 两向量）、LAPIC
      timer（每核 tick 源与本地抢占；PIT 退居 BSP 校准基准）
- [x] `include/percpu.h` + GS 基址机制：`struct cpu` 全套 + `gs:0`
      自指锚。实测坑：`cpus[i].self` 必须对**全部**槽位初始化——
      只初始化 slot 0 令 AP 的 `this_cpu()` 恒 NULL，首个 AP 静默死机
- [x] `include/spinlock.h` + 临界区审计：xchg+pause，irqsave 变体
      才动中断标志；锁次序成文（sched_lock > fs/pipe > pmm/heap >
      console）；调度器/PMM/堆/task 表/控制台逐个换装；控制台升级
      整消息单次持锁写出（多核输出不再交错）+ panic 直通模式
- [x] `sched.c`：**计划修订**——per-CPU runqueue + 偷闲均衡改为
      单全局就绪队列 + sched_lock（教学规模更简，负载均衡免费：
      空闲核直接拉队头）；need_resched/RR 游标/idle 兜底 per-CPU；
      **任务所有权协议 `on_cpu`**（park/adopt 在同一临界区原子
      成对）关闭睡眠窗口竞态——远端唤醒 + 无所有权时第二核会偷走
      仍在原核栈上执行的任务（kswapd 双核执行、栈踩踏 panic 根因）；
      附带修复 `task_fork` 的 memset 不走 task_fill（子任务
      on_cpu==0 恒不可选中，fork 子进程曾永不被调度）
- [x] TLB shootdown 接入（**计划修订**）：接入点 = swap 换出/抛弃
      两处，而非"paging.rs 所有改表函数"——COW/munmap 由**页目录
      单加载不变量**豁免：on_cpu 所有权保证用户页目录至多同时加载
      在一个核，任务切换必重载 CR3 全量冲刷，本地 invlpg 即够；
      唯 kswapd 改写**非当前运行任务**的 PTE 需广播 IPI（对端按
      CR3 匹配 invlpg）
- [x] `ktest.c`：31 smp_topology（发现数=在线数、每核 idle 就绪、
      %gs 自洽）、32 spinlock_contention（4 线程 4x25000 精确
      计数）、33 cross_cpu_run（6 线程 400 次 yield 跨核联合 +
      每次 %gs 读 pid 自检）、34 tlb_shootdown_ipi（强制水位
      换出 + IPI 广播计数取证）+ gate G（断言随在线核数自适应，
      单核亦全绿）
- [x] 压测（修订为 smp=2 全套件双核长跑）：34 用例 + 交互 shell
      45 检查全程双核无 panic，kswapd 两核间健康迁移、swap 换出
      516 页（smp=4 未跑——SeaBIOS 拓扑报数受 sockets=N 限制，
      教学收益边际递减，留 1.x）

### 验收标准（实测）
1. ✅ `-smp 2` 下 `ps` 可见两核各自任务（CPU 列跨核分布：kswapd
   落 CPU1、idle 双列）；`cpus` 命令双行 online（shell 检查新增
   3 项）
2. ✅ 并行计数无丢失——spinlock_contention 100k（4x25000）精确
   断言（计划的 1M 修订为 100k：教学节奏下单用例时长可控）
3. ✅ COW/fork 风暴跨核无 panic（34/34 全套件含 forker/forkacct/
   spawner 双核执行）；TLB 陈旧页访问由 tlb_shootdown_ipi 取证
   （换出发生 ⇒ IPI 广播计数 > 0）
4. ✅ `-smp 1` 全功能回归（34/34，smp 套件单核自适应）
5. ✅ 自测 34/34 通过（30 → 34，新增 4 例 + gate G）

### 依赖与风险（事后清账）
- 依赖 0.6.0 ✅：swap 换出路径正是首个 shootdown 接入点
- 死锁风险 ✅：锁次序成文（spinlock.h 头注 + DESIGN.md §5）：
  sched_lock 最外层、console_lock 最内层叶子锁；持锁禁区 =
  分配/阻塞/反向拿锁；34+45 全套件双核实测无死锁
- COW refcount 原子化 ✅（0.4.0 埋点回收）：pmm 引用计数全部在
  pmm_lock（irqsave 自旋锁）内增减；forkacct 账目双核下精确
  （delta=4）
- CONFIG_SMP 回退开关：**未做**——全量换装后 `-smp 1` 即等价单核
  路径，34/34 回归即回退验证

---

## v0.8.0 —— 磁盘与驱动框架：数据活过断电

### 背景（现状代码依据）
ramfs 重启即空；驱动（console/keyboard/serial）各自为政无统一接口；
时间只有单调 tick。

### 目标
ATA 磁盘 + FAT32 读写 + 统一驱动模型 + RTC 墙钟。

### 技术设计

1. **ATA PIO**（`drivers/ata.c`）：
   - 0x1F0-0x1F7 端口组；IDENTIFY 探测、LBA28 读写扇区
   - 先同步轮询（教学清晰），中断模式留 1.x
   - 页级缓冲：读写均过 512B 扇区对齐层
2. **FAT32**（`fs/fat32.c`）：
   - BPB 解析、FAT 簇链遍历、目录项（含 LFN 长文件名只读支持）
   - 挂到 VFS：`f_op` 四件套（read/write/lookup/create）
   - 写路径：簇分配、FAT 更新、脏目录回写
3. **统一驱动模型**（`drivers/driver.c`）：
   ```c
   struct device { char name[16]; dev_t dev; struct file_operations *f_op; };
   struct device_driver { const char *name; int (*probe)(void); struct device *(*init)(void); };
   ```
   - 注册表数组 + init 时遍历 probe（无热插拔，静态总线）
   - 设备统一出现在 `/dev/`：console、hd0、null、zero
4. **rootfs 切换**：内核参数 `root=/dev/hd0` 时挂 FAT32 为根，
   ramfs 退化为 initrd（构建期生成含启动必需文件的 FAT 镜像，
   QEMU `-hda` 挂载）
5. **RTC**（`drivers/rtc.c`）：CMOS 0x70/0x71，BCD→bin，
   `date` 命令 + uptime 起点

### 任务分解（✅ 全部完成，2026-10）
- [x] `drivers/ata.c`：IDENTIFY + 读写扇区 + 分区表（MBR）解析
      （浮总线嗅探判在场、写后 FLUSH CACHE、`ata_lock` 整命令串行）
- [x] `fs/fat32.c`：挂载/读/写/创建/删除（LFN 读支持）
      ——**超出计划**：LFN 写也做了（小写名生成 8.3 别名+LFN 项，
      否则 readdir 吐大写别名）；mkdir/空目录 unlink、append 尺寸
      回写、per-instance 锁与 bounce 缓冲、mount/unmount 实例拆除
- [x] `drivers/driver.c` + `/dev` 设备表
      （probe→init→注册表→`ramfs_mknod` 发布；console/null/zero/hd0）
- [x] `kmain`：rootfs 切换逻辑；`build.py` 生成 FAT 镜像接线
      （`root=/dev/hd0` → FAT 根 + ramfs 钉 /dev /tmp；挂载表在
      `namei.c`：lookup 遮蔽、`..` 爬出、readdir 合并挂载点、
      `vfs_unmount`；无卷诚实回退 ramfs）
- [x] `drivers/rtc.c` + `date` 命令
      （UIP 等待+双读秒值稳定性、BCD 按 DM 位、年=2000+yy）
- [x] `ktest.c`：32「FAT 读写往返」、33「断电语义」（卸载重挂后数据在）
      ——外加 31 dev_registry、34 rtc_wallclock，共 +4 例（34→38），
      gate H；往返测试附**空闲簇账目**断言（一文件一簇、删光归还）
- [x] `tools/mkfatimg.py`：构建期制作 FAT32 镜像
      （**计划修订**：不放 busybox 风格工具集，放 /bin 全部用户
      程序 + novix.txt 标记文件；MBR + LBA 2048 分区 + LFN 项）

### 验收标准
1. ✅ 写文件 → QEMU 重启（镜像持久）→ 文件仍在且内容一致
   ——以更严格的 `fat_powercycle` 成立：同进程内起**独立第二
   挂载实例**读同一张盘（RAM 状态骗不过重挂），shell 测试亦有
   FAT 根上 write→read→rm 往返
2. ✅ `/bin` 可来自 FAT32；ramfs 仅存 /tmp 与 /dev（且 FILE 页
   缺页直接从 FAT32 inode 读入，exec/spawn 全跑在磁盘上）
3. ✅ `date` 与宿主机一致（QEMU `-rtc base=localtime`，CMOS 即
   本地时间）；设备统一走 `/dev` 打开（hd0 为 `<blk>` 裸块设备）
4. ✅ 自测 38/38 通过（计划写 33/33，实际交付 38 例）

### 依赖与风险
- 依赖：0.2.0 VFS、0.6.0 页缓存（FAT 文件页复用缺页路径）
- 风险：FAT32 一致性（掉电半写）——v0.8 只保证顺序写不撕裂，
  fsck 类修复留 1.x
- 风险：QEMU 默认 IDE 与真机差异（CHS/LBA 模式）——LBA28 独占，
  真机测试时注意老主板

---

## v0.9.0 —— 用户与登录：从"开机即 root"到"你是谁"

### 背景（现状代码依据）
当前 `struct task` 没有 uid/gid 概念——所有进程无差别全权；
shell 开机即得，任何人拿到串口就是管理员。真实 OS 的分水岭之一
就是"先证明你是谁"。而登录流程恰好把前面所有子系统串起来用：
文件系统（/etc/passwd）、fork/exec（getty→login→sh 链）、
信号（Ctrl-C/Ctrl-D）、权限位（登录后的身份贯穿）。

### 目标
开机 → getty 提示 → 用户名+密码校验 → 以该 uid 运行 shell；
uid 贯穿文件权限检查；多用户账户可持久化。

### 登录流程设计（核心交付物）

```
kmain
 └─ init (pid 1, uid 0)
     └─ 读 /etc/inittab（简化版，每行一个 tty 条目）
         └─ fork+exec /bin/getty tty1
             ├─ 打印 /etc/issue（欢迎横幅）
             ├─ 提示 "login: " 读用户名
             ├─ fork+exec /bin/login <username>
             │    ├─ 查 /etc/passwd
             │    │    └─ 无此用户/密码错 → 统一报 "Login incorrect"
             │    │       （不泄露是哪一步失败——对齐 Unix 惯例）
             │    ├─ 提示 "Password: "（终端关回显，读密码）
             │    ├─ 读 /etc/shadow → salt+hash 与输入比对
             │    │    ├─ 失败：计数++；满 3 次 sleep 3s 再试（防爆破）
             │    │    └─ 成功 ↓
             │    ├─ setuid(passwd.uid)   ← 权限降落点（login 是 root）
             │    ├─ chdir(passwd.home)
             │    └─ exec(passwd.shell, "-")   ← "-" 标记登录 shell
             └─ shell 退出 → getty 收尸 → init 重新 spawn getty（循环）
```

**数据文件格式**（教学版，字段对齐 Unix 但裁剪）：
```
/etc/passwd   root:x:0:0:root:/root:/bin/sh
              alice:x:100:100:Alice:/home/alice:/bin/sh
              （name:x:uid:gid:comment:home:shell；x=密码在 shadow）
/etc/shadow   root:s8Kx2mQa:<hex-digest>
              （name:8字符salt:哈希；仅 root 可读）
/etc/inittab  tty1:getty
/etc/issue    Novix 0.9.0 -- authorized users only\n
```

### 技术设计

1. **uid/gid 进 PCB**：`task->uid/gid`（uint16_t），fork 继承、
   spawn 默认继承调用者；`task_create_user` 加 uid 参数
2. **系统调用**：`SYS_GETUID/SETUID/GETGID`；
   `setuid` 仅 uid==0 可调（否则 -EPERM）；`SYS_CHDIR`
3. **密码回显关闭**：`SYS_IOCTL` 最小实现——console fd 的
   `TCGETS/TCSETS` 风格命令（echo on/off 位），login 读密码前 off
4. **哈希**：salted SHA-256 精简实现（内核 + ulib 各一份，
   构建期工具 `tools/mkpasswd.py` 同算法）——**教学取舍**：
   无慢因子（真系统 bcrypt/PBKDF2），注释与 DESIGN.md 标注
5. **salt 来源**：构建期随机生成（mkfatimg 打包时），运行期
   `useradd/passwd` 命令用 RTC+ticks 混合熵
6. **权限检查点**（VFS open/write/unlink）：
   - uid 0 全通过（root 特权）
   - 否则按 inode 的 uid/gid/rwx 位比对——0.2.0 的 mode 字段
     此时真正启用
7. **登录后体验**：`id` 命令显示 uid/gid/用户名；`ps` 增加 UID 列；
   `who` 列出当前登录会话（/var/run/utmp 最简版）
8. **root 初始密码**：构建期生成并打印在 README（教学系统约定）；
   `passwd` 命令可改

### 任务分解（✅ 全部完成，2026-10）
- [x] `task.h/c`：uid/gid/ruid 字段 + 继承规则 + spawn 传参
- [x] `syscall.c`：GETUID/SETUID/GETGID/SETGID/GETRUID/CHDIR/
      GETCWD/IOCTL(echo)/MKDIR/CHOWN/POWEROFF 共 +11；
      SYS_SPAWN/SYS_EXEC 扩参 argv/argc（ELF 加载器压栈，
      `main(argc, argv)` 真正可用）
- [x] `fs/vfs.c`：open/unlink/mkdir 权限检查接入
      （`vfs_permitted`：root 旁路 + owner/group/world 三选一，
      POSIX 语义；新文件属主=创建者）
- [x] `lib/sha256.c` + ulib 同款 + `tools/mkpasswd.py`：哈希原语
      三份同算法（内核 shadow_roundtrip、用户态 login/passwd、
      构建期镜像烤制）
- [x] `os/user/getty.c`：issue 打印、用户名读取、exec login
      （同 pid 换程序的链式设计，logout 即整链归零）
- [x] `os/user/login.c`：passwd/shadow 解析、密码校验（统一报
      "Login incorrect"）、setuid 降落链、utmp 写入
- [x] `os/user/sh.c` 用户态 shell + `id.c`、`whoami.c`、`who.c`、
      `passwd.c`（系统唯一 SUID 二进制）、`useradd.c`（root 专用）、
      `cat.c`、`touch.c`、`uidtest.c`
- [x] `tools/mkpasswd.py`：构建期生成 root/alice 随机密码
      （记录 build/etc-passwd.txt）+ ktest 固定密码做测试锚点
- [x] `tools/mkfatimg.py`：/etc 四件套（passwd/shadow/inittab/
      issue）+ /root + /home/alice + /home/ktest + /var/run
      （utmp 落点）；shadow 另烤 shadow-only `kck` 锚点行——
      passwd 改密永不触碰它，shadow_roundtrip 在被改过密码的
      镜像上依然逐字节可校验（session 3 gate E 的保绿基础）
- [x] `kmain.c`：init 改为 getty_loop（读 /etc/inittab、收尸
      respawn——SysV 规则）；内核 shell 保留为 `console=debug`
      调试后门；boot 期 `fs_perm_fixup`（/bin +x、passwd SUID、
      shadow 0600、/home chown——FAT 不存 Unix 位）
- [x] `ktest.c`：39「setuid_boundary」（uidtest 程序边界漫步：
      argv 传递+root 降级+凡人回不去）、40「shadow_roundtrip」
      （构建期哈希==内核哈希）、41「perm_boundary」（alice 三向：
      /bin 拒、shadow 拒、home 可写）——**超出计划**：argv 漫步
      并进 39 一例证明
- [x] `test_shell.py`：三会话——session 1 `console=debug` 内核
      shell 全量回归（50 项）；session 2 登录冒烟（13 项：错密码
      拒绝、ktest 登录、id/whoami/who、shadow 拒读、passwd 改密
      回环、logout→getty respawn→再登录→poweroff）；session 3
      断电存续（3 项：新密码活过关机 + 恢复 ktest-pass 保幂等）

### 验收标准（实测）
1. ✅ 冷启动：getty → 用户名+正确密码 → sh，`id` 显示 uid
   （自动测试以 ktest 证明全链：uid=200(ktest) gid=200(ktest)；
   root/alice 同链路，root 密码记录在 build/etc-passwd.txt）
2. ✅ alice（uid 100）三向：写 /bin 拒、读 /etc/shadow 拒、
   写 /home/alice 成——ktest perm_boundary 直接断言（此前实装
   期发现家目录错归 root，测试抓出后修 fs_perm_fixup）
3. ✅ 错误密码：每次 "Login incorrect"（错误用户名同报，不泄露
   账户存在性）；3 次失败 usleep(3000) 刹车（login.c attempt
   循环；自动测试覆盖单次错误路径）
4. ✅ `passwd` 改密 → 重启（FAT32 持久）→ 新密码可登录且旧密码
   即死——test_shell session 3 专项（session 2 已先验证改密回环）
5. ✅ shell 退出后 getty 重新出现（init 收尸 respawn；banner
   出现次数 = 2 的计数断言）
6. ✅ 自测 41/41 通过（计划写 36/36，实际交付 41 例 + gate I）

### 依赖与风险
- 依赖：0.2.0（文件系统+mode 位）、0.4.0（fork/exec）、
  0.8.0（/etc 持久化）；信号（0.5.0）用于 Ctrl-C/Ctrl-D 路径
- 风险：权限检查散落 VFS 各入口，漏检点用"audit 清单 + 测试 36
  全覆盖"兜底；DESIGN.md 记录检查点全集
- 风险：无慢因子哈希——离线爆破 /etc/shadow 理论可行，标注为
  教学限制，1.x 换 PBKDF2
- 设计取舍：单 tty（COM1）；VGA 多终端 + 切换（Alt+F1/F2）留 1.x

---

## v1.0.0 —— 里程碑：README 里那句 "a real OS" 的完整兑现

### 目标
脱离 QEMU 依赖可引导；有 shell、用户、init、文档的完整操作系统；
第一次开机完成的是**注册**而不是登录——凭据由第一次开机的人
自己创建，而不是报出一个镜像里预置的身份。

### 任务分解
- [x] **引导**：`build.py iso` 用 `grub-mkrescue` 生成 `novix.iso`
      （GRUB multiboot1 菜单，`grub.cfg` 入库；宿主缺 grub-pc-bin/
      xorriso 时如实报因跳过——Windows 工具链不含，QEMU `-kernel`
      仍是受测路径）。真机验证清单（USB 启动、硬件白名单）留 1.x
- [x] **osh 脚本化**（`user/osh.c`，替换 v0.9.0 的 sh）：
      管道 `|`（fork+exec+dup2 接线）、重定向 `>`/`>>`/`<`、
      后台 `&` + `jobs`/`wait`、变量 `NAME=value`/`$VAR`/`$?`/
      `$pid`、注释 `#`——新增 38 号 DUP2 支撑。边界：无引号/转义、
      变量不导出、无 Ctrl-C 路由（CHANGELOG 诚实边界）
- [x] **开箱注册**（替代原"三账户预置进镜像"方案，需求原话：
      "用户第一次进入此系统应该完成用户注册，而不是上来就是
      登录"）：
      - 出厂零账户镜像（mkfatimg.py）：/etc/passwd 空、/home 空、
        shadow 只留 shadow-only 的 kck 锚点（gate I shadow_roundtrip
        的哈希契约仍可验，而它不是账户、不触发注册）；构建期
        密码生成与 etc-passwd.txt 账本取消
      - `/bin/register` 注册向导（`user/register.c`）：第一步设
        root 密码（无管理员则 useradd 永远无人能跑）；第二步创建
        首个普通用户（uid=etc_max_uid()+1、下限 100；空用户名
        跳过 = 仅 root 系统合法）。两次输入关回显、三轮失败刹车；
        写入 shadow 先行（中断重跑只留可替换的 shadow 行）；
        root 条目查重后再 append；/etc/motd 登录后展示
      - getty 空账户分支（`user/getty.c` + etc.c `etc_user_count`）：
        /etc/passwd 有效行为 0 → 同 pid exec /bin/register；注册
        退出回 init respawn，下一轮循环自然给出登录提示
- [x] **init**：内核 pid 1 监督用户态 `/bin/init`，读 `/etc/init`
      脚本逐行 exec（一次性行 + 首个 respawn 行监督到底）
- [x] **测试**：ktest perm_boundary 自建 /home/alice fixture
      （vfs_mkdir + chown 100，uid 套件与镜像内容解耦）；
      test_shell.py 会话 2 首启注册（root 密码 + ktest/ktest-pass）
      → 登录链全量冒烟，会话 3 证明注册断电存续且向导不重跑；
      脚本启动时自行重建零账户镜像（从 build/user 重打包，秒级）
- [x] **文档**：`docs/DESIGN.en.md`（英文版）、
      `docs/driver-writer-guide.md`（驱动编写指南）、
      `docs/syscall.md`（38 项编号表 + tools/check_syscalls.py
      随构建比对头文件——验收 #4 的校验机制）
- [ ] **发布**：tag v1.0.0、novix.iso 与 novix-hd.img 产物入 CI
      （本地验证已全绿；等维护者打 tag / 接 CI runner）

### 验收标准
1. 全新镜像开机：不出现 login: 提示，出现注册向导横幅
   "no user accounts exist yet"；完成 root 密码 + 用户 ktest 注册后
   自动回到登录提示，注册凭据成功登录，id 报 uid=100(ktest)
2. poweroff 后再开机：注册的账户仍在（FAT32 存续）、注册向导
   不再出现
3. osh 中：`cat /etc/motd | grep Novix > /tmp/x` 全链路正确
4. 普通用户写 /bin 下文件被拒（权限位生效，源自 0.9.0）
5. 文档四件套齐备且与代码一致（CI 校验系统调用编号表）
6. 裸机（或另一台虚拟化平台）从 ISO 引导 → GRUB → init 脚本 →
   getty → 注册/登录链在真机上复现（真机验证清单留 1.x）

## v1.1.0 —— 交互工具箱：常用的文件与系统操作全部进用户态 shell

v1.0.0 留下的明显缺口：osh 的 help 宣称 "ls-class tools" 而
/bin 里没有 ls；删除、复制、移动、日期、清屏都要切内核 debug
shell。这一版把"用户与 OS 的交互"补齐——常用操作在 osh 内
直接可用，内核 shell 回归纯粹的急救路径。

- [x] **GETDENTS（42 号）**：目录枚举报告——内核 shell 的 ls
      提炼为共享报告函数 `shell_report_dir`（emit 回调模式，与
      39-41 同源逐字一致）；syscall 边界检查目录 R（Unix：列
      目录须可读），内核 shell root 路径不受检
- [x] **UNLINK（43 号）**：暴露 VFS 既有删除（拒绝目录、父目录
      W 在 VFS 内查）——/bin/rm
- [x] **DATETIME（44 号）**：RTC 墙钟行共享 `shell_report_date`
      （不 sane 返 −1）——/bin/date
- [x] **ioctl TCCLEAR=2**：清屏回位（无 flags 指针，VGA console
      非 ANSI 驱动，转义序列不进用户态）——/bin/clear
- [x] **新程序九件**（initrd/FAT 双打包，osh 直接可跑）：
      ls（默认 cwd）、pwd、rm（多文件）、cp（读写循环）、
      mv（cp+unlink，无 rename(2) 如实注明）、date、sleep
      （严格解析 0-60 秒）、uname、clear
- [x] **osh history 内建**：32 条环形记录（展开前原始行），
      编号列出；↑/↓ 检索留 1.x 行规程
- [x] **osh help 修正**：完整列出内建、脚本特性与 /bin 工具
- [x] **测试**：ktest gate J 增 user_toolbox（45→46，/bin 列表
      + /tmp unlink 往返 + RTC sane）；test_shell.py 会话 2
      增 9 项（84→93：ls 标签、pwd 家目录、prompt 跟随 cwd、
      touch/ls、cp+mv 内容往返、rm 失败文案、date local 行、
      uname、history）；docs/syscall.md 增 42-44（CI 门禁绿）
- [ ] **发布**：tag v1.1.0（本地三门禁已全绿，等维护者打 tag）

### 验收标准
1. osh 中 `ls /bin` 列出全部程序（与内核 debug shell 的 ls 逐字
   一致，含 "N entries" 尾行）；`ls` 无参默认列 cwd
2. touch → cp → mv → cat 内容往返全链正确；rm 删除后 ls 不再
   列出；rm 不存在的文件给出干净错误而非静默
3. date 输出 RTC 墙钟行（"…local (unix …)"）；clear 后 shell
   继续工作；uname 报 1.1.0
4. history 列出本会话命令（编号）；help 不再宣称不存在的工具
5. 普通用户列无 R 权限的目录被拒（42 号边界检查生效）
6. 三门禁全绿：build.py test（smp=2 与 smp=1）+ test_shell.py

---

## 1.1.x → 2.0.0 版本规划（2026-10-25 制定）

主线一句话：**1.1.x 小步补齐交互与安全债务 → 1.2-1.9 每版一个子系统纵深 →
2.0.0 只重构不改行为的架构分层**。全程延续开发原则：每版三门禁全绿才收，
每版可引导可交互。

### 主线总览

| 版本 | 主题 | 主要交付 |
|---|---|---|
| v1.1.1 | 交互检索 | ↑/↓ 历史翻阅、行编辑增强 |
| v1.1.2 | 文件操作补全 | rename(2)/rmdir(2)、mv 直连 |
| v1.1.3 | 安全加固 | PBKDF2、shadow 访问审计 |
| v1.1.4 | 内存防护 | 栈 guard page |
| v1.1.5 | 环境变量 | environ、export、$PATH |
| v1.1.6 | 文本工具 | wc/head/tail/which/find |
| v1.1.7 | 身份完整化 | euid/egid 拆分、setreuid |
| v1.1.8 | procfs | /proc 只读回归 |
| v1.1.9 | 债务清理 | 遗留标记回收、压力门 |
| v1.2.0 | 线程与作业控制 | clone、pthread、进程组、Ctrl-C |
| v1.3.0 | 网络入门 | 网卡驱动 + 最小 UDP |
| v1.4.0 | TCP 与 socket | TCP 子集、socket VFS 化 |
| v1.5.0 | 存储深化 | page cache、sync/fsync |
| v1.6.0 | 多用户完整化 | /etc/group、chgrp、umask |
| v1.7.0 | 调度观测 | PI 演示、调度统计、nice |
| v1.8.0 | 稳定性与长跑 | 1 小时门、fork bomb、泄漏审计 |
| v1.9.0 | arch 盘点 | 接口清单文档化、RISC-V spike 附录 |
| v2.0.0 | 架构分层重构 | arch/x86_64、行为逐字等价 |

### v1.1.x 系列（九个小步版本）

每版聚焦一个改进，纯增量，三门禁全绿即收（预计各自新增自测 1-2 项 +
test_shell 若干项）。

- **v1.1.1 —— 交互检索**：↑/↓ 翻阅 history（osh 已留行规程，32 条
  环形记录已就位）+ Home/End/Ctrl-U/Ctrl-W 行编辑。转义序列解析放在
  内核 console 输入层一处（串口/VGA 共用），osh 与内核 shell 同享。
  验收：↑↑ 翻出前两条、编辑不破坏历史记录原文。
  ✅ 完成（2026-10-26）：输入层 KEY_* 逻辑键码归一、ulib 编辑器
  ureadline_ex（←/→/Home/End/Ctrl-U/Ctrl-W + 历史回调）、osh 真
  环形历史修复、附带修复堆起点写死 0x400000 被 .bss 增长顶穿的
  启动期 #DF（heap_base 动态化）。shell 105/105。
- **v1.1.2 —— 文件操作补全**：RENAME(45)/RMDIR(46) 进 VFS 与
  syscall 表；mv 从"cp+unlink 临时实现"换直连（如实注解随之删除）；
  rmdir 工具。验收：mv 后 inode 不变（stat 证）、rmdir 拒绝非空。
  ✅ 完成（2026-10-26）：VFS 层 EXDEV 拦截 + ramfs/FAT32 双实现；
  FAT unlink 收紧拒目录（rmdir 唯一路径）；mv=rename 优先+跨实例
  回退；附带修复 mkdir 目录归 root 的权限 bug、补 /bin/mkdir。
  ktest 47/47、shell 109/109。
- **v1.1.3 —— 安全加固**：PBKDF2 替换裸 salted SHA-256
  （DESIGN.md §6.5 的 1.x 承诺到期回收）+ /etc/shadow 访问路径审计
  （login SUID 只读、register 只写）；存量哈希首启自动升级。
  验收：shadow 字段格式带迭代数、旧盘镜像登录后哈希已换新。
  ✅ 完成（2026-10-26）：PBKDF2-HMAC-SHA256 三份同构造（内核/用户/
  mkpasswd），行内 iters 字段；etc_shadow_upgrade 以 login 同款钩子
  升级旧格式（shadowtest 程序验证整链）；附带修复 login 降权返回值
  未检查、passwd target 名字校验缺失。ktest 48/48、shell 109/109。
- **v1.1.4 —— 内存防护**：用户栈 guard page（剖析报告遗留缺口：
  栈越界写应落保护页而非裸 #PF）；ktest 增栈溢出 SIGSEGV 边界用例。
  验收：递归炸栈程序死于 SIGSEGV 而非 panic。
  ✅ 完成（2026-10-26）：栈 2→8 页；guard 落为结构不变量（栈下页
  永不映射 + mmap 分配器硬上界 USER_MMAP_LIMIT）；stackovf 载体 +
  ktest stack_overflow + shell 检查。49/49、110/110。
- **v1.1.5 —— 环境变量**：exec 环境数组传递（environ）、osh
  `export` 内建、$PATH 命令查找（PATH 缺省 /bin）、`env` 工具。
  验收：export 后子进程可见；改 PATH 后外部命令可临时失明。
  ✅ 完成（2026-10-26）：EXEC 向量 ABI 统一 NULL 结尾 + envp 入栈 +
  crt0 environ；osh export/未导出局部语义/PATH 查找与失明；/bin/env。
  49/49、113/113。
- **v1.1.6 —— 文本工具六件**：wc/head/tail/which/find/cut——
  纯用户态零内核改动（吃现成 getdents/stat/read），测试面只增
  test_shell 项。验收：`ls /bin | wc -l` 管道链闭合。
- **v1.1.7 —— 身份完整化**：euid/egid 与 ruid/rgid 正式拆分
  （README 差距表回收）、setreuid(2)/setregid(2)、access(2)。
  验收：SUID login 的 getruid!=getuid 语义进入 ktest uid 组。
- **v1.1.8 —— procfs**：/proc 只读挂载（task/mm/统计）——
  **原内核模块 /proc/procman 的回归**（"假的变真的"叙事闭环：
  当年是 procfs 假装进程，如今是真内核提供真 procfs）。
  验收：cat /proc/uptime 与 10 号 syscall 一致。
- **v1.1.9 —— 债务清理**：全库 `/* SMP: */` `/* v0.x: */` 遗留
  标记盘点回收（能收的收、收不了的转正为 DESIGN.md 已知限制）；
  fork bomb 存活门 + 10 分钟长跑压力进 build.py test 可选目标。
  验收：标记计数归零或全部转为文档化限制；压力门绿。

### v1.2.0 —— 线程与作业控制（minor 功能版）

- **clone(2)**：轻量执行流（共享 mm/文件表，CLONE_VM 起步），
  pt_regs 与调度器改动最小化——on_cpu 协议对线程同样生效
- **最小 pthread**：create/join/mutex/exit 四件（ulib 提供）；
  thread_hello 双线程在 cross_cpu 证据下真并行
- **进程组**：setpgid(2)/getpgrp(2)，会话概念留 1.x 后
- **Ctrl-C 信号路由**：console 输入层 → 前台组 SIGINT（osh 作业
  前台语义）——补齐 v1.1.0 时"无 Ctrl-C 路由"的如实边界
- 验收：thread_hello 跨核并行证据；Ctrl-C 杀前台作业、后台作业
  存活；fork+thread 混合后页计数守恒（gate F 扩展）

### v1.3.0 —— 网络栈入门（延伸方向第一条兑现）

- QEMU e1000（或 virtio-net）驱动：MMIO + DMA 环缓冲，
  统一驱动模型注册为 netif
- **最小 UDP**：socket(2)/bind/sendto/recvfrom/close 六调用；
  校验_and 直通（不做分片）
- 工具：`ping`（ICMP echo）+ `nc`（UDP 回环演示）
- 验收：QEMU user-net 下 ping 通网关；nc 环回收发一致；
  中断路径不破坏既有锁序（NAPI 风格下半部留 v1.4）

### v1.4.0 —— TCP 与 socket 完整化

- TCP 状态机子集：LISTEN/SYN-SENT/ESTABLISHED/CLOSE，
  三次握手与优雅关闭（参照 lwIP 设计，不抄代码）
- **socket VFS 化**：socket fd 统一进文件表，dup2/管道语义
  对 socket 生效
- telnetd 玩具服务：登录世界第一次可以远程进
- 验收：外部 `telnet` 连入并完成一次登录会话；
  socket 经 dup2 进 osh 管道

### v1.5.0 —— 存储深化

- **page cache**：读路径缓存 + 写回策略（脏页阈值/定时），
  FAT 读放大下降可测
- **sync(2)/fsync(2)**：掉电一致性从 fat_powercycle 门扩展到
  随机写模式
- FAT32 簇分配器优化（连续分配提示）；swap 预读
- 验收：io 压力门（iotest 扩展）+ 掉电门双绿；
  写盘字节数可统计（对比无 cache 基线）

### v1.6.0 —— 多用户完整化

- **/etc/group**：组数据库、chgrp(2)、组权限位（rwx 三元组
  第三段生效）
- **umask**：open/mkdir 默认权限掩码
- 新用户默认属组；useradd -g 选项
- 验收：跨用户目录访问矩阵进 test_shell（uid A 建组，
  uid B 验证组 rwx）

### v1.7.0 —— 调度观测与公平

- 优先级继承（PI）最小演示：mutex 持有者临时提升——
  在现有锁序契约内做，不动 sched_lock 语义
- nice(2)：PRIORITY 策略下用户态权重
- /proc 调度统计（每任务切换次数/等待时间），
  `top` 工具
- 验收：低优先级任务在 nice 梯度下可见饥饿差异；
  PI 用例翻转抢占顺序有断言

### v1.8.0 —— 稳定性与长跑（v2.0 前的信任基线）

- **1 小时长跑门**：脚本化 osh 会话循环（fork/pipe/fs 混合
  载荷），页表/堆/swap 计数漂移为零
- fork bomb 存活从 v1.1.9 可选转正式门
- 错误路径清扫：每个 syscall 的边界用例至少一项进 ktest
- 验收：长跑前后 meminfo 逐项 diff 为零；
  三门禁 + 长跑门四绿——**重构前的行为锚点定版**

### v1.9.0 —— arch 层盘点（只文档不动码）

- **ArchOps 接口清单**：early_boot/context_switch/tlb_flush/
  send_ipi/set_timer 五件 + per-CPU 机制，DESIGN.md 新章
- x86 绑定点全量标注（boot.S/APIC/paging.rs/MP 表/isa-debug-exit
  与架构无关代码的边界线）
- **RISC-V spike 附录**：延续 spike/ 目录传统——QEMU virt +
  OpenSBI 起最小核的一次性验证（S 态入口 + UART 打印），
  结论只进文档不进主线
- 验收：接口清单覆盖全部 arch 依赖 grep 零漏；
  spike 附录可复现

### v2.0.0 —— 架构分层重构（只重构不改行为）

- kernel/ 平铺 → `arch/x86_64/`（boot/GDT/IDT/APIC/MP/paging/
  percpu/trampoline）+ 架构无关子系统；ArchOps 落地为编译期
  分派（教学内核不上函数指针表——保持可读）
- **行为逐字等价**为最高验收：重构前后 test_shell.py 三会话
  输出 diff 为零、46 项自测计数与断言文本不变
- 门禁矩阵：x86_64 三门禁 × 重构前后双跑对照
- RISC-V 端口**不在本版**——v2.0.0 只立抽象层；
  端口留给 v2.1.0（届时三门禁变双架构六门禁）
- 风险与对策：纯搬家版本绝不夹带功能（违反则拆版）；
  重构分五步提交（每子系统一步），每步门禁全绿

---

## 延伸方向（1.x 之后，不承诺）

（注：下列条目中 **TCP/IP 栈已入 v1.3.0/v1.4.0**、**线程已入 v1.2.0**、
**procfs 已入 v1.1.8** 规划；其余仍为未承诺方向。）

- 五级页表 LA57（0.3.0 落地的是四级教学版）
- TCP/IP 栈（参照 lwIP 设计：网卡驱动 → netif → tcp/udp → socket VFS）
- 线程（`clone` + 共享 mm；为 1.x 的真并行应用铺路）
- procfs（`/proc` 暴露 task/mm/统计——原模块 `/proc/procman` 的回归）
- 抢占式内核与 RCU、内核模块加载器（`.ko` 风格，回馈起点）
- 中英 man 手册

## 开发原则

1. **诚实**：子系统"真做完"才进版本；做不完移入下版并在
   CHANGELOG 写明，绝不假装完成（0.1.0 移除假子系统的传统）
2. **测试先行**：无断言不合并；自测项只增不减
   （0.1.0=10 → 0.9.0 ≥ 36 → 1.0.0 再增）；`test_shell.py` 用例同步增长
3. **参照 Linux 设计，不抄 Linux 代码**：MIT 许可证约束；
   对应关系（copy_process/dup_mm/sigreturn/rmap）在 DESIGN.md 记档
4. **每版可引导可交互**：任何 tag 上 `python build.py run` 都能起来
5. **埋雷必标**：跨版本的已知限制（如 COW 原子性、锁次序）在代码
   注释中用 `/* SMP: */`、`/* v0.5: */` 标记，后续版本回收
