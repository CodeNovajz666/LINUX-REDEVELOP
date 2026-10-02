# Novix 更新日志

版本号约定见 ROADMAP.md：次版本号递增 = 交付一个完整子系统；
修订号递增 = 稳定性/小改进。

## v0.7.0 —— SMP 多核（2026-10）

内核从"关中断即安全"的单核假设升级为真多核：AP 真并行执行任务、
自旋锁保护共享结构、TLB shootdown 跨核冲刷。

**新增**
- CPU 拓扑发现（`smp/mp.c`）：MP Table 解析——`_MP_` 浮动指针三段
  扫描（EBDA / 基本内存末 KiB / 0xE0000-0xFFFFF）+ PCMP 表 CPU
  条目收集；无表优雅单核。实测两坑：PCMP 头是 **44 字节**
  （oem_ptr/oem_size/oem_count 三字段易漏，按 38 字节解析则条目
  全部错位）；SeaBIOS 在 HTT 开启时每封装只报一个逻辑 CPU，
  QEMU 须 `-smp N,sockets=N` 显式拓扑才会全数上报
- AP 启动（`smp/trampoline.S` + `smp/smp.c`）：位置无关蹦床复制到
  0x8000（复用内核 PML4）→ INIT-SIPI-SIPI → 长模式 → `ap_entry()`
  每核独立引导栈。AP 初始化次序钉死：**私有 GDT/TSS → 共享 IDT →
  GS 基址 → LAPIC → per-CPU idle → 门前停泊**（先 GDT 后 IDT：
  蹦床迷你 GDT 的 0x08 仍是 32 位描述符，共享 IDT 一引用即炸；
  GS 最后：此后一切代码才可安全读 `this_cpu()`），BSP `smp_release()`
  开门后 AP 才解屏蔽 LAPIC timer 加入调度
- 每 CPU 基础设施（`include/percpu.h` + `percpu.c`）：`struct cpu`
  {apic_id, online, current, idle, need_resched, rr_last, 私有
  GDT/TSS}；GS 段基址（wrmsr 0xC0000101）+ `gs:0` 自指锚。
  实测坑：`cpus[i].self` 必须对**全部**槽位初始化——只初始化
  slot 0 令 AP 的 `this_cpu()` 恒 NULL，首个 AP 静默死机
- LAPIC（`smp/apic.c`）：MMIO 重映射进内核空间（任意核可访问）、
  per-CPU 初始化、IPI 发送、per-CPU LAPIC timer（各核 tick 源与
  本地抢占，PIT 退居 BSP 校准基准）
- 自旋锁（`include/spinlock.h`）：xchg 忙等 + pause；irqsave 变体
  才动中断标志（cli/sti 只护本核，持锁中断重入是 SMP 经典坑）；
  锁次序成文：sched_lock > fs/pipe > pmm/heap > console；
  调度器/PMM/堆/task 表/控制台临界区逐个换装
- SMP 调度（`sched.c`）：**单全局就绪队列 + sched_lock**（计划
  修订：原设计 per-CPU 队列 + 偷闲均衡；教学规模下全局队列更简，
  负载均衡免费——空闲核直接拉队头）；need_resched/RR 游标/idle
  兜底 per-CPU，每核独立 tick 本地抢占
- **任务所有权协议 `task->on_cpu`**：-1=停泊，否则=正在执行它的
  核。可被选中当且仅当 RUNNABLE **且**停泊；所有者必须在同一
  sched_lock 临界区内"停泊 prev + 选定后继"原子完成。关闭睡眠
  窗口竞态：`task_sleep_until()` 置 SLEEPING 释放锁后、`kyield()`
  进 `schedule()` 前，远端 tick/task_wake 可把它翻回 RUNNABLE——
  无所有权时第二个核会"偷"走一个仍在原核栈上执行的任务，
  双核共写 kesp/共享内核栈（kswapd 双核执行、栈踩踏 panic 的
  根因）。附带修复：`task_fork` 的 memset 不走 task_fill，子任务
  on_cpu==0 恒不可选中（fork 子进程曾永不被调度）
- TLB shootdown（`smp.c`）：swap 换出/抛弃改写 PTE 后向所有在线
  核广播 IPI、对端按 CR3 匹配 invlpg（0.6.0"TLB 安全依赖切 CR3"
  的债正式还清）；`smp_tlb_ipis_sent()` 计数供测试取证。COW/
  munmap 不广播——由**页目录单加载不变量**豁免：on_cpu 所有权
  保证用户页目录至多同时加载在一个核，任务切换必重载 CR3 全量
  冲刷，本地 invlpg 即够；唯 kswapd 改写非当前运行任务的 PTE
  才需要广播
- 控制台原子化（`console.c`）：整条消息单次持锁写出（多核并发
  输出不再交错撕行）；panic 直通模式（持锁者死亡时崩溃信息仍可
  打印）
- dump_regs 加固（`isr.c`）：新增 CR3/CR2/LAPIC id/GS base 分诊行；
  `this_cpu()` 越界校验（%gs 损坏时不再递归 panic 淹没现场）
- shell `cpus` 命令（每核 APIC id / online / current）；
  `build.py -smp N`（test/run，默认 2）
- ktest 新增 4 例（30 → 34）+ gate G：smp_topology（发现数=在线数、
  每核 idle 就绪、%gs 自洽）/ spinlock_contention（4 线程 4x25000
  精确计数，丢更新即失败）/ cross_cpu_run（6 线程 400 次 yield，
  跨核联合覆盖 + 每次 %gs 读 pid 自检）/ tlb_shootdown_ipi（强制
  水位换出 + IPI 广播计数取证）
- shell 检查 42 → 45（cpus 表头 + 双核 online），会话本身跑在
  2 CPU 上

**测试**
- 内核自测 34/34（`-smp 2`）+ 34/34（`-smp 1` 单核回归），
  shell 自动化 45/45；gate C-G 全绿（新增 gate G：smp 套件，
  断言随在线核数自适应，单核亦全绿）
- 交互式 shell 全程双核无 panic：kswapd 于两核间健康迁移，
  swap 换出 516 页、TLB IPI 广播正常

## v0.6.0 —— 按需分页与交换（2026-09）

进程的地址空间从"exec 时一次画完"变成一张按需兑现的支票——
物理内存跟着实际触碰的页走，紧张时还能把冷页押进交换区赎回。

**新增**
- VMA 地址空间（`include/mm.h` + `mm/vma.c`）：`task->mm` 挂
  有序 VMA 链（ELF 段=FILE、bss 尾/brk/栈/mmap=ANON）；
  `vma_add` 对相邻同属性匿名段自动合并（sbrk 反复增减不成碎片），
  `vma_remove_range` 支持整段删除/跨段截断
- 按需分页：`elf_load` 改登记式——PT_LOAD 只登记 FILE VMA
  不映射一页（栈顶页除外：NULL argv 终止符要先写进去）；
  int14 缺页三路：FILE 页从 inode 按偏移读入（尾零填充）、
  ANON 页清零映射、换出标记 PTE 从槽位读回；未命中 VMA 维持
  v0.4.0 的 SIGSEGV 语义
- **内核态缺页同路径解析**（isr.c）：syscall 在调用者页表上直接
  解引用用户指针（v0.3.0 的设计），惰性映射后内核 memcpy 一个
  尚未触碰的用户页（管道写 .rodata、fork 后写 COW 栈页）会在
  ring0 缺页——COW/demand 解析对两种特权级统一生效，VMA 未
  覆盖的内核态缺页照旧 panic（首个现场：pipetest）
- 交换（`mm/swap.c`）：boot 期从 PMM 预留 1024 帧（4 MiB）作
  槽池（同 ramfs /swapfile 之实质，免一次连续大分配）；kswapd
  内核线程水位（free < 64 页）触发，每 20ms 扫一轮、每轮至多
  8 页，时钟算法=PTE_A 位二次机会；干净 FILE 页直接抛弃
  （inode 即本体），匿名页与**脏** FILE 页换出——脏文件页是
  私有修改，绝不回写进 ramfs 里的 ELF 文件（否则污染后续
  spawn），按 MAP_PRIVATE 语义进槽；COW 共享页（PTE_COW）
  不可换出；TLB 安全依赖调度器每次切入重载 CR3
- fork 交互：`swap_prefault` 先把父进程已换出的页全部赎回，
  再走 `paging_dup_user_dir`（COW 克隆只认在场 PTE）；exec/退出
  路径 `mm_release` 释放换出槽位+页表+VMA 链，swap 槽零泄漏
- `SYS_MMAP(25)`/`SYS_MUNMAP(26)`：匿名 mmap 从
  `0x8000_0000` 向上选址（`mm->mmap_top`），登记 ANON VMA
  惰性兑现；munmap 只收整段映射（释放槽位/帧/VMA）；
  ulib `ummap/umunmap`
- paging.rs 新原语：`paging_pte_read/pte_write`（裸 PTE 读写，
  后者按需补建中间表）——C 侧据此完成 A/D 位时钟扫描与
  换出标记（P_SWP=位10）读写
- `mem -v`：file-in/anon/swap-in/cow/swap-out/file-drop 六计数
  + 槽位占用；`swap_set_watermark` 测试旋钮（0=默认水位）
- 用户程序 `memhog`（exit 88）：1 MiB 匿名 mmap 填充→睡 600ms
  （给 kswapd 窗口）→逐页校验→munmap；零全局零 malloc，
  自身 ELF 文件页在压力下保持干净
- ktest 新增 3 例（27 → 30）+ gate F（vm 套件页与槽位双零泄漏
  把关）：demand_file_paging / mmap_anon（无压力不换出）/
  swap_roundtrip（强制水位下换出>0 且换回>0）
- shell 检查 38 → 42：`mem -v` 计数行、`spawn memhog` 往返
- DESIGN.md：mmap 区从"保留"转正，syscall 表补 25/26，VMA/交换
  契约入地址空间章节；ROADMAP v0.6.0 勾销（两处计划修订：
  交换后端改 PMM 槽池、mmap 号码 22→25 让位 sigreturn）

**测试**：内核自测 30/30、shell 自动化 42/42、gate C-F 全绿
（含新 gate F：vm 套件物理页与 swap 槽位零泄漏）。

## v0.5.0 —— 信号与用户堆（2026-09）

进程有了动态内存、可捕获的信号和进程间通信管道——
Unix 进程模型三大件补齐。

**新增**
- sbrk 动态堆：`elf_load` 记录镜像末尾 → `task->brk`；
  `SYS_SBRK(19)` 增长清零匿名页/回缩释放整页，上限
  `USER_HEAP_LIMIT = 0x8000_0000`
- ulib `umalloc/ufree`：first-fit 空闲链 + 前后块合并，
  尾部空闲块与 sbrk 增量衔接（反复小分配 O(1) 次系统调用）
- 信号三段式（生成/投递/执行）：`task` 增 `sigpending`/
  `sigblocked`/`sighandlers[32]`/`sigrestorer`；`signal.c` 在
  用户栈压伪造帧（蹦床地址 + 保存的 pt_regs）改写中断帧跳入
  handler（rdi=signo）；投递点两处——`isr.c` 中断返回路径 +
  `sched.c` 切换目标任务时（内核帧 cs&3!=3 跳过）
- `SYS_KILL(20)`（SLEEPING/WAITING 目标唤醒为 RUNNABLE）、
  `SYS_SIGACTION(21)`（handler + 蹦床注册，SIGKILL 拒绝注册）、
  `SYS_SIGRETURN(22)`（从用户栈恢复原帧，Linux rt_sigreturn
  教学版）；无 handler 或 SIGKILL → 默认终止
- 管道 `fs/pipe.c`：匿名 inode（无路径，f_op 路由两端）+
  ringbuf 4096（单调读写指针取模）；空/满返回 `EAGAIN_RETRY(-2)`，
  ulib 读写包装自旋重试 = tick 粒度阻塞语义；对端全关 →
  EOF 0 / EPIPE -1；`SYS_PIPE(23)`
- `waitpid` `WNOHANG` 标志（0 = 阻塞原语义）
- `SYS_SIGPROCMASK(24)`：SIG_BLOCK/UNBLOCK/SETMASK 三语义，
  SIGKILL 位强制清零不可屏蔽；解屏蔽的 pending 信号在该
  syscall 的返回路径立即投递（生成阶段不查屏蔽，投递阶段才
  查——POSIX 语义教学版）
- shell Ctrl+C（键 0x03）→ SIGINT 发给最近 spawn 的前台任务
  （`^C` 回显 + 行缓冲丢弃）；sys_kill 的投递核心提取为
  `signal_send()` 供键盘路径共用
- 用户程序：`malloctest`（256x4K + 1MiB 往返与合并证明）、
  `sigtest`（双轮捕获 SIGTERM 存活，exit 77）、`masktest`
  （SIGKILL 不可屏蔽 → 屏蔽中 kill 自己存活 → 解屏蔽即投递，
  exit 44）、`pipetest`（fork + 双管道 1KiB 双向 + EOF + 收割，
  exit 66）
- ktest 新增 5 例（22 → 27）：malloc_heap / kill_sleeping /
  sig_catch_resume / pipe_eof / sig_mask
- `DESIGN.md` 落笔：地址空间分区表（代码 0x40000000 / brk 堆 ≤
  0x80000000 / mmap 保留区 / 栈 0x7FFF_FFFF_E000）、信号编号表、
  syscall 总表 1-24、版本测试账目——跨版本契约的单一事实源
- 行编辑补全（2026-09-28）：Backspace 同时接受 0x08(BS) 与
  0x7F(DEL)——串口终端按 Backspace 发的是 DEL，此前被可打印
  过滤器静默丢弃导致串口下退格无效；`console_putchar` 补 `\b`
  光标回退分支（VGA 侧擦除序列 `"\b \b"` 此前会打印乱码）；
  空行退格为无害 no-op
- Tab 补全与命令历史（2026-09-28）：Tab 补全当前词（命令表 +
  `spawn` 参数经 `vfs_readdir` 实时枚举 /bin，唯一匹配补全+
  空格，多匹配 bash 式列出并重绘提示行）；上/下箭头浏览 16 条
  环形历史（串口 ESC[A/B 与 PS/2 0x48/0x50 在键盘层统一为
  ESC 序列，`keyboard_getchar` 重构出非阻塞 `keyboard_trychar`）；
  回车入史跳过连续重复，召回行可编辑；附带修复
  `shell_parse` 尾随空格裁剪（补全附加的空格曾令
  `spawn forker ` 解析失败）；shell 检查 38 项（含两个出现
  次数检查：history recall、tab forker rerun）

**变更**
- shell `kill PID [SIGNO]`（默认 SIGTERM，信号合法性检查）；
  help 文本同步
- `task_waitpid` 增 flags 参数；fork 继承 sighandlers/sigrestorer/
  sigblocked/brk（sigpending 不继承，POSIX 语义）

**修复**
- `pipe_create` 两次 `fd_alloc` 均在 install 前调用 → 读/写端
  拿同一 fd 号互相覆盖（改为先装读端再分配写端 fd）
- `sys_sigreturn` 取帧地址多 +8：handler 的 `ret` 已消费蹦床
  地址，保存帧恰在 trampoline 的 rsp 处；错位帧令 iretq 把
  rflags 当 CS 加载（#GP err=0x244 即用户 rflags 值）

**测试**
- 内核自测 22 → 27（3 early + 19 mt + 5 fs），全部通过
  （GATE-C/D/E 全绿、页账目零回退）
- shell 自动化检查 25 → 32（malloctest/sigtest/masktest/
  pipetest/kill 坏信号守卫/Ctrl+C 冒烟），全部通过

## v0.4.0 —— fork/exec 与 COW（2026-09）

Unix 进程模型的灵魂落地：`fork()` 写时复制、`exec()` 镜像替换、
pid 安全复用。

**新增**
- `paging.rs`：`paging_dup_user_dir()`（页表深拷贝 + 父子 PTE 只读化
  + `P_COW` 标记（借用 PTE 位 9））、`paging_cow_fault()`
  （refcount==1 恢复 RW，否则复制新页降引用）
- `pmm.c`：每页引用计数 `pmm_ref_inc/pmm_ref_get`（uint8_t 数组）；
  `pmm_free` 改减引用语义，归零才回收
- `task_fork()` = dup_mm（COW）+ dup_files（`f_count` 共享，
  Unix open-file description 语义）+ copy_thread（pt_regs 深拷贝，
  子帧 rax=0——子进程"从 fork 返回"）
- `task_exec()`：新页目录先加载并校验 ELF，成功才替换当前空间
  （坏二进制不动旧镜像）；保留 pid/ppid/fd
- pid 复用：pid == slot 永久绑定（ZOMBIE 未收割不释放 slot）——
  简化方案，防 waitpid 混淆
- `SYS_FORK(16)`/`SYS_EXEC(17)`；COW 判定接入 `isr.c` 缺页路径
  （int 14 → `paging_cow_fault` 成功即重执行，失败才 SIGSEGV）
- 用户程序：`spawner` 改真 fork+exec+waitpid（两轮父子）、
  `forker`（fork 后子写父读验证 COW 隔离）、`forkacct`
  （安静 fork 8 页 + 子写 4 页恰好多耗 4 页的账目证明）、
  `execer`（exec 替换镜像 pid 不变）
- ktest 新增 4 例（18 → 22）：cow_isolation / cow_accounting /
  exec_replace / pid_reuse
- `ulib`：`ufork()/uexec()`；`uprintf` 补 `%ld/%lu/%lx` 长度修饰符

**修复**
- linker.ld 把 `.got`/`.data.rel.ro` 纳入 Multiboot kludge 加载
  范围（Rust GOT 间接调用跳到未加载地址导致内核态 #PF rip=1）
- ktest pid_reuse 轮询由 kyield 改 sleep（init@PRIO_MAX 会饿死
  子进程）

**推迟**
- shell `spawn` 内部改 fork+exec：shell 是内核线程，活跃 C 调用帧
  的内核栈不可深拷贝（`task_fork` 正确拒绝）；待 shell 迁移
  用户态后落实

**测试**
- 内核自测 18 → 22，全部通过（GATE-C/D/E 全绿、页零回退）
- shell 自动化检查 21 → 25（forker/forkacct/execer 冒烟）

## v0.3.0 —— x86_64 长模式 + Rust（2026-09）

i386 内核整体移植到 x86_64 长模式，并引入首个 Rust 模块——
C+Rust 双语言内核；18+21 测试网**原样零修改**通过。

**新增**
- `boot.S` 长模式蹦床（42 → ~150 行）：Multiboot 交接点仍在
  32 位保护模式 → CPUID 自查 LM → `.bss` 静态 PML4/PDPT/PD
  （内核低区 2MiB 大页恒等映射）→ EFER.LME → CR0.PG →
  远跳 64 位段 → `kmain`
- 引导载体定版（阶段 0 spike 实测）：Multiboot 1 + a.out-kludge
  加载 ELF64，保留 QEMU `-kernel` 直启；magic 兼容
  `0x2BADB002`/`0x2BADB003`（QEMU 11.1 实际传后者）
- `kernel/paging.rs`（首个 Rust 模块）：`no_std` staticlib；
  四级页表（PML4/PDPT/PD/PT）、内核 2MiB 大页、用户 4KiB 页、
  48 位规范地址检查、`invlpg` 冲刷；对 C 只导出 `extern "C"`
  函数（POD 结构手工对齐）
- GDT64 + TSS64（16 字节描述符，权限栈只用 RSP0）；
  `pt_regs` 拓宽为 22 qword 统一帧；`iretd` → `iretq`；
  编译加 `-mno-red-zone`（长模式 CPU 总是压 rsp/ss，
  i386 的"ring3 才有"判断退役）
- ELF64 加载（`Elf64_Ehdr/Phdr`，EM_X86_64）：用户入口
  0x08048000 → 0x40000000、栈顶 → 0x7FFF_FFFF_E000；
  `user.ld`/`crt0.S`/`ulib.c` 同步 64 位化
- syscall ABI：保留 `int 0x80`，寄存器改 rax=编号 +
  rdi/rsi/rdx 传参
- `isr.c` 独立成文件（`isr_handler`/`dump_regs`/`exc_name`
  从 `syscall.c` 拆出）；`build.py` 接入 rustc staticlib
  （`relocation-model=static`）、`-target x86_64-freestanding`、
  QEMU 换 `qemu-system-x86_64`、两遍链接法烘 `@DELTA@`

**变更**
- `novix.h` 地址/长度类型拓宽 64 位；i386 freestanding 缺
  `__udivdi3` 的 32 位除法补丁退役
- 门禁 C/D/E 全部移入 init(pid 1) 任务，kmain 只做硬件初始化；
  early 组移回 sti 之前（paging 测试来回换 CR3，被抢占的 tick
  恢复的目录会错）
- gate E 校验 PMM 页而非堆相等（ramfs 无 rmdir，fs 套件故意
  留下 /tmp 是设计内行为）

**实测教训**
- rustc 默认对该 target 生成 GOT 间接跳转，bare-metal 链接
  不填 GOT，首症状是 `call *GOT` 跳 0（rip=3 #UD）——
  `-C relocation-model=static` 规避
- 用户栈在 PML4[255]，建目录不能只预建 PML4[0] 链——
  `paging_map_user` 改各级按需分配
- 页表漫游**每一级**都要 USER 位，只给叶子页设 USER 会让
  ring3 取指时就 #PF

**测试**
- 自测 18/18 原样通过（零新增零修改——移植完成的最硬证据）；
  shell 检查 21/21 通过；GATE A-E 五里程碑全绿

## v0.2.0 —— ramfs 文件系统（2026-08）

程序不再"焊死"在内核里：VFS + ramfs + devfs + 每进程 fd 表，
`spawn` 改从 `/bin/` 加载。

**新增**
- `fs/vfs.c` + `include/vfs.h`：VFS 核心——inode/file 抽象、
  `inode_operations`（目录树形）/`file_operations`（打开后行为）两类
  操作表分层；open/read/write/lseek/stat/mkdir/unlink/readdir；
  每任务 fd 表（`TASK_MAX_FD=16`，创建时 0/1/2 → `/dev/console`）
- `fs/namei.c`：路径解析（`namei`/`namei_parent`；`.` 跳过、`..` 上溯、
  根的 `..` 停在根）
- `fs/ramfs.c`：堆-backed 文件系统（父/子/兄弟节点树 + 数据缓冲，
  扩容走分配-复制-释放）；`ramfs_mount_initrd()` 启动期解包 NRFS 镜像
- `fs/devfs.c`：`/dev/console`（读写）、`/dev/null`（只写）——
  "设备即文件"首个实例
- 系统调用 OPEN(11)/READ(12)/CLOSE(13)/LSEEK(14)/STAT(15)；
  WRITE(2) 改走 fd 表；SPAWN(6) 改走 `/bin/<name>`（支持绝对路径）
- `tools/mkramfs.py`：构建期将用户 ELF 打包为 NRFS initrd 镜像嵌入内核
  （替代 `embed_user.py`）
- shell 新命令：`ls`（类型/大小）、`cat`、`mkdir`、`touch`、`rm`、
  `echo TEXT > FILE` / `>>` 重定向
- 用户程序 `filetest`：用户态文件 syscall 端到端断言（21 项检查）
- ktest 文件系统组 5 例：vfs_create_rw / vfs_lseek / vfs_mkdir_readdir /
  fd_exhaust / devfs_initrd

**变更**
- `task_exit()` 释放全部 fd（kill 路径由 waitpid 兜底，幂等）；
  被 kill 或无人 wait 的进程不再泄漏文件句柄
- 内核堆 8MiB → 16MiB（`HEAP_END` 0x00800000 → 0x01000000），
  容纳 ramfs 数据
- `elf_find_image()`/镜像表退役，`task_spawn_path()` 从 VFS 全量读入
- zig 全局缓存固定到 `build/zig-cache`（`ZIG_GLOBAL_CACHE_DIR`），
  构建自包含
- 启动创建 `/tmp`（scratch：shell、filetest）

**测试**
- 内核自测 13 → 18（3 early + 10 mt + 5 fs），全部通过
- shell 自动化检查 12 → 21（文件命令、重定向、filetest），全部通过

## v0.1.1 —— 健壮性（2026-08）

一个进程的死亡不再等于整机死亡。

- 用户态异常（除零/页错误等）→ 杀死该进程（退出码 128+signo），
  仅内核态异常才 panic；页错误打印 CR2 故障地址
- `task_kill()`/`task_kill_task()`；`ps` 显示 SIG 退出原因
- `kill PID` shell 命令（守卫：idle 任务与 shell 自身不可杀）
- `klog_err/warn/info` 分级日志（时间戳 + 级别，CI 可 grep）
- 新增 fault_div/fault_segv 故障注入程序与对应测试
- 自测 10 → 13；shell 检查 8 → 12

## v0.1.0 —— 真实内核（2026-08）

从 Linux 内核模块（`src/`，模拟 PCB/调度/内存）演进为可启动的
真实操作系统：
- Multiboot 引导、i386 保护模式、GDT/TSS/IDT/PIC/PIT
- 抢占式多任务（单 `iret` 完成中断返回与上下文切换双用途），
  5 种调度策略（PRIORITY/RR/FCFS/EDF/RMS）+ 老化防饿死
- 物理页位图分配器、内核堆、4MB 大页恒等映射、每进程 4KB 页目录
- ring3 ELF32 加载器、int 0x80 系统调用（10 个）、waitpid/僵尸回收
- 双路控制台输入（PS/2 + 串口，轮询兜底）驱动交互 shell
- 内建自测试套件（10 项，全部带断言）
