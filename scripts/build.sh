#!/bin/bash
# build.sh - Process Manager 构建脚本

set -e  # 出错时退出

echo "=== Process Manager Build Script ==="
echo ""

# 检查内核头文件
echo "1. Checking kernel headers..."
if [ ! -d /lib/modules/$(uname -r)/build ]; then
    echo "ERROR: Kernel headers not found!"
    echo "Please install: sudo apt install linux-headers-$(uname -r)"
    exit 1
fi
echo "✓ Kernel headers found"

# 清理
echo ""
echo "2. Cleaning previous build..."
make clean 2>/dev/null || true
echo "✓ Cleaned"

# 编译内核模块
echo ""
echo "3. Building kernel module..."
make module
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to build kernel module"
    exit 1
fi
echo "✓ Kernel module built: proc_manager.ko"

# 编译测试程序
echo ""
echo "4. Building test programs..."
make test
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to build test programs"
    exit 1
fi
echo "✓ Test programs built"

# 检查文件
echo ""
echo "5. Build results:"
ls -lh proc_manager.ko
ls -lh test/procman_test
ls -lh test/procman_test_v2
ls -lh test/stress_test
ls -lh test/scheduler_test

echo ""
echo "=== Build Complete ==="
echo ""
echo "To load module:"
echo "  sudo insmod proc_manager.ko"
echo "  sudo ./scripts/create_device.sh"
echo ""
echo "To run tests:"
echo "  sudo ./test/procman_test_v2"
echo "  sudo ./test/scheduler_test"
echo ""
echo "To unload:"
echo "  sudo rmmod proc_manager"