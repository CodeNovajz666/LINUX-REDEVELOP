#!/usr/bin/env python3
"""Drive the Novix interactive shell over QEMU's -serial stdio."""
import os, sys, subprocess, time, threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(ROOT, 'build', 'kernel.elf')

QEMU = os.environ.get('QEMU')
if not QEMU:
    from shutil import which
    QEMU = which('qemu-system-x86_64')
if not QEMU:
    pkg = os.path.join(ROOT, '..', 'tools', 'qemu-pkg', 'qemu-system-x86_64.exe')
    QEMU = pkg if os.path.exists(pkg) else None
if not QEMU:
    sys.exit('qemu-system-x86_64 not found')

COMMANDS = []

EXPECT = [
    ('help',        'commands:'),
    ('ps',          'idle'),
    ('spawn',       'spawned pid'),
    ('spawner out', 'all children reaped'),
    ('fault_div',   'killed by sig8'),
    ('fault_segv',  'killed by SIGSEGV'),
    ('kill bad pid','no task with pid 999'),
    ('kill idle guard', 'cannot kill the idle task'),
    ('kill shell guard', 'cannot kill the shell'),
    ('policy',      'policy -> RR'),
    ('uptime',      'uptime:'),
    ('mem',         'kernel heap:'),
    ('echo',        'hello from serial'),
    # filesystem (v0.2.0)
    ('ls /',        '<dir>'),
    ('ls /bin',     'hello'),
    ('redirect >',  'novix fs'),
    ('redirect >>', 'more'),
    ('touch',       'empty.txt'),
    ('mkdir',       'dir1'),
    ('rm + missing','cat: /tmp/a.txt: no such file'),
    ('filetest usr','filetest: all checks passed'),
    # fork/exec/COW (v0.4.0)
    ('forker usr',  'COW-ISOLATION-OK'),
    ('forkacct usr','FORKACCT-OK'),
    ('exec usr',    'replacing image'),
    ('exec ok',     'exiting with status 42'),
    # user heap + signals + pipes (v0.5.0)
    ('malloc usr',  'MALLOCTEST-OK'),
    ('sig usr',     'SIGTEST-OK'),
    ('pipe usr',    'PIPETEST-OK'),
    ('mask usr',    'MASKTEST-OK'),
    ('kill bad sig','bad signal 99'),
    # demand paging / swap / mmap (v0.6.0)
    ('mem -v',      'swap slots free'),
    ('memhog usr',  'MEMHOG-OK'),
    # SMP (v0.7.0): both CPUs online in the interactive session
    ('cpus header', 'CPU  APIC  ONLINE  CURRENT'),
    ('cpus cpu0',   '0    0     yes'),
    ('cpus cpu1',   '1    1     yes'),
    # Ctrl+C -> SIGINT to the foreground task (v0.5.0)
    ('ctrl-c echo', '^C'),
    ('ctrl-c kill', 'sent SIGINT to pid'),
    # Backspace/DEL line editing
    ('backspace edit', '\nbs-edit ok'),
    # Tab completion
    ('tab list',    'cat clear'),
    ('tab uptime',  'uptime:'),
    ('tab forker',  'COW-ISOLATION-OK'),
    # relative path resolution (no cwd yet: resolved from /)
    ('touch rel',   'rel-content'),
    ('rm rel',      "rm: gone.txt: failed"),
]

# checks that need N occurrences, not just one
COUNT_EXPECT = [
    # history recall: the command is echoed once while typed and once more
    # when Up redraws it (then re-runs)
    ('history recall', 'echo hist-recall-9', 2),
    # Tab-completed 'spawn forke<Tab>' re-runs forker: once from the ktest
    # cow_isolation case, once from the shell session
    ('tab forker rerun', 'COW-ISOLATION-OK', 2),
]

# v0.7.0: run the shell session on 2 CPUs -- sockets=N so SeaBIOS reports
# every processor in the MP table (plain -smp 2 underreports).
proc = subprocess.Popen(
    [QEMU, '-kernel', KERNEL, '-m', '128M', '-append', 'interactive',
     '-serial', 'stdio', '-display', 'none', '-no-reboot',
     '-smp', '2,sockets=2,cores=1,threads=1',
     '-device', 'isa-debug-exit'],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

out = bytearray()
reader_done = threading.Event()

def send(c, wait=0.5):
    proc.stdin.write(c.encode())
    proc.stdin.flush()
    time.sleep(wait)

def send_typed(line, wait=0.5, char_delay=0.004, enter=True):
    """Type char-by-char: a no-flow-control 16550 FIFO (16 bytes) legitimately
    drops bytes on a full-line paste burst, and that is not the OS under test."""
    for ch in line:
        proc.stdin.write(ch.encode())
        proc.stdin.flush()
        time.sleep(char_delay)
    if enter:
        proc.stdin.write(b'\n')
        proc.stdin.flush()
        time.sleep(wait)

def reader():
    while True:
        b = proc.stdout.read(1)
        if not b:
            break
        out.extend(b)
    reader_done.set()

t = threading.Thread(target=reader, daemon=True)
t.start()

# gates C-G run before the prompt; the SMP suite (gate G) adds a few seconds
deadline = time.time() + 120
while time.time() < deadline:
    if b'novix>' in bytes(out):
        break
    time.sleep(0.2)

send_typed('help')
send_typed('ps')
send_typed('cpus')
send_typed('spawn spawner', wait=3.0)
send_typed('ps')
send_typed('spawn fault_div', wait=1.0)
send_typed('spawn fault_segv', wait=1.0)
send_typed('kill 999')
send_typed('kill 0')
send_typed('kill 1')
send_typed('policy 1')
# Tab completion: 'upt<Tab>' -> 'uptime ' (unique match)
send_typed('upt', enter=False, wait=0.2)
send('\t', wait=0.3)
send('\n', wait=0.5)
send_typed('mem')
send_typed('echo hello from serial')
# Backspace/DEL line editing: 'echx' + Backspace(DEL) + 'o ...' -> 'echo bs-edit ok'
send('\x7f', wait=0.2)                          # DEL on an empty line: harmless no-op
send_typed('echx', enter=False, wait=0.2)
send('\x7f', wait=0.2)                          # the byte a terminal sends for Backspace
send_typed('o bs-edit ok')
# Tab completion: 'c<Tab>' lists every command starting with c (cat, clear)
send('c', wait=0.2)
send('\t', wait=0.4)
send('\n', wait=0.3)                               # runs 'c' -> unknown command
# History: Up-arrow redraws the last command, Enter re-runs it
send_typed('echo hist-recall-9')
send('\x1b[A', wait=0.4)                           # Up: recall
send('\n', wait=0.5)
# filesystem (v0.2.0)
send_typed('ls')
send_typed('ls /bin')
send_typed('echo novix fs > /tmp/a.txt')
send_typed('cat /tmp/a.txt')
send_typed('echo more >> /tmp/a.txt')
send_typed('cat /tmp/a.txt')
send_typed('touch /tmp/empty.txt')
# relative paths resolve from / (namei no longer requires a leading slash)
send_typed('echo rel-content > relnote.txt')
send_typed('cat relnote.txt')
send_typed('touch gone.txt')
send_typed('rm gone.txt')
send_typed('rm gone.txt')
send_typed('mkdir /tmp/dir1')
send_typed('ls /tmp')
send_typed('rm /tmp/a.txt')
send_typed('cat /tmp/a.txt')
send_typed('spawn filetest', wait=4.0)
# fork/exec/COW (v0.4.0); 'spawn forke<Tab>' completes 'r' from /bin
send_typed('spawn forke', enter=False, wait=0.2)
send('\t', wait=0.3)
send('\n', wait=3.0)
send_typed('spawn forkacct', wait=4.0)
send_typed('spawn execer', wait=3.5)
# user heap + signals + pipes (v0.5.0)
send_typed('kill 999 99')
send_typed('spawn malloctest', wait=5.0)
send_typed('spawn sigtest', wait=4.0)
send_typed('spawn masktest', wait=4.0)
send_typed('spawn pipetest', wait=6.0)
# demand paging / swap / mmap (v0.6.0)
send_typed('mem -v')
send_typed('spawn memhog', wait=3.0)
# Ctrl+C -> SIGINT: hello lives ~600ms; interrupt it mid-sleep
send_typed('spawn hello', wait=0.4)
send('\x03', wait=0.8)
send_typed('poweroff', wait=1.0)

try:
    proc.wait(timeout=30)
except subprocess.TimeoutExpired:
    proc.kill()
    print('!! QEMU did not exit: poweroff never reached')
reader_done.wait(timeout=5)
text = out.decode('utf-8', 'replace')
with open(os.path.join(ROOT, 'build', 'shell_session.log'), 'w') as f:
    f.write(text)

print('---- session ----')
print(text)
print('---- checks ----')
failed = 0
for label, needle in EXPECT:
    ok = needle in text
    print(f"[{'ok' if ok else 'FAIL'}] {label}")
    failed += 0 if ok else 1
for label, needle, want in COUNT_EXPECT:
    got = text.count(needle)
    ok = got >= want
    print(f"[{'ok' if ok else 'FAIL'}] {label} (x{got}/{want})")
    failed += 0 if ok else 1

total = len(EXPECT) + len(COUNT_EXPECT)
print(f'\nshell test: {total - failed}/{total} checks passed')
sys.exit(0 if failed == 0 and proc.returncode in (0, 1) else 1)
