# Process Manager Kernel Module Makefile
# Complete build system for kernel module and test programs

# 内核模块配置
obj-m += proc_manager.o
proc_manager-objs := \
    src/procman.o \
    src/pcb.o \
    src/process.o \
    src/scheduler.o \
    src/scheduler_priority.o \
    src/scheduler_context.o \
    src/scheduler_realtime.o \
    src/resource.o \
    src/error.o \
    src/memory.o \
    src/file_system.o \
    src/signal.o \
    src/smp.o \
    src/security.o \
    src/performance.o

# 内核构建目录
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# 编译器标志
ccflags-y := -I$(PWD)/src -Wall -Werror

# 测试程序
TEST_TARGETS := \
    test/procman_test \
    test/procman_test_v2 \
    test/stress_test \
    test/scheduler_test \
    test/demo \
    test/comprehensive_test

EXAMPLE_TARGETS := \
    examples/example_simple \
    examples/example_advanced

# 默认目标
all: module

# 构建内核模块
module:
	@echo "Building Process Manager kernel module..."
	@$(MAKE) -C $(KDIR) M=$(PWD) modules
	@echo "✓ Kernel module built: proc_manager.ko"

# 构建所有测试程序
test: $(TEST_TARGETS)

# 构建示例程序
examples: $(EXAMPLE_TARGETS)

# 构建所有
all: module test examples

# 构建单个测试程序
test/procman_test: test/procman_test.c
	@echo "Building basic test program..."
	@gcc -Wall -O2 -o $@ $<
	@echo "✓ Built: $@"

test/procman_test_v2: test/procman_test_v2.c
	@echo "Building enhanced test program..."
	@gcc -Wall -O2 -o $@ $<
	@echo "✓ Built: $@"

test/stress_test: test/stress_test.c
	@echo "Building stress test program..."
	@gcc -Wall -O2 -pthread -o $@ $<
	@echo "✓ Built: $@"

test/scheduler_test: test/scheduler_test.c
	@echo "Building scheduler test program..."
	@gcc -Wall -O2 -o $@ $<
	@echo "✓ Built: $@"

test/demo: test/demo.c
	@echo "Building demo program..."
	@gcc -Wall -O2 -o $@ $<
	@echo "✓ Built: $@"

test/comprehensive_test: test/comprehensive_test.c
	@echo "Building comprehensive test program..."
	@gcc -Wall -O2 -o $@ $<
	@echo "✓ Built: $@"

# 构建示例程序
examples/example_simple: examples/example_simple.c
	@echo "Building simple example..."
	@gcc -Wall -O2 -o $@ $<
	@echo "✓ Built: $@"

examples/example_advanced: examples/example_advanced.c
	@echo "Building advanced example..."
	@gcc -Wall -O2 -pthread -o $@ $<
	@echo "✓ Built: $@"

# 清理构建文件
clean:
	@echo "Cleaning build files..."
	@$(MAKE) -C $(KDIR) M=$(PWD) clean
	@rm -f $(TEST_TARGETS) $(EXAMPLE_TARGETS) 2>/dev/null || true
	@rm -f *.o *.ko *.mod.c *.mod.o modules.order Module.symvers 2>/dev/null || true
	@rm -f src/*.o 2>/dev/null || true
	@rm -f .*.cmd 2>/dev/null || true
	@rm -rf .tmp_versions/ 2>/dev/null || true
	@echo "✓ Clean complete"

# 安装模块
install: module
	@echo "Installing module..."
	@sudo insmod proc_manager.ko
	@MAJOR=$$(grep procman /proc/devices | awk '{print $$1}'); \
	if [ -n "$$MAJOR" ]; then \
		sudo mknod -m 666 /dev/procman c $$MAJOR 0; \
		echo "✓ Device file created: /dev/procman"; \
	fi
	@echo "✓ Module installed"

# 卸载模块
uninstall:
	@echo "Uninstalling module..."
	@sudo rmmod proc_manager 2>/dev/null || true
	@sudo rm -f /dev/procman 2>/dev/null || true
	@echo "✓ Module uninstalled"

# 重新加载模块
reload: uninstall install
	@echo "✓ Module reloaded"

# 运行完整测试套件
test-all: module test
	@echo "Running complete test suite..."
	@sudo ./scripts/test_all.sh

# 运行演示
demo: module test/demo
	@echo "Running demo..."
	@sudo ./scripts/demo.sh

# 运行示例
run-example-simple: module examples/example_simple
	@echo "Running simple example..."
	@sudo ./examples/example_simple

run-example-advanced: module examples/example_advanced
	@echo "Running advanced example..."
	@sudo ./examples/example_advanced

# 查看内核日志
log:
	@echo "Showing kernel messages..."
	@sudo dmesg | tail -20

# 快速检查
check: module
	@echo "Quick module check..."
	@sudo insmod proc_manager.ko 2>&1 | grep -v "insmod" || true
	@sleep 1
	@sudo rmmod proc_manager
	@echo "✓ Module check passed"

# 创建设备文件（如果模块已加载）
create-device:
	@MAJOR=$$(grep procman /proc/devices | awk '{print $$1}'); \
	if [ -n "$$MAJOR" ]; then \
		sudo mknod -m 666 /dev/procman c $$MAJOR 0; \
		echo "Device file created: /dev/procman"; \
	else \
		echo "Module not loaded or major number not found"; \
	fi

# 帮助信息
help:
	@echo "Process Manager Kernel Module Build System"
	@echo ""
	@echo "Available targets:"
	@echo "  all              - Build everything (default)"
	@echo "  module           - Build kernel module only"
	@echo "  test             - Build all test programs"
	@echo "  examples         - Build all example programs"
	@echo "  clean            - Clean all build files"
	@echo "  install          - Install module and create device"
	@echo "  uninstall        - Remove module and device"
	@echo "  reload           - Reinstall module"
	@echo "  test-all         - Run complete test suite"
	@echo "  demo             - Run demonstration"
	@echo "  run-example-simple - Run simple example"
	@echo "  run-example-advanced - Run advanced example"
	@echo "  log              - Show kernel messages"
	@echo "  check            - Quick module check"
	@echo "  create-device    - Create device file (if module loaded)"
	@echo "  help             - Show this help"
	@echo ""
	@echo "Examples:"
	@echo "  make && sudo insmod proc_manager.ko"
	@echo "  make test-all"
	@echo "  make demo"
	@echo "  make run-example-advanced"

.PHONY: all module test examples clean install uninstall reload test-all demo \
        run-example-simple run-example-advanced log check create-device help