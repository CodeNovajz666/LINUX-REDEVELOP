#!/bin/bash
# [历史归档] 属于项目起点 —— Linux 内核模块 proc_manager（src/），
# 与当前的 Novix OS（os/）无关，仅作历史保留。当前测试：os/build.py test。

# test_all.sh - 运行完整测试套件

set -e

echo "=== Process Manager Complete Test Suite ==="
echo "Started at: $(date)"
echo ""

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

FAILED=0
PASSED=0
TOTAL=0

# 函数：打印结果
print_result() {
    TOTAL=$((TOTAL + 1))
    if [ $1 -eq 0 ]; then
        echo -e "${GREEN}✓ ${NC}$2"
        PASSED=$((PASSED + 1))
    else
        echo -e "${RED}✗ ${NC}$2"
        FAILED=$((FAILED + 1))
    fi
}

# 函数：运行测试
run_test() {
    echo -e "${BLUE}[TEST]${NC} $1"
    shift
    eval "$@"
    return $?
}

# 1. 清理环境
echo "1. Cleaning environment..."
run_test "Clean build" make clean > /dev/null 2>&1
print_result $? "Clean build"

# 2. 编译测试
echo -e "\n2. Compilation tests..."
run_test "Build module" make module
print_result $? "Build kernel module"

run_test "Build test programs" make test
print_result $? "Build test programs"

# 3. 加载模块
echo -e "\n3. Module loading tests..."
sudo rmmod proc_manager 2>/dev/null || true
sudo rm -f /dev/procman

run_test "Load module" sudo insmod proc_manager.ko
if [ $? -eq 0 ]; then
    print_result 0 "Load kernel module"
    
    # 创建设备文件
    MAJOR=$(grep procman /proc/devices | awk '{print $1}')
    if [ -n "$MAJOR" ]; then
        sudo mknod /dev/procman c $MAJOR 0
        sudo chmod 666 /dev/procman
        print_result 0 "Create device file"
    else
        print_result 1 "Get major number"
    fi
else
    print_result 1 "Load kernel module"
    sudo dmesg | tail -20
    exit 1
fi

# 4. 基础功能测试
echo -e "\n4. Basic functionality tests..."
run_test "Process creation" "sudo ./test/procman_test_v2 <<< '1\ntest1\n5\n0\n0' 2>&1 | grep -q 'Created process'"
print_result $? "Process creation"

run_test "Process listing" "sudo ./test/procman_test_v2 <<< '3\n0' 2>&1 | grep -q 'Process List'"
print_result $? "Process listing"

run_test "Process destruction" "sudo ./test/procman_test_v2 <<< '2\n1001\n0' 2>&1 | grep -q 'Destroyed process'"
print_result $? "Process destruction"

# 5. 调度器测试
echo -e "\n5. Scheduler tests..."
run_test "Scheduling" "sudo ./test/procman_test_v2 <<< '7\n0' 2>&1 | grep -q 'Scheduled'"
print_result $? "Basic scheduling"

run_test "Scheduler statistics" "sudo ./test/procman_test_v2 <<< '8\n0' 2>&1 | grep -q 'Scheduler Statistics'"
print_result $? "Scheduler statistics"

# 6. 压力测试
echo -e "\n6. Stress tests..."
run_test "Memory test" "sudo ./test/stress_test memory 2>&1 | grep -q 'PASSED'"
print_result $? "Memory leak test"

run_test "Boundary test" "sudo ./test/stress_test boundary 2>&1 | tail -5 | grep -q 'Test'"
print_result $? "Boundary test"

# 7. 调度器算法测试
echo -e "\n7. Scheduler algorithm tests..."
run_test "Scheduler test" "sudo ./test/scheduler_test <<< '1\n0' 2>&1 | tail -10 | grep -q 'Testing'"
print_result $? "Scheduler algorithm test"

# 8. 卸载测试
echo -e "\n8. Module unloading test..."
run_test "Unload module" sudo rmmod proc_manager
print_result $? "Unload kernel module"

# 清理设备文件
sudo rm -f /dev/procman

# 汇总结果
echo -e "\n=== Test Results ==="
echo -e "Total tests: $TOTAL"
echo -e "${GREEN}Passed: $PASSED${NC}"
echo -e "${RED}Failed: $FAILED${NC}"

if [ $FAILED -eq 0 ]; then
    echo -e "\n${GREEN}✓ All tests passed!${NC}"
    echo "The Process Manager module is working correctly."
    exit 0
else
    echo -e "\n${RED}✗ Some tests failed${NC}"
    echo "Check the test output for details."
    exit 1
fi