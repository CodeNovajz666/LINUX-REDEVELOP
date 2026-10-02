#!/bin/bash
# clean.sh - 清理构建文件

echo "Cleaning Process Manager build files..."

# 1. 清理内核模块构建文件
make clean 2>/dev/null || true

# 2. 删除测试程序
rm -f test/procman_test 2>/dev/null || true
rm -f test/procman_test_v2 2>/dev/null || true
rm -f test/stress_test 2>/dev/null || true
rm -f test/scheduler_test 2>/dev/null || true
rm -f test/demo 2>/dev/null || true

# 3. 删除临时文件
rm -f *.o *.ko *.mod.c *.mod.o modules.order Module.symvers 2>/dev/null || true
rm -f src/*.o 2>/dev/null || true
rm -f .*.cmd 2>/dev/null || true
rm -rf .tmp_versions/ 2>/dev/null || true

# 4. 清理设备文件（如果已加载）
sudo rmmod proc_manager 2>/dev/null || true
sudo rm -f /dev/procman 2>/dev/null || true

# 5. 清理/proc文件（如果存在）
sudo rm -f /proc/procman 2>/dev/null || true

echo "Clean complete!"