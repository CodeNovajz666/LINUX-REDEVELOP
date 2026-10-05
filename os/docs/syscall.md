# Novix 系统调用参考（v1.0.0）

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
| 6 | SPAWN | path / char** argv / argc | pid | 裸名按 /bin 解析；(0,0) = 无参数 |
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
| 17 | EXEC | path / char** argv / argc | 不返回 | 失败返回 -1，旧映像完好 |
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
| 33 | IOCTL | fd(0-2) / req / u16* flags | 0 | TCGETS=0 TCSETS=1，位 0x8=ECHO |
| 34 | POWEROFF | — | — | isa-debug-exit 退出 QEMU |
| 35 | MKDIR | path | 0 | 父目录需要 W+X |
| 36 | CHOWN | path / uid / gid | 0 | 仅 root；FAT 下只改 inode 缓存 |
| 37 | GETRUID | — | ruid | SUID exec 后仍是调用者 |
| 38 | DUP2 | oldfd / newfd | newfd | v1.0.0：osh 重定向/管道接线 |

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
