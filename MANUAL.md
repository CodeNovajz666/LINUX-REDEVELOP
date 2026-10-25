# Novix 测试手册

本文汇总 Novix 的**全部测试方法**：怎么跑、测什么、怎么看结果、失败怎么排查。
面向维护者与 CI。

- 测试入口集中在 `os/build.py` 与 `os/tools/`
- **所有命令都在 `os/` 目录下执行**（`build.py` 与 `tools/` 都在那里）
- 跨版本契约见根目录 [DESIGN.md](DESIGN.md)，版本计划见 [ROADMAP.md](ROADMAP.md)，
  逐版变更见 [CHANGELOG.md](CHANGELOG.md)

---

## 1. 前置条件

| 依赖 | 说明 |
|------|------|
| Python 3 | 跑 `build.py` 与 `tools/*.py` |
| QEMU | `qemu-system-x86_64`；Windows 下自动发现 `tools/qemu-pkg/` 内的捆绑版 |
| zig | 交叉编译 C（`tools/zig-x86_64-windows-0.16.0/`，Windows 捆绑） |
| Rust | 需 `x86_64-unknown-none` target（编译 `kernel/paging.rs`） |
| grub-mkrescue + xorriso | **仅** `build.py iso` 需要；缺失时该目标如实报错跳过 |

QEMU 查找顺序（`tools/test_shell.py`）：环境变量 `QEMU` → `PATH` → `tools/qemu-pkg/`。

---

## 2. 快速开始：全量测试（收版三门禁）

三条全绿是收版的硬门槛。**串行执行**——它们共用 `build/` 目录，并行会互相踩踏：

```powershell
cd os

# 门禁 1：内核自测（ktest），2 CPU
python build.py test -smp 2

# 门禁 2：内核自测单核回归
python build.py test -smp 1

# 门禁 3：串口自动化 shell 测试（三会话）
python tools/test_shell.py
```

一键串跑（PowerShell）：

```powershell
python build.py test -smp 2; python build.py test -smp 1; python tools/test_shell.py
```

`Makefile` 等价封装（在 `os/` 下）：

| 命令 | 等价于 |
|------|--------|
| `make test` | `python build.py test -smp 2` |
| `make test1` | `python build.py test -smp 1` |
| `make shell` | `python tools/test_shell.py` |
| `make run` | `python build.py run` |

**当前基线（v1.1.5）**：门禁 1 = 49/49，门禁 2 = 49/49，门禁 3 = 113/113。

---

## 3. 测试体系（三层）

| 层 | 载体 | 规模 | 覆盖 |
|----|------|------|------|
| 一、内核自测 | `kernel/ktest.c`，由 `kmain` 驱动 | 49 项 | 内核机制：内存、调度、COW、信号、管道、VMA/swap、SMP、磁盘、身份、osh 管道、调试报告、用户工具箱 |
| 二、串口自动化 | `tools/test_shell.py` 驱动 QEMU `-serial stdio` | 113 项 | 端到端：内核 shell 全量回归、首启注册、登录世界、脚本化 shell、osh 调试命令、交互工具箱（v1.1.0）、交互检索与行编辑（v1.1.1）、断电存续 |
| 三、辅助校验 | `tools/` 下若干脚本 | 随用随跑 | syscall 表一致性、ELF/GOT 检查、镜像 dump 等 |

三层互补：第一层证明**内核机制**正确（可观测行为，非打印语句），
第二层证明**用户可见的完整链路**正确（真的敲键、真的登录、真的断电重启），
第三层是构建期与排障期的静态校验。

---

## 4. 第一层：内核自测（ktest）

### 4.1 命令与结果判读

```powershell
python build.py test          # 默认 -smp 2
python build.py test -smp 2
python build.py test -smp 1   # 单核回归
```

- 构建内核后，QEMU 无头运行，串口日志写入 `build/serial.log` 并回显到终端
- 超时 180s；超时视为失败
- 通过判据：串口出现 `GATES-OK` 与 `KTEST SUMMARY: 46/46 passed, 0 failed`

**退出码语义**

| 层面 | 值 | 含义 |
|------|----|------|
| `build.py test` 进程 | `0` | 全绿 |
| `build.py test` 进程 | `1` | 失败 |
| QEMU 内部（isa-debug-exit） | `1` | gate 全过（kernel 写 0） |
| QEMU 内部 | `5` | 引导 panic（kernel 写 2） |
| QEMU 内部 | `99` | 180s 超时（脚本置入） |

### 4.2 执行顺序与 gate

内核按固定次序跑各套件，每组以 gate 字母收尾。gate 是**组级断言**：
套件全过 + 资源账目（页/swap 槽）零泄漏才打印绿色。

| gate | 套件 | 断言要点 |
|------|------|----------|
| （early，无字母） | early（3 项） | 在**开中断前**运行：paging 测试会来回切 CR3，抢占 tick 会恢复错目录 |
| C | ring3 hello | `/bin/hello` 以 ring3 运行、退出码 42、用户任务页**全回收**（此项不计入 44，是 kmain 里的独立检查） |
| D | multitasking（19 项） | 套件全过 + 页账目零泄漏 |
| E | filesystem（5 项） | 套件全过 + 页账目零泄漏（fs 套件不 spawn 用户进程，故用 PMM 页为硬通货） |
| F | vm（3 项） | 页**与** swap 槽零泄漏（水位强制拉高后校验再复原） |
| G | smp（4 项） | AP 已上线后再跑；断言随在线核数自适应，故 `-smp 1` 亦全绿；**不做** PMM 快照（kswapd/AP 会合法移动页） |
| H | disk（4 项） | FAT32 挂载为根 + 驱动模型 + RTC；AP 活跃，FAT 锁跨核竞争是证明的一部分 |
| I | uid（3 项） | 身份与权限边界；不做 PMM 快照（inode 缓存永久占用是设计使然） |
| J | osh（3 项） | dup2 系统调用端到端 + 脚本工具链随镜像发行 + 调试报告内核侧断言 |

`-smp 1` 与 `-smp 2` 双跑的意义：SMP 套件断言随核数自适应，单核跑用于**回归**，
确保多核协议（`on_cpu` 所有权、TLB shootdown IPI）的修复没有在单核路径引入退化。

### 4.3 49 项明细

**early（3）** — 开中断前

| 项 | 验证内容 |
|----|----------|
| `pmm` | 物理页分配/释放、页对齐、页账目精确增减 |
| `heap` | 内核堆分配、写入回读、释放与合并 |
| `paging` | 四级页表创建；内核低区 2 MiB 大页共享进用户目录 |

**multitasking（19）** — gate D

| 项 | 验证内容 |
|----|----------|
| `priority_order` | PRIORITY 策略下的调度**顺序** |
| `round_robin` | RR 轮转顺序 |
| `fcfs_order` | FCFS 先来先服务顺序 |
| `edf_order` | EDF 最早截止优先顺序 |
| `sleep_wake` | sleep 到期唤醒 |
| `aging_anti_starvation` | 老化机制防饿死 |
| `syscall_userproc` | 用户进程经 int 0x80 运行 |
| `fault_div_isolation` | 除零只杀进程（128+8），不 panic |
| `fault_segv_isolation` | 段错误只杀进程（128+11） |
| `kill_other_task` | 跨任务 kill |
| `cow_isolation` | fork 后 COW 隔离（父子互不可见） |
| `cow_accounting` | COW 页引用计数账目 |
| `exec_replace` | exec 替换镜像、退出码传递 |
| `pid_reuse` | pid 复用 |
| `malloc_heap` | 用户态 malloc/free 往返 + 合并 |
| `kill_sleeping` | 杀死睡眠中的任务 |
| `sig_catch_resume` | 信号捕获后继续运行 |
| `pipe_eof` | 管道双向数据完整性 + EOF |
| `sig_mask` | 信号屏蔽与 pending |

**filesystem（5）** — gate E

| 项 | 验证内容 |
|----|----------|
| `vfs_create_rw` | 创建/写入/回读 |
| `vfs_lseek` | 文件定位 |
| `vfs_mkdir_readdir` | 目录创建与遍历 |
| `fd_exhaust` | fd 表耗尽边界 |
| `devfs_initrd` | /dev 设备节点 + initrd 打包的 /bin |

**vm（3）** — gate F

| 项 | 验证内容 |
|----|----------|
| `demand_file_paging` | FILE 页按需缺页装入 |
| `mmap_anon` | 匿名 mmap 映射与清零页 |
| `swap_roundtrip` | 强制水位触发的换出/换回往返（含 TLB shootdown IPI 计数断言） |

**smp（4）** — gate G

| 项 | 验证内容 |
|----|----------|
| `smp_topology` | MP 表拓扑发现、在线核数 |
| `spinlock_contention` | 10 万次自旋竞争计数**精确** |
| `cross_cpu_run` | 六线程循环让出，必须共同覆盖两核；每线程经 `%gs` 始终观测到自己的 pid |
| `tlb_shootdown_ipi` | TLB shootdown IPI 生效 |

**disk（4）** — gate H

| 项 | 验证内容 |
|----|----------|
| `dev_registry` | 驱动模型 probe→init→/dev 注册表 |
| `fat_roundtrip` | FAT32 写/读/删往返 |
| `fat_powercycle` | **断电语义**：新实例重挂同一磁盘，证明数据落盘（RAM 状态骗不过重挂） |
| `rtc_wallclock` | CMOS RTC 墙钟读取 |

**uid（3）** — gate I

| 项 | 验证内容 |
|----|----------|
| `setuid_boundary` | SUID exec 与 setuid 边界漫步 |
| `shadow_roundtrip` | 宿主工具与内核 SHA-256 逐字节一致（"kck" 锚点） |
| `perm_boundary` | 三向权限检查（测试自建 `/home/alice`，不依赖镜像预置账户） |

**osh（3）** — gate J

| 项 | 验证内容 |
|----|----------|
| `dup2_osh` | dup2 端到端：pipe+fork 接线、边界语义、文件重定向 |
| `scripting_toolkit` | shell 及其管道伙伴、/etc 脚本文件随镜像发行 |
| `debug_syscalls` | sys_ps/cpus/meminfo 背后的内核报告：表头、每 CPU 一行、内存数值、-v 附加 swap 行 |
| `user_toolbox` | v1.1.0：getdents/unlink/datetime 的内核侧断言——/bin 列表（entries 尾行）、/tmp unlink 往返（列见→删→列不见）、RTC 日期行 sane |

---

## 5. 第二层：串口自动化 shell 测试

### 5.1 命令与前置

```powershell
python build.py             # 先构建：需要 build/kernel.elf 与 build/user/
python tools/test_shell.py
```

脚本自身会：
1. 检查 `build/kernel.elf` 与 `build/user/` 是否存在（缺失则提示先跑 `build.py`）
2. **重建零账户出厂镜像** `build/novix-hd.img`（`mkfatimg.py`，无需编译器，几秒）
   —— 上一轮测试会在盘上留下已注册账户（这正是被测特性），必须重置才能复现首启
3. 依次驱动三个 QEMU 会话，通过 `-serial stdio` 敲键、读回、逐项断言
4. 末尾打印 `shell test: N/N checks passed`

### 5.2 三会话

| 会话 | 启动方式 | 检查数 | 内容 |
|------|----------|--------|------|
| 1 | `root=/dev/hd0 interactive console=debug` | 50 | 直接进内核调试 shell，跑 v0.2.0–v0.8.0 全量命令回归 |
| 2 | 默认启动（零账户镜像） | 32 | getty 发现零账户 → 首启注册向导 → 登录世界 → 脚本化 shell → osh 调试命令 |
| 3 | 全新启动 | 2 | 断电后注册账户存续、注册向导**不再运行**、仅会话 2 改过的密码可用 |

合计 **84** 项。

### 5.3 113 项明细

**会话 1（内核 shell，54 项）**

| 分组 | 检查项 |
|------|--------|
| 基础命令 | `help`、`ps`、`spawn`、`spawner out`、`policy`、`uptime`、`mem`、`echo` |
| 故障隔离 | `fault_div`、`fault_segv` |
| kill 边界 | `kill bad pid`、`kill idle guard`、`kill shell guard`、`kill bad sig` |
| 行编辑（v1.1.1） | `ctrl-u rerun`（清行后重敲命令运行）、`home no-op + date`（Home 预解析、追加式编辑器不受扰）、精确计数 `ctrl-u killed line`、`ctrl-w killed word`（受害文本恰好出现一次=仅键入回显） |
| 文件系统 | `ls /`、`ls /bin`、`redirect >`、`redirect >>`、`touch`、`mkdir`、`rm + missing`、`touch rel`、`rm rel` |
| 用户程序 | `filetest usr`、`forker usr`、`forkacct usr`、`exec usr`、`exec ok`、`malloc usr`、`sig usr`、`pipe usr`、`mask usr`、`memhog usr` |
| 内存视图 | `mem -v` |
| SMP 视图 | `cpus header`、`cpus cpu0`、`cpus cpu1` |
| 磁盘/RTC | `date wallclock`、`ls shows marker`、`cat marker`、`dev block node`、`fat write+read` |
| 行编辑/信号 | `ctrl-c echo`、`ctrl-c kill`、`backspace edit` |
| Tab 补全/历史 | `tab list`、`tab uptime`、`tab forker`、`history recall`、`tab forker rerun` |

**会话 2（首启注册 + 登录世界 + 交互工具箱，54 项）**

| 分组 | 检查项 |
|------|--------|
| 注册向导 | `firstboot banner`、`register root prompt`、`register root set`、`register name prompt`、`register user pw`、`register user created`、`register done`、`register retypes`（重输提示 ×3） |
| 登录链 | `login issue`、`login prompt`、`password prompt`、`bad password`、`login shell prompt`、`getty respawn`（登出后 ×2）、`old password dead`（改密后旧密码失效 ×2） |
| 身份 | `id shows ktest`、`whoami`、`who session`、`shadow denied`（/etc/shadow 权限拒绝） |
| 改密 | `passwd current ask`、`passwd updated` |
| osh 脚本 | `osh exit status`、`osh background job`、`osh comments`、`osh motd pipeline`（管道+重定向 ×4）、`osh var+pipe+redirect`（变量+管道+重定向 ×2） |
| osh 调试 | `osh ps header`（任务表表头）、`osh ps shell row`（osh 任务行）、`osh cpus header`（CPU 表头）、`osh mem lines`（内存统计）、`osh upt`（开机毫秒）、`osh kill no task`（kill 错误路径）、`osh kill bad pid`（pid 0 拒解析）、`osh kill mortal guard`（uid 100 杀不动 kswapd） |
| osh 交互检索（v1.1.1） | `osh up-up recall + history`（↑↑ 召回次新行重跑+两次 history 列表）、`osh home insert`（Home+行首插入）、`osh ctrl-u clear`、精确计数 `osh ctrl-u killed line`、`osh ctrl-w killed word` |
| osh 工具箱 | v1.1.0 九件端到端：`osh ls dir tag`（`<dir>` 标注）、`osh pwd home`（登录家目录）、`osh cd tmp prompt`（prompt 跟随 cwd）、`osh touch ls`、`osh cp+mv roundtrip`（cat 证内容搬运）、`osh rm missing`（失败文案）、`osh date local`（RTC 行）、`osh uname`（版本行）、`osh history`（编号列出） |

**会话 3（断电存续，2 项）**

| 项 | 验证内容 |
|----|----------|
| `new password after reboot` | 断电重启后能用会话 2 改过的新密码登录 |
| `id after reboot` | 注册账户跨断电存续（/etc 在 FAT32 盘上） |

### 5.4 断言写法（扩展测试时）

`test_shell.py` 的 `EXPECT` 是 `(描述, 期望子串[, 次数])` 三元组列表，
逐会话收集，最后统计通过数。新增检查只需在对应会话的列表里加一行。

---

## 6. 第三层：辅助校验脚本（`os/tools/`）

| 脚本 | 用途 | 触发时机 |
|------|------|----------|
| `check_syscalls.py` | syscall 表与头文件一致性校验 | `build.py` 构建时**自动调用** |
| `check_run_exit.py` | 校验 QEMU 退出码语义 | 排障时手动 |
| `mkramfs.py` | 把 `/bin` 与零账户 `/etc` 打包进内核（initrd） | 构建时自动 |
| `mkfatimg.py` | 生成 64 MiB FAT32 持久化磁盘（零账户出厂镜像） | 构建时自动；`test_shell.py` 启动时重建 |
| `mkpasswd.py` | 宿主端密码哈希（与内核 SHA-256 逐字节对齐） | 排障/对齐验证 |
| `fatdump.py` | dump FAT 镜像内容（排查磁盘布局） | 排障时手动 |
| `elf_inspect.py` / `phdr_inspect.py` / `elfsym.py` / `symaddr.py` / `hexva.py` | ELF 段/程序头/符号/地址检查 | 排障时手动 |
| `got_inspect.py` / `got_verify.py` | GOT 相关校验（bare-metal 链接踩坑排查） | 排障时手动 |

---

## 7. 构建与运行（非测试但常用）

```powershell
python build.py                 # 构建：内核 + 29 个用户程序 + 64MiB FAT32 镜像
python build.py run             # 构建并交互启动（默认 2 CPU；首启进注册向导）
python build.py run -smp 1      # 单核交互启动
python build.py clean           # 清空 build/
python build.py iso             # 生成 novix.iso（需 grub-mkrescue + xorriso）
```

**产物**

| 路径 | 内容 |
|------|------|
| `build/kernel.elf` | 内核（Multiboot1 + a.out-kludge，QEMU `-kernel` 直启） |
| `build/user/*` | 29 个 ring3 用户程序 |
| `build/novix-hd.img` | 64 MiB FAT32 持久化根（零账户出厂镜像） |
| `build/novix.iso` | GRUB multiboot ISO（`iso` 目标产出） |
| `build/serial.log` | `test` 目标的串口日志 |

**进入内核调试 shell**（`console=debug` 后门，会话 1 走的就是这条路）：

```powershell
..\tools\qemu-pkg\qemu-system-x86_64.exe -kernel build\kernel.elf -m 128M -smp 2 `
  -drive file=build\novix-hd.img,format=raw,if=ide,index=0,media=disk `
  -rtc base=localtime -serial stdio -append "root=/dev/hd0 interactive console=debug"
```

### 7.1 ISO 构建：Windows 宿主走 WSL2（已验证路径）

Windows 没有原生 grub-mkrescue（MSYS2 只有 xorriso、没有 grub 包），
**必须借 Linux 环境**。WSL2 是推荐路径（2026-10 已在本机验证，
`novix.iso` 经 GRUB 引导跑完全部 46 项内核自测）。

一次性安装（管理员 PowerShell 装 WSL，工具在 Ubuntu 内装）：

```powershell
wsl --install -d Ubuntu        # 若已有 Ubuntu-22.04 可跳过
```

```bash
# 工具安装：WSL 内用 -u root 免 sudo 密码（wsl -- sudo 会卡密码提示）
wsl -d Ubuntu-22.04 -u root -- bash -c \
  "apt-get update -qq && apt-get install -y -qq grub-pc-bin grub-common xorriso mtools"
```

生成 ISO（两种等价方式）：

```bash
# 方式 A：项目目标（需 WSL 内有 python3；注意 build.py 会检查 rustc，
# WSL 未装 Rust 时被拦，用方式 B）
wsl -d Ubuntu-22.04 -- bash -c "cd /mnt/d/Projects/LINUX-REDEVELOP/os && python3 build.py iso"

# 方式 B：手动复刻 cmd_iso 动作（无需 python3/rust，kernel.elf 用 Windows 侧产物）
wsl -d Ubuntu-22.04 -- bash -c "cd /mnt/d/Projects/LINUX-REDEVELOP/os && \
  mkdir -p build/iso/boot/grub && \
  cp build/kernel.elf build/iso/boot/kernel.elf && \
  cp grub.cfg build/iso/boot/grub/grub.cfg && \
  grub-mkrescue build/iso -o build/novix.iso -volid NOVIX -quiet"
```

**引导验证**（光盘优先，否则 SeaBIOS 先试无引导代码的硬盘 MBR 卡死）：

```powershell
..\tools\qemu-pkg\qemu-system-x86_64.exe -boot order=d -cdrom build\novix.iso `
  -m 128M -smp 2,sockets=2,cores=1,threads=1 `
  -drive file=build\novix-hd.img,format=raw,if=ide,index=0,media=disk `
  -rtc base=localtime -display none -serial file:build\iso-boot.log
```

两个已知注意点：

- **`-boot order=d` 必须加**：默认启动序硬盘在前，`novix-hd.img` 的 MBR
  无引导代码，SeaBIOS 停在 `Booting from Hard Disk...`（用 monitor
  `screendump` 取证定位）
- **SMP 拓扑要显式**：`-smp 2` 裸参数下 SeaBIOS 的 MP 表只报 1 CPU
  （gate G 自适应通过但只有单核）；`-smp 2,sockets=2,cores=1,threads=1`
  才能双核上线（见根 DESIGN.md 的 SeaBIOS 教训）
- grub.cfg（2026-10-15 起）为**三菜单项**：默认 `Novix 1.1.0 -- run`
  （`root=/dev/hd0 interactive`，进注册/登录世界，登录后是用户态
  shell osh）、第二项 `kernel self-test`（46 项自测后
  `NOVIX-BOOT-OK` 停机）、第三项 `kernel debug shell`
  （`console=debug`，46 项自测后进内核调试 shell `novix>`，可用
  ps/cpus/spawn 等调试命令；osh 也已内建 ps/cpus/mem/upt/kill 的
  只读版本，内核 shell 保留为急救路径）。
  注意 run/debug 模式**每次开机先跑完全部 46 项自测再进 shell**
  （开箱完整性门禁，约多花十几秒），登录提示出现前的大段 [TEST]
  输出不是卡死
- **纯 ISO（无盘）也受支持**：去掉上面命令里的 `-drive` 整段即
  可——root 自动回退 ramfs，initrd 携带 /bin 与零账户 /etc，首启
  注册向导照常工作（disk 套件如实 skipped，仍 46/46）。VMware 等
  不想挂虚拟硬盘的场景用这个路径

---

## 8. 测试设计原则

1. **验证可观测行为，不验证打印语句**：调度*顺序*、跨核执行证据、
   精确的竞争计数、COW 隔离、管道数据完整性、换出/换回往返、
   **断电重挂**（RAM 状态骗不过重挂）。
2. **资源账目零泄漏**：gate C–F 用 PMM 页/swap 槽的相等快照作为硬断言；
   gate G–J 因 FAT inode 缓存、kswapd、AP 的合法活动而改用组内断言。
3. **不依赖镜像预置状态**：`perm_boundary` 自建 `/home/alice`，
   不依赖镜像里预置账户——测试与出厂镜像解耦。
4. **如实报告而非伪装**：`iso` 目标在缺宿主工具时明确报错跳过，
   不伪造成功；引导 panic 与超时各有独立退出码。

---

## 9. 常见问题排查

| 现象 | 排查方向 |
|------|----------|
| `build/kernel.elf or build/user missing` | 先 `python build.py` 再跑 `tools/test_shell.py` |
| `qemu-system-x86_64 not found` | 设 `QEMU` 环境变量，或确认 `tools/qemu-pkg/` 存在 |
| `TIMEOUT: QEMU did not exit within 180s` | 引导卡死或死锁；看 `build/serial.log` 停在哪个 gate |
| 退出码 5 | 引导 panic；串口日志有 `NOVIX-BOOT-FAILED` |
| 某 gate 报 `N pages leaked` | 该套件有页/资源泄漏；定位该 gate 覆盖的套件 |
| 门禁并行时随机失败 | 三门禁共用 `build/`，必须**串行**跑 |
| 会话 2 没进注册向导 | 磁盘上有旧账户；`test_shell.py` 会重建镜像，手动跑时先删 `build/novix-hd.img` |
| 残留 QEMU 后台进程占日志 | 先停掉旧交互式 `build.py run` 会话再重建 |

---

## 10. 版本测试账目

跨版本记录见根目录 [DESIGN.md](DESIGN.md) §4。摘要：

| 版本 | 内核自测 | shell 自动检查 |
|------|----------|----------------|
| v0.5.0 | 27 | 32 |
| v0.6.0 | 30 | 42 |
| v0.7.0 | 34（smp=2/1 双跑） | 45 |
| v0.8.0 | 38（gate H 磁盘套件，双跑） | 50 |
| v0.9.0 | 41（gate I uid 套件，双跑） | 66（50 内核 shell + 13 登录/passwd + 3 断电密码） |
| **v1.0.0** | **44（gate J osh 套件含调试报告，双跑）** | **84（50 内核 shell + 32 首启注册/登录 + 2 断电注册存续）** |
