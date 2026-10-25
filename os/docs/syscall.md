# Novix 系统调用参考（v1.1.0）

对标原内核模块的 `docs/api.md`。编号在 `kernel/include/syscall.h`
定义、`kernel/syscall.c syscall_dispatch` 分发、`user/ulib.c` 封装。
入口统一为 `int 0x80`，参数走 `rdi/rsi/rdx`，返回值在 `rax`
（long，-1 表示失败）。CI 校验本表与头文件一致。

| 编号 | 名称 | rdi / rsi / rdx | 返回 | 备注 |
|---:|---|---|---|---|
| 1 | EXIT | status | — | 不返回；128+信号 = 信号致死 |
| 2 | WRITE | fd=1 / buf / len | 写入数 | 缓冲区必须整体在用户区 |
| 3 | GETPID | — | pid | |
| 4 | YIELD | — | 0 | 主动让出 CPU |
| 5 | SLEEP | ms | 0 | |
| 6 | SPAWN | path / char** argv /（忽略） | pid | 裸名按 /bin 解析；v1.1.5 起 argv 为 NULL 结尾向量（ulib 封装不变），无环境 |
| 7 | WAITPID | pid(-1=任意) / flags(WNOHANG=1) | 退出码 | 0 = WNOHANG 下孩子仍在 |
| 8 | GETCHAR | — | 字符/0 | |
| 9 | SET_POLICY | 0..4 | 0 | PRIORITY/RR/FCFS/EDF/RMS |
| 10 | UPTIME | — | 毫秒 | |
| 11 | OPEN | path / flags | fd | 权限检查在 open 时进行 |
| 12 | READ | fd / buf / len | 读取数 | |
| 13 | CLOSE | fd | 0 | |
| 14 | LSEEK | fd / off / whence | 新位置 | 只对普通文件；不许越过 EOF |
| 15 | STAT | path / struct stat* | 0 | |
| 16 | FORK | — | 父=子 pid，子=0 | COW；fd 共享偏移 |
| 17 | EXEC | path / char** argv / char** envp | 不返回 | v1.1.5 ABI：argv/envp 均 NULL 结尾（envp=0 即空环境）；envp 压上新栈、crt0 发布为 environ；失败返回 -1，旧映像完好 |
| 18 | FREEPAGES | — | 空闲页数 | 调试用（测试断言） |
| 19 | SBRK | 增量 | 旧 break | |
| 20 | KILL | pid / sig | 0 | |
| 21 | SIGACTION | sig / handler / restorer | 0 | |
| 22 | SIGRETURN | — | — | 信号栈帧在用户栈上 |
| 23 | PIPE | int fds[2] | 0 | 环形缓冲 + EOF/EPIPE |
| 24 | SIGPROCMASK | how / set* / oldset* | 0 | |
| 25 | MMAP | len | va | 匿名；内核选地址 |
| 26 | MUNMAP | va / len | 0 | 必须整段 |
| 27 | GETUID | — | uid | |
| 28 | SETUID | uid | 0 | root 任意降；凡人仅可重申自身 |
| 29 | GETGID | — | gid | |
| 30 | SETGID | gid | 0 | 同 SETUID 规则 |
| 31 | CHDIR | path | 0 | 目录需要 X 位 |
| 32 | GETCWD | buf / len | strlen | len≥2 |
| 33 | IOCTL | fd(0-2) / req / u16* flags | 0 | TCGETS=0 TCSETS=1（位 0x8=ECHO）；v1.1.0 增 TCCLEAR=2（清屏回位，无 flags） |
| 34 | POWEROFF | — | — | isa-debug-exit 退出 QEMU |
| 35 | MKDIR | path | 0 | 父目录需要 W+X |
| 36 | CHOWN | path / uid / gid | 0 | 仅 root；FAT 下只改 inode 缓存 |
| 37 | GETRUID | — | ruid | SUID exec 后仍是调用者 |
| 38 | DUP2 | oldfd / newfd | newfd | v1.0.0：osh 重定向/管道接线 |
| 39 | PS | buf / len | 字节数 | v1.0.0：osh 内建 ps；任务表报告，截断式写入 |
| 40 | CPUS | buf / len | 字节数 | v1.0.0：osh 内建 cpus；每 CPU 一行，截断式写入 |
| 41 | MEMINFO | buf / len / verbose | 字节数 | v1.0.0：osh 内建 mem；verbose≠0 附加 mm/swap 行 |
| 42 | GETDENTS | path / buf / len | 字节数 | v1.1.0：/bin/ls；目录须授 R；内核 shell 同源报告，截断式写入 |
| 43 | UNLINK | path | 0 | v1.1.0：/bin/rm；拒绝目录；父目录 W 在 VFS 内检查 |
| 44 | DATETIME | buf / len | 字节数 | v1.1.0：/bin/date；RTC 墙钟（与内核 shell date 同行格式）；RTC 不 sane 返 -1 |
| 45 | RENAME | old path / new path | 0 | v1.1.2：/bin/mv 快路径；inode 不变（同实例内）；两个父目录 W+X；目标存在拒绝 -1（不覆盖） |
| 46 | RMDIR | path | 0 | v1.1.2：/bin/rmdir；仅空目录；父目录 W+X |
| 47 | SETREUID | ruid / euid | 0 | v1.1.7：-1 保留原值；root 任意，凡人仅可取自身 (ruid,euid) 内的值——可降可回不可升 |
| 48 | SETREGID | rgid / egid | 0 | v1.1.7：rgid 拆分后的组版本，同上规则 |
| 49 | ACCESS | path / mode | 0/-1 | v1.1.7：以真实 (ruid,rgid) 做权限判定（SUID 程序问"调用者能不能"就用它）；mode 为 R=4/W=2/X=1 位组合 |

## ABI 约定

- argv 传递（SPAWN/EXEC）：rsi = `char** argv`（指向用户内存的指针
  数组），rdx = argc；`(0, 0)` 表示"无参数"，argv[0] 取路径本身。
  ELF 加载器把 argc、argv 数组和字符串压上新栈（`kernel/elf.c`，
  [rsp]=argc、[rsp+8]=argv[0]，rsp 16 字节对齐）。
- 用户指针合法性：所有系统调用在拷贝前校验缓冲区落在
  `[USER_CODE_BASE, USER_STACK_TOP)` 内。
- 权限检查点：open（含 O_CREAT 的父目录 W+X、O_TRUNC/APPEND 的 W）、
  unlink/mkdir（父目录 W+X）、chdir（目标 X）、执行（文件 X）、
  chown（仅 root）。owner > group > world，uid 0 全通过。
- v1.1.2：rename 两个父目录 W+X、rmdir 父目录 W+X（与 unlink/mkdir
  同一层）；跨文件系统实例 rename 返回 -1（EXDEV 语义，ABI 无 errno）。
