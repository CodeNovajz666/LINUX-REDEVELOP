#!/bin/bash
# [历史归档] 属于项目起点 —— Linux 内核模块 proc_manager（src/），
# 与当前的 Novix OS（os/）无关，仅作历史保留。

# create_zip.sh - 创建proc_manager.zip发布包

set -e

echo "=== Creating proc_manager.zip Release Package ==="
echo ""

# 临时目录
TEMP_DIR="/tmp/proc_manager_release"
RELEASE_DIR="$TEMP_DIR/proc_manager"

# 清理旧的临时目录
rm -rf "$TEMP_DIR"
mkdir -p "$RELEASE_DIR"

echo "1. Copying project files..."

# 复制源代码
cp -r src/ "$RELEASE_DIR/"
cp -r test/ "$RELEASE_DIR/"
cp -r scripts/ "$RELEASE_DIR/"
cp -r docs/ "$RELEASE_DIR/"
cp -r examples/ "$RELEASE_DIR/"

# 复制根目录文件
cp Makefile "$RELEASE_DIR/"
cp README.md "$RELEASE_DIR/"
cp LICENSE "$RELEASE_DIR/"
cp .gitlab-ci.yml "$RELEASE_DIR/"

# 创建额外的文档
cat > "$RELEASE_DIR/INSTALL.md" << 'EOF'
# Installation Guide

## Quick Installation
bash
1. Clone or extract the package
unzip proc_manager.zip
cd proc_manager
2. Build the module
make clean
make
3. Load the module
sudo insmod proc_manager.ko
4. Create device file
MAJOR=(grep procman /proc/devices | awk '{print 1}')
sudo mknod /dev/procman c $MAJOR 0
sudo chmod 666 /dev/procman
## Testing
bash
Run all tests
make test-all
Or run individual tests
sudo ./test/procman_test_v2
sudo ./test/scheduler_test
sudo ./test/stress_test memory
## Examples
bash
Run examples
sudo ./examples/example_simple
sudo ./examples/example_advanced
## Verification
bash
Run complete verification
./scripts/final_verify.sh
## Uninstallation
bash
Unload module
sudo rmmod proc_manager
sudo rm -f /dev/procman
EOF

cat > "$RELEASE_DIR/QUICKSTART.md" << 'EOF'
# Quick Start Guide

## 5-Minute Demo

1. **Build and load the module:**
bash
make clean && make
sudo insmod proc_manager.ko
sudo ./scripts/create_device.sh
2. **Test basic functionality:**
bash
sudo ./test/procman_test_v2
Choose option 1 to create a process
Choose option 3 to list processes
Choose option 7 to trigger scheduling
3. **See scheduler in action:**
bash
sudo ./test/scheduler_test
4. **Clean up:**
bash
sudo rmmod proc_manager
## Key Features Demo

### Process Management
bash
Create processes
sudo ./test/procman_test_v2 <<< '1\ninit\n1\n0\n0'
sudo ./test/procman_test_v2 <<< '1\nbash\n5\n1\n0'
sudo ./test/procman_test_v2 <<< '1\nvim\n3\n0\n0'
List processes
sudo ./test/procman_test_v2 <<< '3\n0'
### Scheduling
bash
Test different scheduling policies
sudo ./test/scheduler_test <<< '1\n0' # Priority
sudo ./test/scheduler_test <<< '2\n0' # Round Robin
sudo ./test/scheduler_test <<< '3\n0' # FCFS
### Monitoring
bash
View system status
cat /proc/procman
Get statistics
sudo ./test/procman_test_v2 <<< '8\n0'
EOF

echo "2. Creating zip archive..."
cd "$TEMP_DIR"
zip -r "proc_manager.zip" "proc_manager" > /dev/null
mv "proc_manager.zip" "$HOME/"

echo "3. Verifying archive..."
cd "$HOME"
unzip -t "proc_manager.zip" > /dev/null

echo ""
echo "=== Release Package Created ==="
echo ""
echo "Package: $HOME/proc_manager.zip"
echo "Size: $(du -h "$HOME/proc_manager.zip" | cut -f1)"
echo ""
echo "Contents:"
unzip -l "$HOME/proc_manager.zip" | head -20
echo "..."
echo ""
echo "To use:"
echo "  unzip proc_manager.zip"
echo "  cd proc_manager"
echo "  make clean && make"
echo "  sudo insmod proc_manager.ko"
echo ""
echo "Ready for submission!"