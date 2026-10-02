# Process Manager Test Cases

## 1. Overview
This document describes the test cases for the Process Manager kernel module.

## 2. Test Categories

### 2.1 Unit Tests
Testing individual components in isolation.

### 2.2 Integration Tests
Testing interactions between components.

### 2.3 System Tests
Testing the complete system.

### 2.4 Stress Tests
Testing under high load conditions.

### 2.5 Boundary Tests
Testing edge cases and limits.

## 3. Test Cases

### 3.1 Module Loading/Unloading

#### TC-001: Module Loading
**Description**: Test module loading
**Preconditions**: Clean system
**Steps**:
1. Run `sudo insmod proc_manager.ko`
2. Check return code
3. Verify module appears in `lsmod`
4. Check kernel messages
**Expected Results**:
- Module loads successfully
- No error messages
- Device file can be created

#### TC-002: Module Unloading
**Description**: Test module unloading
**Preconditions**: Module loaded
**Steps**:
1. Run `sudo rmmod proc_manager`
2. Check return code
3. Verify module removed from `lsmod`
**Expected Results**:
- Module unloads successfully
- All resources freed
- Clean kernel log

### 3.2 Process Management

#### TC-003: Process Creation
**Description**: Create a new process
**Preconditions**: Module loaded, device open
**Steps**:
1. Call `PROC_MAN_CREATE_PROC` ioctl
2. Provide valid process name and priority
3. Check return value
4. Verify process appears in list
**Expected Results**:
- Returns valid PID (>0)
- Process appears in process list
- Correct name and priority set

#### TC-004: Process Creation with Invalid Name
**Description**: Test error handling for invalid names
**Preconditions**: Module loaded
**Steps**:
1. Try to create process with empty name
2. Try to create process with very long name
3. Try to create process with special characters
**Expected Results**:
- All attempts fail with appropriate error
- No processes created

#### TC-005: Process Creation with Invalid Priority
**Description**: Test priority validation
**Preconditions**: Module loaded
**Steps**:
1. Try to create process with priority 0
2. Try to create process with priority 11
3. Try to create process with priority -1
**Expected Results**:
- All attempts fail
- Error indicates invalid priority

#### TC-006: Process Destruction
**Description**: Destroy an existing process
**Preconditions**: Process created
**Steps**:
1. Create a process
2. Call `PROC_MAN_DESTROY_PROC` with its PID
3. Check return value
4. Verify process removed from list
**Expected Results**:
- Returns success (0)
- Process removed from list
- Memory freed

#### TC-007: Destroy Non-existent Process
**Description**: Test error handling
**Preconditions**: Module loaded
**Steps**:
1. Call `PROC_MAN_DESTROY_PROC` with invalid PID
2. Call with non-existent PID
**Expected Results**:
- Both calls fail
- Appropriate error returned

### 3.3 Process Listing

#### TC-008: List Processes (Empty)
**Description**: List processes when none exist
**Preconditions**: No processes created
**Steps**:
1. Call `PROC_MAN_LIST_PROCS`
2. Check returned count
**Expected Results**:
- Returns count 0
- Empty process list

#### TC-009: List Processes (Multiple)
**Description**: List multiple processes
**Preconditions**: Multiple processes created
**Steps**:
1. Create 5 processes
2. Call `PROC_MAN_LIST_PROCS`
3. Verify all processes listed
**Expected Results**:
- Returns count 5
- All processes listed with correct info
- Order maintained

#### TC-010: Partial Buffer
**Description**: Test with insufficient buffer
**Preconditions**: 10 processes created
**Steps**:
1. Create 10 processes
2. Call `PROC_MAN_LIST_PROCS` with buffer size 5
3. Check returned count
**Expected Results**:
- Returns count 5
- Only first 5 processes returned
- No crash or corruption

### 3.4 Process States

#### TC-011: State Transitions (Valid)
**Description**: Test valid state changes
**Preconditions**: Process created
**Steps**:
1. Create process in READY state
2. Change to RUNNING
3. Change to BLOCKED
4. Change to READY
5. Change to TERMINATED
**Expected Results**:
- All transitions succeed
- State changes correctly
- No side effects

#### TC-012: State Transitions (Invalid)
**Description**: Test invalid state changes
**Preconditions**: Process created
**Steps**:
1. Try READY → NEW
2. Try RUNNING → NEW
3. Try TERMINATED → READY
**Expected Results**:
- All transitions fail
- Error indicates invalid transition
- State unchanged

#### TC-013: State Persistence
**Description**: Verify state maintained
**Preconditions**: Process created
**Steps**:
1. Set process to RUNNING
2. List processes
3. Verify state preserved
**Expected Results**:
- State remains RUNNING
- Correct in process list

### 3.5 Process Priority

#### TC-014: Priority Setting (Valid)
**Description**: Test valid priority changes
**Preconditions**: Process created
**Steps**:
1. Create process with priority 5
2. Change to priority 1
3. Change to priority 10
4. Verify changes
**Expected Results**:
- All changes succeed
- Priority updated correctly
- Affects scheduling

#### TC-015: Priority Setting (Invalid)
**Description**: Test invalid priority
**Preconditions**: Process created
**Steps**:
1. Try to set priority 0
2. Try to set priority 11
3. Try to set priority -5
**Expected Results**:
- All attempts fail
- Priority unchanged
- Appropriate error

### 3.6 Scheduling

#### TC-016: Basic Scheduling
**Description**: Test scheduling trigger
**Preconditions**: Processes created
**Steps**:
1. Create 3 processes with different priorities
2. Call `PROC_MAN_SCHEDULE` 5 times
3. Verify which process runs
**Expected Results**:
- Highest priority runs first
- Same priority: FIFO order
- Correct process selected

#### TC-017: Priority Scheduling
**Description**: Test priority-based scheduling
**Preconditions**: Multiple processes
**Steps**:
1. Create processes with priorities 1, 5, 10
2. Trigger scheduling
3. Verify priority 1 runs
4. Block priority 1
5. Verify priority 5 runs
**Expected Results**:
- Highest ready priority always runs
- Lower priorities only when higher blocked

#### TC-018: Round Robin Scheduling
**Description**: Test round-robin with priorities
**Preconditions**: Set to round-robin policy
**Steps**:
1. Set scheduler to round-robin
2. Create processes with different priorities
3. Trigger scheduling multiple times
4. Verify time slicing
**Expected Results**:
- Processes get time slices
- Time slice = priority * 10
- Fair scheduling

#### TC-019: FCFS Scheduling
**Description**: Test first-come first-served
**Preconditions**: Set to FCFS policy
**Steps**:
1. Set scheduler to FCFS
2. Create processes in order
3. Trigger scheduling
4. Verify order maintained
**Expected Results**:
- Processes run in creation order
- No preemption
- Simple scheduling

#### TC-020: Aging Mechanism
**Description**: Test priority aging
**Preconditions**: Low priority processes
**Steps**:
1. Create low priority process
2. Create high priority process
3. Run scheduler many times
4. Trigger aging
5. Verify priority increased
**Expected Results**:
- Low priority gets CPU eventually
- Priority increases with wait time
- Prevents starvation

### 3.7 Scheduler Statistics

#### TC-021: Statistics Collection
**Description**: Test stats collection
**Preconditions**: Scheduler active
**Steps**:
1. Get initial statistics
2. Perform scheduling operations
3. Get updated statistics
4. Verify changes
**Expected Results**:
- Statistics update correctly
- Counts match operations
- No corruption

#### TC-022: Statistics Reset
**Description**: Test stats reset
**Preconditions**: Statistics collected
**Steps**:
1. Perform some operations
2. Reset statistics
3. Verify all zero
**Expected Results**:
- All statistics reset to zero
- Can continue collecting

### 3.8 Resource Tracking

#### TC-023: CPU Time Tracking
**Description**: Test CPU time measurement
**Preconditions**: Process running
**Steps**:
1. Create process
2. Schedule it multiple times
3. Check CPU time
**Expected Results**:
- CPU time increases
- Proportional to scheduling
- Accurate measurement

#### TC-024: Memory Tracking
**Description**: Test memory usage tracking
**Preconditions**: Process created
**Steps**:
1. Create process
2. Update memory usage
3. Verify tracking
**Expected Results**:
- Memory usage tracked
- Updates correctly
- No memory leaks

#### TC-025: I/O Tracking
**Description**: Test I/O operation tracking
**Preconditions**: Process created
**Steps**:
1. Simulate I/O operations
2. Update I/O count
3. Verify tracking
**Expected Results**:
- I/O count tracked
- Updates correctly
- Accurate reporting

### 3.9 Stress Testing

#### TC-026: Maximum Processes
**Description**: Test process limit
**Preconditions**: Module loaded
**Steps**:
1. Create processes until limit
2. Try to create one more
3. Verify error
**Expected Results**:
- Can create up to limit
- Error on exceed
- No crash

#### TC-027: High Frequency Operations
**Description**: Test rapid operations
**Preconditions**: Module loaded
**Steps**:
1. Create/destroy processes rapidly
2. Schedule rapidly
3. Monitor stability
**Expected Results**:
- No crashes
- No memory leaks
- Correct operation

#### TC-028: Concurrent Access
**Description**: Test multiple threads
**Preconditions**: Module loaded
**Steps**:
1. Run multiple test threads
2. Concurrent operations
3. Check for races/corruption
**Expected Results**:
- Thread-safe operation
- No corruption
- Correct results

### 3.10 Boundary Cases

#### TC-029: Name Boundaries
**Description**: Test name length limits
**Preconditions**: Module loaded
**Steps**:
1. Create process with 31-char name
2. Create with 32-char name
3. Verify behavior
**Expected Results**:
- 31 chars: success
- 32 chars: truncated
- No buffer overflow

#### TC-030: PID Wraparound
**Description**: Test PID allocation
**Preconditions**: Many processes created
**Steps**:
1. Create many processes
2. Force PID wraparound
3. Verify no conflicts
**Expected Results**:
- PID allocation works
- No duplicate PIDs
- Correct wraparound

#### TC-031: Zero Values
**Description**: Test zero/edge values
**Preconditions**: Module loaded
**Steps**:
1. Test with priority 0
2. Test with invalid states
3. Test with null pointers
**Expected Results**:
- Proper validation
- Error returns
- No crashes

### 3.11 Error Handling

#### TC-032: Invalid ioctl Commands
**Description**: Test invalid commands
**Preconditions**: Module loaded
**Steps**:
1. Send invalid ioctl command
2. Send out-of-range command
3. Verify error
**Expected Results**:
- Returns ENOTTY
- No side effects
- Module stable

#### TC-033: Memory Allocation Failure
**Description**: Test OOM handling
**Preconditions**: Module loaded
**Steps**:
1. Simulate memory pressure
2. Try to create process
3. Verify handling
**Expected Results**:
- Graceful failure
- No crash
- Resources freed

#### TC-034: Invalid User Pointers
**Description**: Test bad pointers
**Preconditions**: Module loaded
**Steps**:
1. Pass invalid user pointers
2. Pass NULL pointers
3. Verify handling
**Expected Results**:
- Returns EFAULT
- No kernel oops
- Safe handling

### 3.12 /proc Interface

#### TC-035: /proc File Access
**Description**: Test /proc interface
**Preconditions**: Module loaded
**Steps**:
1. Read /proc/procman
2. Verify format
3. Verify content
**Expected Results**:
- File readable
- Correct format
- Accurate data

#### TC-036: Concurrent /proc Reads
**Description**: Test multiple readers
**Preconditions**: Module loaded
**Steps**:
1. Multiple processes read /proc
2. Concurrent access
3. Verify stability
**Expected Results**:
- Concurrent reads work
- No corruption
- No deadlocks

## 4. Test Automation

### 4.1 Automated Test Scripts
- `scripts/test_all.sh`: Complete test suite
- `scripts/run_tests.sh`: Individual test categories
- `scripts/stress_test.sh`: Stress testing
- `scripts/boundary_test.sh`: Boundary testing

### 4.2 Test Coverage
- Code coverage: >90%
- Path coverage: >85%
- Boundary coverage: 100%
- Error path coverage: >80%

## 5. Test Environment

### 5.1 Requirements
- Linux kernel 4.0+
- Kernel headers installed
- GCC compiler
- Root access (for module operations)
- 512MB+ free memory
- Multi-core CPU (for concurrency tests)

### 5.2 Setup
bash
1. Install dependencies
sudo apt install build-essential linux-headers-$(uname -r)
2. Build module
make clean
make
3. Run tests
./scripts/test_all.sh
复制
## 6. Expected Results

### 6.1 Success Criteria
- All tests pass
- No memory leaks
- No crashes or hangs
- Correct functionality
- Good performance

### 6.2 Performance Targets
- Process creation: < 10ms
- Scheduling: < 1ms
- Context switch: < 5ms
- Memory usage: < 1MB base