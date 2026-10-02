# Novix 设计备忘

跨版本共享的"定死不动"的契约：地址空间分区、信号编号、系统调用
ABI、SMP 不变量（§5）。改动其中任何一行都意味着破坏向后兼容——
先读这份文档再动 `novix.h`。版本计划见 [ROADMAP.md](ROADMAP.md)，
逐版变更见 [CHANGELOG.md](CHANGELOG.md)。

## 1. 用户地址空间分区表（x86_64 长模式，48 位规范地址）

v0.3.0 移植定基，v0.5.0 随 sbrk 固化，v0.6.0 mmap 区启用。**写死分区、
杜绝重叠**是 sbrk（0.5.0）、mmap（0.6.0）、栈扩展（0.9.x）三方
不打架的前提。

| 区域 | 起始 | 结束 | 大小 | 归属 |
|------|------|------|------|------|
| 用户代码/数据/静态 BSS | `0x0000_0000_4000_0000` | 随镜像 | — | `USER_CODE_BASE`，`user.ld` 链接基址 |
| brk 堆（sbrk 区） | 镜像 end（页对齐） | `0x0000_0000_8000_0000` | ≤ 1 GiB | `USER_HEAP_LIMIT` 上限；`task->brk` 当前断点 |
| mmap 区 | `0x0000_0000_8000_0000` | 向上生长 | ≤ 1 GiB/次 | 0.6.0 起匿名 mmap 从此向上分配（`mm->mmap_top`） |
| （mmap 区以上至栈底） | mmap_top | `0x0000_7F00_0000_0000` | ~127 TiB | 保留，未使用 |
| 用户栈 | `0x0000_7FFF_FFFF_E000` − 64 KiB | `0x0000_7FFF_FFFF_E000` | 16 页 | `USER_STACK_TOP` 固定栈顶，向下生长 |

要点：

- 内核低区（2 MiB 大页恒等映射）共享进**每一个**用户页目录——
  syscall 在调用者页表上直接执行，用户指针直接解引用，无需跨
  CR3 拷贝（见 `syscall.c` 头注）。
- 用户栈落在 PML4[255]：`paging_map_user` 各级**按需**建表，
  漫游的每一级都要 USER 位，否则 ring3 取指即 #PF（0.3.0 实测
  教训）。
- sbrk 增长从 `task->brk`（elf_load 记录的镜像 end 页对齐）到
  `USER_HEAP_LIMIT`，回缩释放整页；越过上限返回 -1。
- 栈固定 16 页，无 guard page；越界写打在 unmapped 上 →
  #PF → SIGSEGV（fault_segv 即此路径）。
- v0.6.0 起地址空间由 `task->mm` 的 VMA 链描述（`mm.h`）：
  ELF 段=FILE VMA、bss 尾/brk/mmap/栈=ANON VMA，加载只登记不
  映射。#PF 命中 VMA → FILE 页从 inode 读入 / ANON 页清零 /
  换出位（PTE 位 10）从 swap 槽读回；未命中维持 SIGSEGV。
  **内核态缺页同路径解析**——syscall 在调用者页表上直接解引用
  用户指针，未触碰页/ COW 页会在 ring0 缺页（isr.c）。
- 交换（`mm/swap.c`）：boot 期 PMM 预留 1024 帧槽池；kswapd 内核
  线程水位触发，时钟算法（PTE_A 二次机会）。干净 FILE 页直接
  抛弃（inode 即本体）；匿名页与**脏** FILE 页（私有修改，绝不
  回写 ELF 文件）换出到槽；COW 共享页跳过。

## 2. 信号编号表（1..31，Linux 兼容子集）

| 编号 | 名字 | 内核生成源 | 默认动作 |
|------|------|-----------|---------|
| 2 | SIGINT | shell Ctrl+C（`signal_send`，0.5.0） | 终止 |
| 4 | SIGILL | （保留） | 终止 |
| 7 | SIGBUS | （保留） | 终止 |
| 8 | SIGFPE | #DE 除零异常路径 | 终止（128+8） |
| 9 | SIGKILL | `kill PID 9` | 终止；**不可捕获、不可屏蔽** |
| 11 | SIGSEGV | #PF 非法访问路径 | 终止（128+11） |
| 15 | SIGTERM | `kill PID` 默认；shell kill 命令 | 终止 |

规则（`signal.c`）：

- `sigpending`/`sigblocked` 均为 u32 位图，bit (n−1) = 信号 n。
- **生成**（`signal_send`）：目标可捕获（用户任务 + 注册了
  handler + 非 SIGKILL）→ 只挂 pending 位（屏蔽与否在投递时才
  查，POSIX 语义）；否则立即默认终止。
- **投递**（`signal_deliver_pending`）：只在两个点改写 pt_regs——
  `isr.c` 中断/syscall 返回路径、`sched.c` 切换到目标任务时
  （须 CR3 已装载，因为保存帧写到**用户栈**）。内核帧
  （cs&3 != 3）跳过。每帧改写只投递一个信号，余下的等
  sigreturn 恢复后再投。
- **执行**：用户栈压 `[sigrestorer | 保存的 pt_regs]`，帧改写
  rip=handler、rdi=signo；handler `ret` 进蹦床 → `SYS_SIGRETURN`
  从 trampoline 的 rsp（注意：**不加 8**，handler 的 ret 已消费
  返回地址）恢复原帧，并还原被中断 syscall 的 rax。
- `sigprocmask`（0.5.0）：SIG_BLOCK/UNBLOCK/SETMASK 三语义，
  SIGKILL 位强制清零；解屏蔽的 pending 信号在该 syscall 的返回
  路径立即投递。
- fork 继承 handlers/sigrestorer/sigblocked；`sigpending` 不
  继承（POSIX）。

## 3. 系统调用表（int 0x80，rax = 编号，rdi/rsi/rdx 传参）

| # | 名字 | 参数 | 返回 |
|---|------|------|------|
| 1 | exit | status | 不返回 |
| 2 | write | fd, buf, len | 写入字节数 |
| 3 | getpid | — | pid |
| 4 | yield | — | 0 |
| 5 | sleep | ms | 0 |
| 6 | spawn | name | 子 pid（0.5.x 语义：内核直接建任务） |
| 7 | waitpid | pid, flags | 退出状态 / -1 / WNOHANG 0 |
| 8 | getchar | — | 字符 |
| 9 | set_policy | n | 0 |
| 10 | uptime | — | ms |
| 11 | open | path, flags | fd |
| 12 | read | fd, buf, len | 字节数（pipe 空返 −2 由 ulib 重试） |
| 13 | close | fd | 0 |
| 14 | lseek | fd, off, whence | 新偏移 |
| 15 | stat | path, stat* | 0 |
| 16 | fork | — | 父：子 pid；子：0 |
| 17 | exec | path | 不返回（失败 −1） |
| 18 | freepages | — | 内核空闲页（调试） |
| 19 | sbrk | increment | 旧 brk |
| 20 | kill | pid, sig | 0 |
| 21 | sigaction | sig, handler, restorer | 0 |
| 22 | sigreturn | （帧在用户栈） | 被中断处的 rax |
| 23 | pipe | int fds[2]* | 0 |
| 24 | sigprocmask | how, set*, oldset* | 0 |
| 25 | mmap | length | 起始 va（内核选址，匿名清零页） |
| 26 | munmap | va, length | 0（仅接受整段映射） |

约定：错误统一 −1（sign-extend）；pipe 的"请稍后重试"是 −2
（`EAGAIN_RETRY`，仅 pipe 返回，ulib 包装成阻塞语义）。

## 4. 版本测试账目

| 版本 | 内核自测 | shell 自动检查 |
|------|---------|---------------|
| v0.2.0 | 18 | 21 |
| v0.3.0 | 18（零改动） | 21（零改动） |
| v0.4.0 | 22 | 25 |
| v0.5.0 | 27 | 32 |
| v0.6.0 | 30 | 42 |
| v0.7.0 | 34（smp=2 与 smp=1 双跑） | 45（会话跑在 2 CPU） |

`python build.py test`（自测）与 `python tools/test_shell.py`
（串口自动化）双双全绿是收版的硬门槛；页账目零回退由 gate C/D/E
门禁保证，v0.6.0 起页与 swap 槽位零泄漏另由 gate F 把关，v0.7.0
起 SMP 套件由 gate G 把关（断言随在线核数自适应，`-smp 1` 亦全绿）。

## 5. SMP 契约（v0.7.0 定基）

单核时代"关中断即互斥"的全部隐含假设已失效。以下不变量是
0.8.0（磁盘/RTC 中断）及以后所有驱动代码的安全前提。

### 5.1 中断向量布局（`smp.h`）

| 向量 | 用途 | 备注 |
|------|------|------|
| 0x00-0x1F | CPU 异常 | 不变 |
| 0x20-0x2F | 传统 ISA IRQ（IOAPIC 投递，BSP） | 0.8.0 新 IRQ 从此区续排 |
| 0x30 | LAPIC timer | **每核各自**的 tick 与本地抢占 |
| 0x50 | IPI: RESCHED | 唤醒/偷闲提示，接收方只置 need_resched |
| 0x51 | IPI: TLB shootdown | 对端按 CR3 匹配后 invlpg |
| 0x80 | syscall（int 0x80, DPL 3） | 不变 |
| 0xFE/0xFF | LAPIC error / spurious | spurious 不发 EOI |

### 5.2 任务所有权（`task->on_cpu`）

- `-1` = 停泊（可被任意核选中）；`>= 0` = 正在该核上执行
  （**含"已置 SLEEPING/WAITING/ZOMBIE 但尚未切出"的窗口**）。
- 可被选中的充要条件：`state == RUNNABLE && on_cpu == -1`
  （`task_pickable()`，sched.c 所有扫描路径统一走它）。
- 所有者必须在**同一 `sched_lock` 临界区**内完成"停泊 prev
  （存 kesp、降级状态、清 on_cpu）+ 选定并收养 next（置 on_cpu）"。
- 任务收养只有三个入口：`schedule()`、`task_create_idle()`
  （BSP）、`ap_entry()`（AP 收养自己的 idle）——新增入口必须
  手工置 on_cpu。
- **教训**：不经 `task_fill()` 的创建路径（`task_fork` 的
  memset）必须显式 `on_cpu = -1`，否则 memset 残留 0 =
  "被 CPU0 拥有"，任务永不被调度。

### 5.3 锁次序（`spinlock.h` 头注为权威版本）

```
sched_lock  >  fs_lock / pipe locks  >  pmm_lock / heap_lock  >  console_lock
```

- 只许按此序获取、反序释放；pmm/heap/console 是叶子锁，持有
  它们时不得再取任何锁。
- `spin_lock_irqsave` 是共享数据的默认形态（cli/sti 只护本核；
  持锁中断重入同一把锁是 SMP 经典死锁）。裸 `spin_lock` 只用于
  中断已确定关闭的路径。
- 持锁禁区：阻塞/睡眠、kmalloc 可能拿 heap_lock（注意次序）、
  kprintf 会拿 console_lock（叶子上可用，别的锁内慎用）。

### 5.4 页目录单加载不变量（TLB 语义）

- 一个**用户**页目录至多同时加载在一个 CPU 上（on_cpu 所有权
  的推论）；任务切换必重载 CR3 = 非 global 项全量冲刷。
- 因此改写**当前任务** PTE 的路径（COW 恢复 RW、munmap、缺页
  补映射）本地 `invlpg` 即够，无需广播。
- 唯一例外：kswapd 改写**非当前运行任务**的 PTE（swap 换出/
  抛弃）→ `smp_tlb_shootdown()` 广播 IPI，对端按 CR3 匹配后
  invlpg。新增"改他人页表"的路径（0.8.0 页缓存回写要注意）
  必须同样广播。
- 内核低区共享映射（2MiB 大页）对所有核一致且 boot 后不变；
  LAPIC/IOAPIC 的 MMIO 映射同理。

### 5.5 每 CPU 陷阱（`percpu.h`）

- `this_cpu()` 经 `gs:0` 自指锚取 `struct cpu *`；`cpus[i].self`
  必须对**全部**槽位初始化（`percpu_init`），否则 AP 恒 NULL。
- AP 早期初始化次序：私有 GDT/TSS → 共享 IDT → `percpu_set_gs`
  ——在此之前读 `this_cpu()` 的任何代码（含异常路径）都不可靠。
- 调度器上下文就绪前（AP 门前停泊期）不得接收 timer tick/
  IPI 驱动的 schedule()：LAPIC timer 保持屏蔽直到收养 idle。
