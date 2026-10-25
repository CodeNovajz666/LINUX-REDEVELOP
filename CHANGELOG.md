# Novix 更新日志

版本号约定见 ROADMAP.md：次版本号递增 = 交付一个完整子系统；
修订号递增 = 稳定性/小改进。

## v1.1.3 —— 安全加固：PBKDF2、shadow 审计、login 降权检查（2026-10-26）

ROADMAP 1.1.x 第三步：DESIGN.md "无慢因子哈希"的 1.x 承诺到期回收。
密码哈希从裸 salted SHA-256 换成 PBKDF2-HMAC-SHA256（迭代数入行），
旧盘哈希登录时自动升级；同时补上 login 降权检查这个真实世界出过
CVE 的经典漏洞位。

**新增**
- HMAC-SHA256（RFC 2104）+ PBKDF2（RFC 8018，单块 dkLen=32）三份同
  构造：kernel/sha256.c、user/sha256.c（孪生）、tools/mkpasswd.py
  （hashlib 口径）。迭代数 ETC_PBKDF2_ITERS=4096，写进行内
- shadow 行格式 v2：`name:salt:iters:hash`；旧格式 `name:salt:hash`
  仍可验证——etc_verify 双格式，iters 字段有无即判别
- `etc_hash_password`：唯一的"盐+哈希"入口（register/useradd/passwd
  原本三份各自漂移的实现收拢为一份）
- `etc_shadow_upgrade`：login 的升级钩子——legacy 行验证通过后，
  用刚证明过的口令重算 PBKDF2 并改写该行（降权前、以 root 身份）
- /bin/shadowtest：运行期验证载体（ktest uid 组以 root 运行）——
  种 legacy 行 → legacy 验证 → 升级 → 新行仍可验证 → 二次升级为
  no-op → 清场，退出码 45

**修复（安全）**
- **login 的 setgid/setuid 返回值现在被检查**：降权静默失败会给
  用户一个 root shell（真实 login(1) 出过同类 CVE）。失败即拒登
- passwd 的 target 现在过 `etc_valid_name`：root 用含 ':' 的名字
  改密会写出畸形 shadow 行（useradd/register 早有校验，passwd 漏了）
- shadow 访问审计落档（login.c 头注释 + DESIGN.md）：/etc/shadow
  0600/uid 0，触碰者 login（root，读+升级改写）、passwd（唯一 SUID，
  root 读+改写自己行）、register/useradd（root，写）——每个读者
  打开文件时都是 uid 0，凡人路径不可达

**测试**
- ktest 47→48：shadow_roundtrip 改验 PBKDF2 锚点（kernel KDF 与
  mkpasswd 逐字节一致，含 iters 字段解析与"慢因子下限 1000"断言）；
  新增 shadow_upgrade（shadowtest 载体，login 同款代码路径）
- test_shell 109 项不变但全链路换血：注册向导、登录、passwd 改密、
  断电存续全部走 PBKDF2
- 三门禁全绿收版：48/48（smp=2 与 smp=1）、109/109

## v1.1.2 —— 文件操作补全：rename(2)/rmdir(2)，mv 直连（2026-10-26）

ROADMAP 1.1.x 第二步：VFS 补齐两个"真文件系统"该有的操作，mv 从
"cp+unlink 的临时实现"换成 rename 快路径 + 跨实例回退（GNU mv 同款
语义）。

**新增**
- `inode_operations` 增 `rename`/`rmdir` 两个操作 + `vfs_rename`/
  `vfs_rmdir`（两个父目录 W+X；rename 目标存在拒绝而非覆盖——教学
  系统绝不静默毁数据；跨文件系统类型在 VFS 层拦截、同类型跨实例由
  文件系统自身拒绝——EXDEV 语义，ABI 无 errno 返 -1）
- ramfs：rename 原地改挂（inode/打开的 fd/st_ino 全部存活，目录防
  "移进自己"环检查）；rmdir 仅空目录、挂载点视为忙
- FAT32：rename = 删旧 dirent 组 + 目标目录追加新 dirent（同一
  first_clus 与 size——数据不动，缓存节点原地改身份）；跨目录仅
  文件（目录跨目录需改写 ".."，如实 -1）；rmdir 仅空目录。
  **unlink 收紧**：FAT 的 unlink 原本能删空目录（与 ramfs 不一致），
  现在统一拒绝目录——rmdir 是唯一的目录删除路径
- 45 号 RENAME / 46 号 RMDIR 系统调用（+ ulib `urename`/`urmdir`），
  docs/syscall.md 同步（CI 门禁 46/46）
- 新程序两件：`/bin/rmdir`（空目录删除，三种失败路径文案分明）、
  `/bin/mkdir`——v1.1.0 工具箱漏了它，osh 里 mkdir 一直是"命令
  不存在"；mv 重写：rename 优先，跨实例（如 ramfs /tmp ↔ FAT 根）
  回退 copy+unlink，"无 rename(2)"的如实注解随之删除
- osh help 工具清单补 mkdir/rmdir

**修复**
- **mkdir 创建的目录归 root**：node_new 默认 uid 0，凡人建目录后
  自己没有 W 位——touch 进自己刚建的目录失败（rmdir 却能成功，
  因为权限看的是父目录，测试网此前从未覆盖这条路径）。vfs_mkdir
  现在与 vfs_open 创建文件同款规则：新目录属主=创建者
- 跨实例 rename 曾把 FAT 的 newdir 交给 ramfs 的 op 处理
  （node_of 把 fat32_node 当 ramfs_node 用，#GP 实锤）——VFS 层
  现在先比对 i_op（op 表即文件系统类型身份）再分发

**测试**
- ktest 46→47：新 `vfs_rename`（ramfs：inode 存活——旧名打开的 fd
  在改名后继续写读、目标不覆盖、目录原地改名、跨目录移文件、
  rmdir 拒绝非空/删后 stat 消失、跨实例拒绝）；`vfs_mkdir_readdir`
  尾部改用 rmdir 清场（"ramfs 无 rmdir"的历史注释退役）；FAT 往返
  测试增 rename 段（st_ino 前后一致、内容不经复制、拒覆盖）+
  unlink 拒目录/rmdir 拒文件双向断言
- test_shell 105→109：mv 同目录 rename 快路径 + 跨实例回退的内容
  往返、rmdir 空目录删除（精确计数：目录名只出现在回显与 history
  里，绝不在 ls 输出里）、rmdir 非空拒绝文案、mkdir 工具就位
- 三门禁全绿收版：47/47（smp=2 与 smp=1）、109/109

## v1.1.1 —— 交互检索：↑/↓ 历史翻阅与行编辑（2026-10-26）

ROADMAP 1.1.x 系列第一步：把两个 shell 的交互输入补到"日常可用"。
转义序列解析收拢到内核输入层一处，两个 shell 共享同一套逻辑键码。

**新增**
- 内核输入层（keyboard.c）统一转义序列解析：PS/2 方向键/Home/End
  与串口终端的 CSI（ESC [ A/B/C/D/H/F）、SS3（ESC O H/F）序列在
  进入输入队列前归一为 `KEY_UP/DOWN/LEFT/RIGHT/HOME/END`
  （0x80-0x85，高于可打印 ASCII 不冲突）；序列状态机跨 IRQ 存活
  （同一序列的三个字节可分批到达）；孤立的 ESC 无消费者、如实丢弃
- ulib 行编辑器 `ureadline_ex`：←/→/Home/End 光标移动、行中插入与
  退格（退格-重印尾部-擦余量-回退四步重绘，兼容非 ANSI 的 VGA
  console）、Ctrl-U 清至行首、Ctrl-W 删前一个词；历史回调接口
  （↑ 取更旧、↓ 逐级返回、离开当前行时暂存半输入行）；
  `ureadline` 保留为无历史版本
- osh：↑/↓ 历史翻阅接入编辑器；history 修成**真环形**——v1.1.0
  的实现在满 32 条后 slot 恒停最后一条（只覆盖最新），现在
  `nhist % OSH_HIST_MAX` 真轮转、编号保持纪元连续、旧条目退役；
  help 文案新增 editing 行
- 内核 shell：逻辑键码接入（自带 ESC 解析删除）、Ctrl-U/Ctrl-W；
  Home/End 对追加式编辑器如实为无操作
- test_shell 检查框架新增**精确计数**类别（EXACT_EXPECT）：受害
  文本必须恰好出现 N 次（Ctrl-U/Ctrl-W 的键入回显恰好一次，证明
  命令真的没带着受害词运行）

**修复**
- **内核堆起点动态化（启动期双重故障的根因）**：HEAP_START 原本
  写死 0x400000，v1.1.0 内核 `.bss` 结束于 0x3fbba8——只剩 0x4458
  字节余量。本次用户程序增长使 ramfs_img 变大约 380KB，`.bss`
  越过常量顶进堆区，堆分配器把内核自身的静态数据当空闲块发出
  （ramfs 节点落在 boot 栈上、memcpy 参数错乱、initrd 解包期
  #DF；QEMU 异常日志实锤 RIP=memcpy+0x8a、dst 指向内核 .text）。
  heap_init 现在从 `max(HEAP_START, __bss_end 向上 64KiB 对齐)`
  开始，内核再长只是堆上移；ktest 的超限断言与 `mem` 报告改用
  `heap_base()`
- `ureadline` 在 stdin 读错误时无限忙自旋（`!= 1` 即 continue）——
  编辑器改为返回 -1，调用方自行决定；ulib 补 overlap 安全的
  `umemmove`
- 预存编译警告清理：kernel/shell.c 的 strstr 弃 const 限定

**测试**
- ktest 46/46 不变（smp=2 与 smp=1 双绿）；shell 检查 96→105 项：
  会话 1 +4（内核 shell Ctrl-U/Ctrl-W/Home 无操作/精确计数×2）、
  会话 2 +5（osh ↑↑ 召回次新行并重跑、Home+行首插入、Ctrl-U 改跑
  date、精确计数×2；history 检查改为环形语义断言——'1 id' 让位于
  列表行 'echo osh-edit-a'）
- 三门禁全绿收版

## v1.1.0 —— 交互工具箱：ls/rm/date 进 /bin，osh 学会 history（2026-10）

v1.0.0 之后的第一课：把"用户与 OS 的交互"补成完整的工具箱。
此前 osh 的 help 写着 "ls-class tools"，/bin 里却没有 ls；删除、
复制、移动、日期、清屏全要切内核 debug shell。现在常用的
文件与系统操作全部在用户态 shell 内直接可用。

**新增**
- 42 号 GETDENTS（+ ulib `ugetdents`）：目录枚举报告——内核
  shell 的 ls 提炼为共享报告函数 `shell_report_dir`（emit 回调
  模式，与 39-41 同源），/bin/ls 拿到与内核 shell 逐字一致的
  列表；syscall 边界检查目录 R 权限（Unix：列目录须可读），
  内核 shell 走 root 路径不受检
- 43 号 UNLINK（+ `uunlink`）：直接暴露 VFS 既有删除能力
  （拒绝目录、父目录 W 在 VFS 内检查）——/bin/rm
- 44 号 DATETIME（+ `udatetime`）：RTC 墙钟行，内核 shell 的
  date 提炼为 `shell_report_date` 共享（RTC 不 sane 返 −1）——
  /bin/date
- ioctl 33 号扩 TCCLEAR=2 请求（清屏回位，无 flags 指针）——
  /bin/clear；转义序列不进用户态（VGA console 非 ANSI 驱动）
- 新程序九件（进入 initrd/FAT 镜像，osh 直接可跑）：
  `ls`（默认 cwd，4 KiB 截断缓冲）、`pwd`、`rm`（多文件）、
  `cp`（open/read/write 循环）、`mv`（cp+unlink 教学简化，
  无 rename(2)，如实注明）、`date`、`sleep`（严格十进制解析，
  0-60 秒）、`uname`、`clear`
- osh 新内建 `history`：32 条环形记录（原始行，展开前），
  编号列出；↑/↓ 检索属 1.x 行规程范围，如实注明
- osh help 修正：不再谎称 "ls-class tools"——完整列出内建、
  脚本特性与 /bin 工具清单

**测试**
- ktest gate J 增 `user_toolbox`（45→46）：/bin 列表断言（含
  entries 尾行与已知程序名）、/tmp 上 unlink 往返（列见→删→
  列不见）、RTC 日期行 sane——smp=2/1 双跑全绿
- shell 会话 2 增 9 项（93 项）：ls 的 `<dir>` 标签、pwd 打印
  登录家目录、cd 后 prompt 跟随 cwd、touch→ls、cp+mv 内容
  往返（cat 证明）、rm 失败路径文案、date 的 local 行、
  uname 版本、history 编号列出
- 会话 1 时序竞态修复：spawn fault_div/fault_segv 后改
  wait_for(异常报告) 事件驱动——盲 sleep 曾让一次运行在
  spawner 子进程 + 异常报告的输出洪峰期丢输入字节（6 项
  needle 假失败）；重跑 96/96 全绿
- docs/syscall.md 增 42-44 行（CI check_syscalls.py 门禁通过）

## v1.0.0 —— 里程碑：脚本化 shell、用户态 init、ISO 与开箱注册（2026-10）

README 里那句 "a real OS" 的完整兑现：osh（管道/重定向/后台/变量）、
用户态 init 读 /etc/init、/etc/motd、grub ISO 目标、文档四件套；
以及开机体验的最后一课——第一次进入系统完成的是**注册**而不是
登录：出厂镜像零账户，首次开机由 getty 引导进入注册向导，
之后才回到熟悉的登录链。

**新增**
- osh（`user/osh.c`，替代 v0.9.0 的 sh）：基于 0.2-0.9 全部系统
  调用的脚本化 shell——管道 `|`（fork+exec+dup2 接线）、重定向
  `>` `>>` `<`、后台 `&`（jobs/wait 作业表，exit 前 kill+reap 防
  僵尸泄漏）、变量 `NAME=value` 与 `$VAR`/`${VAR}`/`$?`/`$pid`、
  `#` 注释。明确教学边界：无引号/转义、变量不导出给子进程、
  无 Ctrl-C 到前台作业的路由
- DUP2（38 号系统调用 + ulib `udup2`）：新 fd 并入调用者的打开
  文件描述（共享偏移，同 fork 继承），目标 fd 隐式 close——osh 的
  管道/重定向全靠它。ktest 新增 osh 套件（gate J，41→43）：
  dup2_osh（iotest 载体证明管道接线/边界语义/文件重定向）+
  scripting_toolkit（/bin/osh echo grep init 与 /etc/init motd 在镜像）
- osh 调试内建（39-41 号系统调用 PS/CPUS/MEMINFO + ulib
  `ups`/`ucpus`/`umeminfo`）：内核 debug shell 的 ps/cpus/mem 以
  只读报告并入 osh（`ps`、`cpus`、`mem [-v]`、`upt`、`kill
  PID [SIGNO]`）——报告由内核 shell 与系统调用共享同一套流式
  格式化函数（`ksnprintf` + emit 回调），截断式写入；内核 shell
  保留为 console=debug 急救路径。gate J 增至 44（debug_syscalls
  断言报告内容），shell 会话 2 增至 84 项中的 32 项
  （osh 端到端敲 ps/cpus/mem/upt/kill）
- 用户态 init（`user/init.c`）：内核 pid 1 监督 /bin/init；init 读
  /etc/init 逐行执行——一次性行顺序跑，首个 `respawn` 行监督到底
  （SysV respawn 规则，logout 即回登录屏）；无 /etc/init 时回退
  v0.9.0 的 /etc/inittab 契约；换班间隙尽力收养孤儿
- /etc/motd：login 成功后、降权之后打印（像真 login(1)）；
  /etc/init 与 /etc/motd 烤进镜像（mkfatimg）
- 新命令：echo（-n 支持）、grep（子串匹配，stdin/文件/`-`，
  exit 0/1/2）——验收线 `cat /etc/motd | grep Novix > /tmp/x`
  全链路可用
- ISO：`build.py iso` 用 grub-mkrescue 生成 novix.iso（multiboot1
  菜单，grub.cfg 入库）；宿主缺 grub-pc-bin/xorriso 时如实报因
  跳过——QEMU `-kernel` 仍是受测路径
- 文档四件套：`docs/syscall.md`（38 项编号表，tools/check_syscalls.py
  随构建比对头文件——ROADMAP 验收 #4）、`docs/driver-writer-guide.md`、
  `docs/DESIGN.en.md`（英文版设计文档，与中文版并列）
- 出厂镜像零账户（mkfatimg.py）：`/etc/passwd` 出厂为空、
  `/home` 出厂为空，`/etc/shadow` 只保留 shadow-only 的 "kck"
  锚点行（它不是账户，gate I 的 shadow_roundtrip 仍可验证宿主
  工具与内核 SHA-256 逐字节一致）。密码不再构建期生成，
  `build/etc-passwd.txt` 账本随之取消——凭据只来自开机时的
  注册向导
- `/bin/register`（`user/register.c`）：首启注册向导，两步——
  ① 设 root 密码（没有管理员的系统 useradd 永远无人能跑）；
  ② 创建首个普通用户（uid 自 etc_max_uid()+1 起、下限 100；
  空用户名 = 跳过，仅 root 系统同样合法）。密码两次输入、
  关回显，不匹配/为空三轮刹车（与 login 同款预算）；写入
  **shadow 先行**——中断后重跑只留下可按名替换的 shadow 行，
  绝不会留下"有 passwd 条目却无法登录"的半成品；root 的
  passwd 条目先查重再 append（重跑不产生重复行）。完成退出，
  init respawn getty，下一轮循环看到账户即给出登录提示
- getty 空账户分支（`user/getty.c`）：开机先问 `etc_user_count()`
  （etc.c 新增，与 passwd_scan 同有效性规则计 /etc/passwd 行数）
  ——为 0 就同 pid exec /bin/register 而不是打印登录提示；
  注册后的链路自动落回 init→getty→login→osh
- etc.c：`etc_valid_name` 把 useradd/register 共用的账户名规则
  （1-23 字符、不含 ':'）收拢成一份实现；
  `etc_passwd_append` 补 O_CREAT——镜像里第一次 append 就是
  注册向导写 root 行

**修复**
- `test_dev_registry` 无条件断言 `hd0` 存在——v0.8.0 起无盘 ramfs
  回退模式（`-append` 不带 `root=`、无 `-drive`）下该套件必红，
  GATES-FAILED、退出码 3，与套件注释"无盘应跳过而不失败"自相矛盾。
  现 hd0 相关断言以 `ata_present()` 为门，console/null/zero 的
  常驻断言保留；修复后无盘启动 43/43 + NOVIX-BOOT-OK，有盘路径
  的 gate H 断言不变
- `elf_load` argv 压栈 8 字节错位（v0.9.0 埋雷，osh 首次踩中）：
  `arr_beg ≡ 0 (mod 16)` 时 16 字节对齐在 rsp 与 argv 数组之间
  留出 8 字节空洞——exec 出的程序读到 argv[0]=(null)、
  argv[1]=arr[0]。是否踩中由参数长度组合决定（"-sh"、"magic42"
  幸运通过；`cat /tmp/pv2.txt` 必炸：argv[1] 变成 "cat"）。修复后
  数组位置钉死在 argc_off+8，与 [rsp]/[rsp+8] 严格成对
- /tmp 补 0777（Unix 惯例）：v0.9.0 的 ramfs /tmp 是 root 0755，
  凡人重定向全部 `cannot open`
- osh 首版三处（自测抓出）：2 字符词被 '>>' 分支误吞（`id` 直接
  syntax error）；tokenizer 只记 (指针,长度) 未 NUL 落地导致
  argv[0] 吞整行——改为词槽 arena 规范化；作业表满时旧实现漏杀
  中间阶段进程
- ktest perm_boundary 自建 fixture：镜像不再预置 /home/alice，
  测试自己 `vfs_mkdir` + chown 100（与 register/useradd 的
  运行期路径完全一致），uid 套件与镜像内容解耦，断言前后
  双向恢复现场
- keyboard.c 显式初始化 i8042（VMware 键盘死亡）：QEMU 的 BIOS
  开机已配好控制器，VMware 上电时扫描/翻译/IRQ1 路由均处于未知
  状态，keyboard_init 从未编程控制器——真机与 VMware 上键盘无
  响应。现在开机主动编程：清输出缓冲、停用 kbd/aux 后写命令字
  节 0x65（IRQ1 开 + set-1 翻译开）、0xF4 启用扫描；并新增 PS/2
  轮询兜底（keyboard_trychar 同时 poll 串口 LSR 与 i8042 状态口，
  覆盖 IRQ1 未路由的场景），鼠标字节（AUX 位）照旧丢弃
- 无盘 ISO 启动注册死循环：FAT 镜像才携带 /etc（mkfatimg.py），
  initrd 只打包 /bin/*——纯 ISO 引导时根目录没有 /etc，register
  以 O_CREAT 打开 /etc/shadow 因父目录不存在必失败，向导无限
  重启。修复为 initrd 同样打包 etc_files() 的零账户 /etc 内容
  （与 FAT 镜像同一来源，build.py）；顺带修掉 ramfs_mount_initrd
  对 0 字节文件调 kmalloc(0)（返回 NULL，被误判分配失败导致整
  个 initrd 拒载）的陷阱，并在无盘路径补建 /root、/home（FAT
  镜像自带、ramfs 无人创建）。修复后无盘 ISO 全链路冒烟通过：
  注册 root+用户 → 登录 uid=100 → poweroff 关机

**测试**
- 内核自测 43/43（-smp 2 与 -smp 1 双跑），shell 自动化 78/78
  （50 内核 shell + 26 首启注册/登录/脚本化/改密 + 2 断电存续）；
  门禁 C-J 全绿；check_syscalls 38/38 对齐。test_shell.py 启动时
  自行重建零账户镜像（从 build/user 重打包，秒级、无需重新编译）
  ——会话 2 必然踩中首次注册流程；会话 3 证明注册结果断电存续且
  向导不重跑（等待裸 login: 提示本身就是该断言）

**诚实边界**（教学取舍，1.x 再还）
- ISO 目标在缺 grub 工具链的宿主上跳过（本仓库 Windows 工具链
  不含 grub-pc-bin/xorriso），真机引导清单留 1.x
- osh 无引号/转义、变量不导出、无 Ctrl-C 到前台作业；/tmp 无
  sticky 位；/etc/init 只支持一个 respawn 服务
- register 对 root 密码无强度/复杂度检查（长度上限同
  ETC_PW_MAX=40）；无 userdel（注销账户留 1.x）；磁盘上残留的
  /home 目录由向导收养并 chown，不视为错误
- v0.9.0 的边界（SHA-256 无慢因子、FAT 权限位重启重建、单 tty、
  logout 不清 utmp）原样继承

## v0.9.0 —— 用户与登录（2026-10）

开机不再直接给 shell：init 读 /etc/inittab 拉起 getty，getty→login→sh
一条进程链完成"你是谁"的证明，uid/gid 从 PCB 一路贯穿到 VFS 权限位。

**新增**
- 身份层（`task.h/c`）：`task->uid/gid/ruid`（uint16_t）；fork 继承、
  exec 保持（POSIX 语义）、spawn 默认继承调用者。ruid 与 uid 分工：
  root 的 setuid 是登录式降落（两者同动，登录 shell 里 `id` 不显示
  多余 ruid）；SUID exec 时 ruid 继续记录"是谁调用的"
- 系统调用 +11（`syscall.h` 27-37 号）：GETUID/SETUID/GETGID/
  SETGID/GETRUID、CHDIR/GETCWD（目录 X 位检查，per-task cwd）、
  IOCTL（TCGETS/TCSETS，fd 0-2 的 echo 位——login 读密码前关回显）、
  MKDIR（父目录 W 检查）、CHOWN（root only）、POWEROFF；
  SYS_SPAWN/SYS_EXEC 扩参 argv/argc——ELF 加载器把 argv 数组压上
  用户栈，`main(argc, argv)` 第一次真正可用（getty→login 传用户名、
  login→sh 传 "-sh" 都靠它）
- VFS 权限检查（`fs/vfs.c vfs_permitted`）：uid 0 全通过；否则
  owner/group/world 三选一比对（owner 胜 group 胜 world，POSIX），
  检查点在 open（含 O_CREAT 的父目录 W、O_TRUNC/APPEND 的 W）、
  unlink、mkdir；新文件属主=创建者。0.2.0 埋下的 mode 位至此启用
- FAT32 权限补正（`kmain.c fs_perm_fixup`）：FAT 不存 Unix 位，
  boot 期对 inode 缓存一次性补正——/bin 全部 +x、/bin/passwd 加
  S_ISUID、/etc/shadow 收 0600、/home/<name> 按 /etc/passwd 的
  uid chown。实测坑：passwd 七字段解析时错用 comment 位当 home，
  家目录全部归了 root，perm_boundary 测试抓出
- SUID exec（`task.c task_exec`）：二进制带 S_ISUID 且属主非
  调用者 → uid 改采文件属主，ruid 留作调用者记录。教学简化：
  单 uid 无 euid 拆分，/bin/passwd 对凡人是"真变 root"而非
  "有效 root"
- 登录链三程序（`user/`）：getty（打 /etc/issue、"login: " 读名、
  **exec** /bin/login <name>——链上每个环节同 pid 换程序，shell
  退出即整链归零）；login（/etc/shadow 校验 salted SHA-256；错误
  用户名与错误密码同报 "Login incorrect" 不泄露账户存在性；3 次
  失败 3 秒刹车；成功 → 写 utmp → setgid/setuid 降落 → chdir(home)
  → exec(shell, "-sh")）；sh（用户态 shell：cd/logout/help/
  poweroff 内建 + /bin 程序 spawn+wait，"ktest@novix:/home/ktest$ "
  式提示符）
- init 收尸循环（`kmain.c getty_loop`）：读 /etc/inittab
  （"tty1:getty"，全路径直通），waitpid 收尸后 respawn——SysV
  respawn 规则，logout 即触发；`console=debug` 启动参数保留内核
  shell 作调试后门（shell 测试 session 1 即此模式）
- 密码体系：salted SHA-256 三份同算法——内核 `sha256.c`
  （shadow_roundtrip 用）、ulib 用户态一份（login/passwd 用）、
  构建期 `tools/mkpasswd.py`（烤进镜像）；root/alice 密码每次
  构建随机生成，记录在 build/etc-passwd.txt；ktest 用户固定
  "ktest-pass"（测试锚点）。**教学取舍**：无慢因子（bcrypt/
  PBKDF2 留 1.x）
- 用户命令 8 个：id（uid/gid 带用户名，SUID 下加显 ruid）、
  whoami、who（读 /var/run/utmp）、passwd（改密；系统唯一 SUID
  二进制——凡人 exec 后可改自己的密码，改他人密码仍需真 root）、
  useradd（root 专用，非 SUID：凡人 exec 即拒）、cat
  （stat-then-open 区分 no such file 与 permission denied）、
  touch、uidtest
- ktest uid 套件 +3（38 → 41）+ gate I：setuid_boundary（spawn
  /bin/uidtest 带 argv "magic42"——argv 传递 + root 降级 uid 100 +
  凡人 setuid(0) 必拒，一次边界漫步全证明）；shadow_roundtrip
  （构建期哈希 == 内核 SHA-256，逐字节）；perm_boundary（alice
  三向：写 /bin 拒、读 shadow 拒、写 /home/alice 成——uid 直接
  翻在 boot task 上，先恢复后断言，失败不可能泄漏 uid 100）
- test_shell.py 三会话：session 1 `console=debug` 内核 shell
  （v0.2-0.8 全量回归 50 项）；session 2 登录冒烟 13 项——错密码
  "Login incorrect" 后正确密码登录 ktest、id/whoami/who、
  cat /etc/shadow 被拒、passwd 改密回环（改完旧密码即死）、
  logout→getty respawn（banner 二次出现）→再登录→poweroff；
  session 3 断电存续 3 项——fresh boot 后只有新密码能登录
  （/etc/shadow 在 FAT 盘上活过关机），验证后把 ktest 密码改回
  "ktest-pass" 保持镜像幂等

**修复**
- `dir_append_slot` 只认 0x00 结束槽、从不复用 0xE5 删除槽——目录一旦
  "活项+删除项占满"（如上一轮启动留下的根目录），再建文件就追加新簇
  且目录永不收缩：fat_roundtrip 的 free-count 不变量在脏盘上必红
  （实测一次启动净漏 2 簇，40/41），反复建删文件的目录单调膨胀。现按
  链序取第一个 0xE5/0x00 槽（FAT 惯例；walker 遇 0xE5 即重置 LFN 组，
  单槽复用拼不进存活 LFN）。连带修掉 `fat_i_create` 的隐含猜测
  "off==0 且非首簇 ⇒ 刚追加的新簇、可免读盘"——复用后已存在簇的
  0 号槽也会被选中，盲目 memset 会抹掉同簇其余目录项；改为
  `dir_append_slot` 显式回传 fresh 标志。修复后同一脏盘连续 7 次启动
  挂载簇数零漂移
- init 门禁汇总行硬编码 `GATE-E-OK/FAILED`——门禁只有 C-E 时代的
  历史遗留（磁盘/uid 套件已是 gate H/I），改为 `GATES-OK/FAILED`
- `namei_parent` 把末尾的 `.`/`..` 当作待创建的名字交出去——FAT/ramfs
  的 walker 都跳过真 `.`/`..` 项，查重失效，`mkdir a/.`、`mkdir a/..`
  会造出第二个点目录项（POSIX 应 EEXIST/EINVAL）。现直接拒绝，
  路径中段的 `.`/`..` 不受影响
- 密码长度两端截断不一致：`passwd` 用全长（≤39 字符）哈希，
  `etc_verify` 却截到 32 字节——设置 33-39 字符的密码后永远验证
  不过，账户锁死。统一为 `etc.h` 的 `ETC_PW_MAX 40`（8+40 装得进
  64 字节哈希输入），两端同截
- `useradd` 不校验用户名：>23 字符的名字在 /etc/passwd 被截成
  pwent.name 大小而 /etc/shadow 写全名，两表对不上即无法登录；
  名字带 `:` 直接破坏行格式。现拒收 >23 字符或含 `:` 的名字
- `fat_i_create` 的 mkdir 错误路径漏簇：目录自身簇分配成功后，若
  写 "."/".." 或追加父目录槽位失败（盘满等），该簇无人引用也无人
  归还。现失败且 slot dirent 尚未落盘时 `chain_free` 回收
- VFS 权限检查补最终父目录的搜索（X）位：create/mkdir/unlink 此前
  只查 W，路径遍历只 X 检查了祖先目录——POSIX 对父目录要求 W+X，
  "有 W 无 X"的目录可被穿透创建/删除。`vfs_permitted(parent,
  W|X)` 三处补齐
- `etc_shadow_set` 残留的 `replaced` 标志 set 而不用（-Wall 报警），
  删除；删旧行+追加新行的逻辑不变
- `elf_load` 成功路径漏写 `*pd_out/*entry_out`——task_create_user
  拿到 0 页目录，调度器切 CR3 即三重故障（QEMU `-d int,cpu_reset`
  定位到 mov cr3,0）。此前调用方恰好都不读这两个出参，v0.9.0
  argv 改造后首次踩中
- `vfs_canonicalize` 只写 out[1..] 从不落 out[0]——绝对路径首字符
  是栈上垃圾，namei 必失败，sys_chdir 全链路瘫痪（login 进不了
  家目录、提示符恒 "/$ "）。补一行 `out[0]='/'`，login shell
  prompt 检查抓出
- utmp 双坑：镜像缺 `/var/run` 目录（mkfatimg 补建）+
  `etc_write_file` 开文件不带 O_CREAT（首写 /var/run/utmp 时
  文件不存在，open 失败静默丢会话，`who` 恒 "no sessions"）
- shadow_roundtrip 改锚 shadow-only 账户 `kck`：原校验钉死
  "ktest-pass"，test_shell session 3 启动时盘上密码已被 session 2
  改掉，gate E 必红。passwd 只重写目标用户行，kck 的构建期摘要
  永不被动——契约校验在任何镜像状态都成立
- hello 每个 tick 200→400ms：ctrl-c 检查的 ^C 落点原在 450ms、
  总时长 600ms，宿主一慢就擦边（实测 13 个 QEMU 并发时必失败）
- whoami 检查针 `'\nktest\n'`→`'\nktest\r'`：Windows 宿主 QEMU
  stdout 文本模式把 guest 的 CRLF 再展开成 0D 0D 0A——内核
  serial_putc 只发一次 CR，是宿主伪影不是 OS 缺陷；检查针锚定
  guest 真实发出的 CR 即跨宿主稳定
- uidtest ruid 断言对齐 POSIX：root setuid(100) 后 ruid 同步为
  100（登录降落语义），原断言 ruid==0 错误

**测试**
- 内核自测 41/41（`-smp 2`）+ 41/41（`-smp 1` 单核回归），
  shell 自动化 66/66（50 内核 shell + 13 登录/passwd 回环 +
  3 断电密码存续）；gate C-I 全绿（新增 gate I：uid 套件）

**诚实边界**（教学取舍，1.x 再还）
- SHA-256 无慢因子：离线爆破 /etc/shadow 理论可行；无密码长度/
  复杂度策略
- 权限补正只活 in inode 缓存（FAT 不存 Unix 位）：重启重建，
  运行期 chown/useradd 改的同样不落盘
- logout 不清 utmp 行（下次登录重写）；无 PAM/会话管理/登录
  时间戳；单 tty（COM1），VGA 多终端留 1.x
- 用户 shell 无重定向/管道/脚本/变量（v1.0.0 osh 的范围）

## v0.8.0 —— 磁盘与驱动框架（2026-10）

文件第一次活过断电：ATA 盘上真 FAT32 做根文件系统，驱动统一
注册成设备出现在 `/dev`，墙钟从 CMOS RTC 起步。

**新增**
- ATA PIO（`drivers/ata.c`）：主通道 0x1F0-0x1F7 主盘、LBA28、
  同步轮询（中断模式按计划留 1.x）。探测先"嗅总线"——选中空
  槽位读回 0xFF 即无盘，再 IDENTIFY 取扇区数；写后 FLUSH CACHE
  保证数据落盘。`ata_lock` 整命令串行（寄存器组是通道级共享
  状态，双核交叠命令会互相踩——叶子锁，调用者不得持更高级锁）；
  MBR 分区表解析（首个分区 LBA 2048 起）
- FAT32（`fs/fat32.c`）：BPB 解析 + 全 FAT 一次扫描定空闲簇数
  （FSInfo 只是参考，FAT 本体才是真相）；簇链遍历带 guard 防
  环。`dir_walk` 单遍扫描器统一 lookup/readdir/create/unlink
  四路径；**LFN 读**（0x0F 属性组、0x40 序号标记、校验和核对后
  按 UTF-16 组装）+ **LFN 写**（创建小写名时生成 8.3 别名 +
  LFN 项）——实测坑：别名恒大写，`needs_lfn` 必须大小写敏感
  比较，否则小写目录名 readdir 吐出大写别名，`ls /bin` 里
  hello 变 HELLO；inode 缓存按 (父簇, sname) 键控、lookup 时
  刷新磁盘现状；per-instance 自旋锁 + 512B 扇区 bounce 缓冲；
  `fat32_mount`/`fat32_unmount` 实例化/拆除（断电测试的基础）。
  写路径：簇分配 + FAT 链接 + 目录项追加（目录满则链新簇）、
  append 后 dirent 尺寸回写（`dirent_update`）、删除打 0xE5
  标记（含 LFN 区间）+ 簇链归还
- 统一驱动模型（`drivers/driver.c`）：`device_driver{name, probe,
  init}` + `device{name, mode, f_op}`；boot 期静态总线遍历
  probe→init→注册表，`/dev` 经 `ramfs_mknod` 发布——console、
  null、zero、hd0（`<blk>`，f->pos 计扇区的裸块设备）。注册表
  boot 期写一次之后只读，无需锁；设备 fops 自带各自的硬件锁
- RTC（`drivers/rtc.c`）：CMOS 0x70/0x71（索引位 7 顺带屏蔽
  NMI），经典读法——等 UIP 更新周期过去、整组读回、秒值复读
  证明稳定；BCD 按 B 寄存器 DM 位解码、12 小时制 PM 位兜底；
  世纪寄存器不通用，年 = 2000+yy。启动拍下 `boot_time` 快照；
  `rtc_unix_seconds` 折算 Unix 秒。`date` 命令原样打本地时间
  （QEMU `-rtc base=localtime`）+ unix 秒 + uptime——无时区
  机制，诚实标注 local
- **rootfs 切换**（`kmain.c`）：内核参数 `root=/dev/hd0` 时
  FAT32 根取代 ramfs 根，ramfs 只留 `/dev`、`/tmp` 两个目录
  钉在 FAT 根下；无 FAT 卷则诚实回退 ramfs。挂载表（`namei.c`，
  8 槽）：lookup 时挂载**遮蔽**底层同名项、`..` 从挂载根爬回
  父目录（`fs_root->i_parent = parent`）、`vfs_unmount` 干净
  拆除；**readdir 合并挂载点**——`ls /` 先列 dev、tmp 再列
  FAT 本体（bin、novix.txt），挂载点在其父目录可见，与真 OS
  一致
- 构建接线（`build.py` + `tools/mkfatimg.py`）：构建期生成
  64 MiB FAT32 镜像（MBR + LBA 2048 分区 + LFN 项），打包
  /bin 全部用户程序与 novix.txt 标记文件；QEMU `-drive
  if=ide,index=0`、`-rtc base=localtime`、`-append root=/dev/hd0`
- FILE 页缺页直接从 FAT32 inode 读入——exec/spawn 的按需分页
  跑在真磁盘上（gate C 的 /bin/hello 已在 FAT 根下）
- ktest 新增 4 例（34 → 38）+ gate H：dev_registry（注册表与
  /dev 节点一一对应）/ fat_roundtrip（写读 lseek append
  mkdir unlink 全往返 + **空闲簇账目精确**：一个文件一簇，
  删光归还）/ fat_powercycle（**断电语义**：另起一个独立的
  fat32_mount 实例读同一张盘——RAM 状态骗不过重挂，只有真
  到达盘片的数据幸存；读完 unmount 即"关机"）/ rtc_wallclock
  （启动时间合理 + 墙钟在走）
- shell `date` 命令；shell 自动检查 45 → 50（date 墙钟、
  novix.txt 标记、cat 标记、`<blk>` 块设备节点、FAT 根上
  write+read+rm 持久往返）

**测试**
- 内核自测 38/38（`-smp 2`）+ 38/38（`-smp 1` 单核回归），
  shell 自动化 50/50；gate C-H 全绿（新增 gate H：disk 套件）
- fat_powercycle 的独立实例重挂即模拟断电——验收标准"写文件
  → 重启 → 文件仍在"以更严格的形式成立：不重启就换一个全新
  内存状态读同一张盘

**修复**
- 文件类型判断全内核统一改掩码比较 `(mode & S_IFMT) == S_IF*`
  （ramfs/vfs/namei/task/shell 原为裸位测试 `mode & S_IFDIR`——
  S_IF* 是类型位段而非独立标志位，可并存叠加，`ls` 把块设备
  /dev/hd0 误报成 `<dir>`，shell 检查 dev block node 抓出）

**诚实边界**（教学取舍，1.x 再还）
- 无 fsck/日志：只保证顺序写不撕裂，掉电半写的修复留 1.x
- ATA 轮询占 CPU（每扇区 200K 次 status 轮询上限），IRQ 模式
  留 1.x；FAT32 无 rmdir 非空目录、无 truncate 收缩
- 无页缓存统一层：FAT 文件读写过自己的 bounce 缓冲，与
  swap/inode 生命周期的整合留 0.9.x

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
