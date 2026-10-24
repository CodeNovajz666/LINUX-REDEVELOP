# Novix 驱动编写指南（v1.1.0）

Novix 的驱动模型是 Linux driver/device 拆分的教学缩样：驱动描述
"怎么探测、怎么初始化"，设备描述"叫什么、怎么读写"。没有热插拔——
注册表是静态数组，总线就是这台机器本身。

## 1. 两个结构体

```c
/* kernel/include/driver.h */
struct device {
    const char *name;                        /* "hd0"、"rtc0" ... */
    uint16_t mode;                           /* S_IFCHR 或 S_IFBLK */
    const struct file_operations *f_op;      /* read/write/close */
    void *priv;                              /* 驱动私有状态 */
};

struct device_driver {
    const char *name;
    int  (*probe)(void);                     /* 硬件在吗？1/0 */
    struct device *(*init)(void);            /* 上电；NULL = 失败 */
};
```

`f_op` 就是 VFS 的 file_operations（`kernel/include/vfs.h`）——设备
与普通文件在系统调用层完全同构，`open("/dev/hd0")` 拿到的 `struct
file` 和打开一个 FAT 文件没有区别。

## 2. 注册与生命周期

1. **boot 期注册**：在 `drivers/driver.c driver_init()` 里（或链接期
   数组）调用 `driver_register(&你的驱动)`——必须发生在
   `driver_bus_init()` 之前。
2. **总线遍历**：`driver_bus_init()` 逐个 probe；探测成功的驱动立刻
   init，返回的 `struct device *` 进入注册表。
3. **发布到 /dev**：`devfs_publish()` 把每个活设备 `ramfs_mknod` 成
   `/dev/<name>` 节点。从此用户程序 `open`/`read`/`write` 它。
4. **不再变化**：注册表 boot 期写一次之后只读，无需锁。设备各自
   的 f_op 自带硬件锁（见 §4）。

参考实现：`drivers/ata.c`（块设备 + MBR 分区）、`drivers/rtc.c`
（字符设备 + wall clock）、`drivers/driver.c` 内建的
console/null/zero。

## 3. 写一个最小字符设备

```c
#include "driver.h"
#include "vfs.h"

static int mydev_read(struct file *f, void *buf, uint32_t len) { ... }
static int mydev_write(struct file *f, const void *buf, uint32_t len) { ... }
static const struct file_operations mydev_fops = {
    .read = mydev_read, .write = mydev_write,
};

static struct device mydev_dev = {
    .name = "mydev", .mode = S_IFCHR, .f_op = &mydev_fops,
};

static int mydev_probe(void)  { return mydev_hw_present(); }
static struct device *mydev_init(void) { mydev_hw_init(); return &mydev_dev; }

static const struct device_driver mydev_driver = {
    .name = "mydev", .probe = mydev_probe, .init = mydev_init,
};

/* 在 driver_init() 处（或 boot 早期）调用 */
driver_register(&mydev_driver);
```

启动后 `/dev/mydev` 出现，`cat /dev/mydev | grep x` 这样的 osh
管道直接作用于你的硬件。

## 4. 锁与中断的规矩

- **注册表无锁**：boot 期写一次、之后只读。
- **f_op 自带叶子锁**：ATA 用 `ata_lock` 串行整条命令（寄存器组是
  通道级共享状态，双核交叠命令会互相踩）。锁次序铁律见
  `DESIGN.md` §4.3：`sched_lock > fs/pipe > pmm/heap > console`，
  叶子锁不得持更高级锁时获取。
- **同步轮询优先**：v0.8.0 的 ATA 是 PIO 轮询（中断模式留 1.x）。
  教学阶段"能睡的都睡、能轮询的都轮询"是特性不是欠债。
- **写后落盘**：块设备写路径结尾 FLUSH CACHE（`ata_write_sectors`
  的调用方负责 flush），断电测试 `fat_powercycle` 才能证明数据
  真的到了盘上。

## 5. 验收清单（进入内核树前自查）

- [ ] probe 在无硬件时安静返回 0，不打印、不崩
- [ ] init 失败返回 NULL，已分配资源全部归还
- [ ] /dev 节点可以被 osh `cat`、重定向，SMP 下 2 CPU 并发读写不炸
- [ ] f_op 的锁满足全局锁次序（叶子锁）
- [ ] ktest 有对应用例（参考 `dev_registry` / `fat_roundtrip`）
