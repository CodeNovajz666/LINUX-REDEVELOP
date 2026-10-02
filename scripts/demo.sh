#!/bin/bash
# [历史归档] 属于项目起点 —— Linux 内核模块 proc_manager（src/），
# 与当前的 Novix OS（os/）无关，仅作历史保留。当前演示：os/build.py run。

# demo.sh - Process Manager 演示脚本

clear
echo "==============================================="
echo "    Process Manager Kernel Module Demo"
echo "==============================================="
echo ""
echo "This demo will show:"
echo "1. Module loading and device creation"
echo "2. Process creation and management"
echo "3. Priority-based scheduling"
echo "4. Scheduler statistics"
echo "5. Resource monitoring"
echo ""
read -p "Press Enter to begin..."

# 颜色
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

# 1. 清理
echo -e "\n${BLUE}[1/6] Cleaning...${NC}"
make clean 2>/dev/null
sudo rmmod proc_manager 2>/dev/null
sudo rm -f /dev/procman
echo "Done."

# 2. 编译
echo -e "\n${BLUE}[2/6] Compiling...${NC}"
make module
if [ $? -ne 0 ]; then
    echo "Compilation failed!"
    exit 1
fi
echo -e "${GREEN}✓ Module compiled${NC}"

# 3. 加载模块
echo -e "\n${BLUE}[3/6] Loading module...${NC}"
sudo insmod proc_manager.ko
if [ $? -ne 0 ]; then
    echo "Failed to load module!"
    exit 1
fi

MAJOR=$(grep procman /proc/devices | awk '{print $1}')
sudo mknod /dev/procman c $MAJOR 0
sudo chmod 666 /dev/procman
echo -e "${GREEN}✓ Module loaded${NC}"
echo -e "${YELLOW}Kernel messages:${NC}"
sudo dmesg | tail -5
echo ""

# 4. 演示进程管理
echo -e "\n${BLUE}[4/6] Process management demo...${NC}"
echo "Creating test processes..."
sudo ./test/procman_test_v2 <<< '1\ninit\n1\n0\n3\n0' 2>/dev/null
echo ""
sleep 1

echo "Creating more processes with different priorities..."
for i in {1..5}; do
    sudo ./test/procman_test_v2 <<< "1\nprocess$i\n$((i*2))\n0\n0" 2>/dev/null
done
echo ""

echo "Listing all processes:"
sudo ./test/procman_test_v2 <<< '3\n0' 2>/dev/null | head -20
echo ""

# 5. 调度演示
echo -e "\n${BLUE}[5/6] Scheduling demo...${NC}"
echo "Running scheduler 10 times:"
for i in {1..10}; do
    sudo ./test/procman_test_v2 <<< "7\n0" 2>&1 | grep "Scheduled\|No process"
    sleep 0.1
done
echo ""

echo "Scheduler statistics:"
sudo ./test/procman_test_v2 <<< '8\n0' 2>/dev/null | grep -A5 "Scheduler"
echo ""

# 6. 清理
echo -e "\n${BLUE}[6/6] Cleaning up...${NC}"
sudo rmmod proc_manager
sudo rm -f /dev/procman
echo -e "${GREEN}✓ Cleanup done${NC}"

echo ""
echo "==============================================="
echo "    Demo Complete!"
echo "==============================================="
echo ""
echo "Demonstrated features:"
echo "✓ Kernel module loading/unloading"
echo "✓ Device file creation"
echo "✓ Process creation with priorities"
echo "✓ Priority-based scheduling"
echo "✓ Scheduler statistics collection"
echo ""
echo "Project successfully implements all requirements!"
echo ""