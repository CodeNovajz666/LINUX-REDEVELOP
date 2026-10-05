#!/usr/bin/env python3
"""Drive the Novix interactive shell over QEMU's -serial stdio.

Three sessions (v0.9.0):
  session 1 -- console=debug boots straight into the kernel shell: the
               full command battery (v0.2.0 .. v0.8.0 regression).
  session 2 -- the default boot now hands the machine to the login world
               (init -> getty -> login -> sh). Login smoke test as the
               fixed "ktest" account (password "ktest-pass"; the separate
               shadow-only "kck" anchor keeps the shadow_roundtrip ktest
               byte-exact on this same image -- see mkfatimg.py):
               wrong password rejected, shadow unreadable, passwd
               roundtrip, logout -> getty respawn, old password dead.
  session 3 -- fresh boot: only the new password works (the shadow lives
               on the FAT32 disk), then ktest-pass is restored so the
               run stays idempotent against the same image.
"""
import os, sys, subprocess, time, threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(ROOT, 'build', 'kernel.elf')
DISK = os.path.join(ROOT, 'build', 'novix-hd.img')

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
    # disk + RTC (v0.8.0): FAT32 root, marker file, wall clock, block device
    ('date wallclock', ' local (unix '),
    ('ls shows marker', 'novix.txt'),
    ('cat marker', 'survive poweroff'),
    ('dev block node', '<blk>'),
    ('fat write+read', 'disk-persist-line'),
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

# session 2 (v0.9.0): the login world, including the passwd roundtrip
LOGIN_EXPECT = [
    ('login issue',        'authorized users only'),
    ('login prompt',       'login: '),
    ('password prompt',    'Password: '),
    ('bad password',       'Login incorrect'),
    ('login shell prompt', 'ktest@novix:/home/ktest$'),
    ('id shows ktest',     'uid=200(ktest) gid=200(ktest)'),
    ('whoami',             '\nktest\r'),   # guest serial emits CRLF; a Windows
                                           # host QEMU doubles the CR (0D 0D 0A),
                                           # so anchor on the CR the OS sent
    ('who session',        'ktest   tty1   pid '),
    ('shadow denied',      'cat: /etc/shadow: permission denied'),
    ('passwd current ask', 'Current password: '),
    ('passwd updated',     'passwd: password updated for ktest'),
    # v1.0.0: osh scripting
    ('osh exit status',    'code=1'),
    ('osh background job', '[1] pid '),
    ('osh comments',       'cc-comment'),
]
LOGIN_COUNT_EXPECT = [
    # logout unwinds the getty->login->sh chain back to init, which
    # respawns getty: the banner prints a second time
    ('getty respawn', 'authorized users only', 2),
    # wrong password at first login + the dead old password after the
    # passwd roundtrip
    ('old password dead', 'Login incorrect', 2),
    # v1.0.0: 'cat /etc/motd | grep Novix > /tmp/x' + cat -- the needle
    # hits the boot banner (1), motd at each of the two logins (2) and the
    # redirected copy (1)
    ('osh motd pipeline', 'Novix 1.0.0 -- a real OS', 4),
    # variable + redirect + pipe + grep + cat: two cats of derived files;
    # the echoed command lines only ever contain '$p-pipe', never the value
    ('osh var+pipe+redirect', 'zz-pipe', 2),
]

# session 3 (v0.9.0): fresh boot -- the password change survived poweroff
# (shadow lives on the FAT32 disk); restore ktest-pass so the run is
# idempotent against the same image
PERSIST_EXPECT = [
    ('new password after reboot', 'ktest@novix:/home/ktest$'),
    ('id after reboot',           'uid=200(ktest) gid=200(ktest)'),
    ('passwd restored',           'passwd: password updated for ktest'),
]

# v0.7.0: run the shell session on 2 CPUs -- sockets=N so SeaBIOS reports
# every processor in the MP table (plain -smp 2 underreports).
# v0.8.0: FAT32 boot disk on IDE primary master + RTC with local time.
def start_qemu(append):
    proc = subprocess.Popen(
        [QEMU, '-kernel', KERNEL, '-m', '128M', '-append', append,
         '-serial', 'stdio', '-display', 'none', '-no-reboot',
         '-smp', '2,sockets=2,cores=1,threads=1',
         '-drive', f'file={DISK},format=raw,if=ide,index=0,media=disk',
         '-rtc', 'base=localtime',
         '-device', 'isa-debug-exit'],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = bytearray()
    reader_done = threading.Event()

    def reader():
        while True:
            b = proc.stdout.read(1)
            if not b:
                break
            out.extend(b)
        reader_done.set()

    threading.Thread(target=reader, daemon=True).start()
    return proc, out, reader_done

def wait_for(out, needle, timeout=120, count=1):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if bytes(out).count(needle) >= count:
            return True
        time.sleep(0.2)
    return False

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

# ---- session 1: the kernel shell (console=debug) ----
proc, out, reader_done = start_qemu('root=/dev/hd0 interactive console=debug')

# gates C-I run before the prompt; the SMP suite (gate G) adds a few seconds
wait_for(out, b'novix>')

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
send_typed('ls /dev')
send_typed('cat /novix.txt')
send_typed('date')
# v0.8.0: write + read back on the FAT32 root (relative path resolves from /)
send_typed('echo disk-persist-line > persist.txt')
send_typed('cat persist.txt')
send_typed('rm persist.txt')
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
    print('!! session1: QEMU did not exit: poweroff never reached')
reader_done.wait(timeout=5)
rc1 = proc.returncode
text1 = bytes(out).decode('utf-8', 'replace')
with open(os.path.join(ROOT, 'build', 'shell_session.log'), 'w') as f:
    f.write(text1)

# ---- session 2: the login world (getty -> login -> sh) ----
proc, out, reader_done = start_qemu('root=/dev/hd0 interactive')
PROMPT = b'ktest@novix:/home/ktest$'

# gates C-I run first, then init respawns getty which prints the banner
wait_for(out, b'login: ')
send_typed('ktest', wait=0.6)                     # username (echoed)
send_typed('wrong-pass', wait=1.2)                # attempt 1: rejected
send_typed('ktest-pass', wait=1.5)                # attempt 2: accepted -> sh
wait_for(out, PROMPT)
send_typed('id')
send_typed('whoami')
send_typed('who')
send_typed('cat /etc/shadow')                     # mortals may not read shadow
# ---- v1.0.0: osh scripting ----
# the ROADMAP acceptance line: pipeline + redirect through /etc/motd
send_typed('cat /etc/motd | grep Novix > /tmp/x', wait=0.8)
send_typed('cat /tmp/x', wait=0.8)
# variable + redirect + pipe + grep, anchored on output text the echoed
# command line cannot contain ('$p' vs its expanded value)
send_typed('p=zz', wait=0.5)
send_typed('echo $p-pipe > /tmp/pv.txt', wait=0.8)
send_typed('cat /tmp/pv.txt', wait=0.8)
send_typed('cat /tmp/pv.txt | grep pipe > /tmp/pv2.txt', wait=0.8)
send_typed('cat /tmp/pv2.txt', wait=0.8)
# $? after a failed command
send_typed('cat /no/such/path', wait=0.8)
send_typed('echo code=$?', wait=0.8)
# comment handling (output text comes from a variable, not the echo'd line)
send_typed('c=cc', wait=0.5)
send_typed('echo $c-comment # gone', wait=0.8)
# background job: the [n] notice prints immediately, wait reaps it
send_typed('hello &', wait=0.8)
wait_for(out, b'[1] pid ')
send_typed('wait', wait=2.0)
# passwd roundtrip: ktest execs the SUID binary and changes its own password
send_typed('passwd')
wait_for(out, b'Current password: ')
send_typed('ktest-pass', wait=0.8)
wait_for(out, b'New password: ')
send_typed('smoke-pw-77', wait=0.8)
wait_for(out, b'Retype new password: ')
send_typed('smoke-pw-77', wait=1.5)
wait_for(out, b'passwd: password updated for ktest')
send_typed('logout', wait=1.5)                    # unwinds to init -> respawn
wait_for(out, b'login: ', count=2)
send_typed('ktest', wait=0.6)
send_typed('ktest-pass', wait=1.2)                # the old password is dead
wait_for(out, b'Login incorrect', count=2)
send_typed('smoke-pw-77', wait=1.5)               # the new one is live
n = bytes(out).count(PROMPT)
wait_for(out, PROMPT, count=n + 1)
send_typed('poweroff', wait=1.0)

try:
    proc.wait(timeout=30)
except subprocess.TimeoutExpired:
    proc.kill()
    print('!! session2: QEMU did not exit: poweroff never reached')
reader_done.wait(timeout=5)
rc2 = proc.returncode
text2 = bytes(out).decode('utf-8', 'replace')
with open(os.path.join(ROOT, 'build', 'shell_login.log'), 'w') as f:
    f.write(text2)

# ---- session 3: fresh boot -- the password change survived poweroff ----
# (/etc/shadow lives on the FAT32 disk; session 3 also restores ktest-pass
# so the run stays idempotent against the same image)
proc, out, reader_done = start_qemu('root=/dev/hd0 interactive')

wait_for(out, b'login: ')
send_typed('ktest', wait=0.6)
send_typed('smoke-pw-77', wait=1.5)               # only the new password works
wait_for(out, PROMPT)
send_typed('id')
send_typed('passwd')                              # restore the build default
wait_for(out, b'Current password: ')
send_typed('smoke-pw-77', wait=0.8)
wait_for(out, b'New password: ')
send_typed('ktest-pass', wait=0.8)
wait_for(out, b'Retype new password: ')
send_typed('ktest-pass', wait=1.5)
wait_for(out, b'passwd: password updated for ktest')
send_typed('poweroff', wait=1.0)

try:
    proc.wait(timeout=30)
except subprocess.TimeoutExpired:
    proc.kill()
    print('!! session3: QEMU did not exit: poweroff never reached')
reader_done.wait(timeout=5)
rc3 = proc.returncode
text3 = bytes(out).decode('utf-8', 'replace')
with open(os.path.join(ROOT, 'build', 'shell_persist.log'), 'w') as f:
    f.write(text3)

# ---- checks ----
def run_checks(text, expect, count_expect):
    failed = 0
    for label, needle in expect:
        ok = needle in text
        print(f"[{'ok' if ok else 'FAIL'}] {label}")
        failed += 0 if ok else 1
    for label, needle, want in count_expect:
        got = text.count(needle)
        ok = got >= want
        print(f"[{'ok' if ok else 'FAIL'}] {label} (x{got}/{want})")
        failed += 0 if ok else 1
    return failed

print('---- session 1 (kernel shell) ----')
print(text1)
print('---- session 2 (login world) ----')
print(text2)
print('---- session 3 (password survives poweroff) ----')
print(text3)
print('---- checks ----')
failed = run_checks(text1, EXPECT, COUNT_EXPECT)
failed += run_checks(text2, LOGIN_EXPECT, LOGIN_COUNT_EXPECT)
failed += run_checks(text3, PERSIST_EXPECT, [])

total = (len(EXPECT) + len(COUNT_EXPECT)
         + len(LOGIN_EXPECT) + len(LOGIN_COUNT_EXPECT)
         + len(PERSIST_EXPECT))
print(f'\nshell test: {total - failed}/{total} checks passed')
sys.exit(0 if failed == 0 and rc1 in (0, 1) and rc2 in (0, 1)
         and rc3 in (0, 1) else 1)
