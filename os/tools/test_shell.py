#!/usr/bin/env python3
"""Drive the Novix interactive shell over QEMU's -serial stdio.

Three sessions (v1.0.0):
  session 1 -- console=debug boots straight into the kernel shell: the
               full command battery (v0.2.0 .. v0.8.0 regression).
  session 2 -- the default boot of a FRESH image: getty finds zero
               accounts and runs first-boot registration (root password,
               then the "ktest" user with password "ktest-pass"), which
               exits back into the respawn and the login world:
               wrong password rejected, shadow unreadable, passwd
               roundtrip, logout -> getty respawn, old password dead.
               (The shadow-only "kck" anchor keeps the shadow_roundtrip
               ktest byte-exact on this same image -- see mkfatimg.py.)
  session 3 -- fresh boot: the registered accounts survive poweroff
               (/etc lives on the FAT32 disk) and the registration wizard
               does NOT re-run -- straight to the login prompt, and only
               the password changed in session 2 works.
The disk image is regenerated at startup: session 2 must see the
zero-account first boot, and a previous run left registered accounts on
the disk (they survive poweroff -- that is the point).
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

# ---- regenerate a fresh zero-account disk image ----
# The v1.0.0 out-of-box image ships no accounts: session 2 must land in
# the first-boot registration wizard. A previous run of this script left
# registered accounts on the disk (they survive poweroff -- that is the
# feature under test), so rebuild the image from build/user (mkfatimg
# needs no compiler; it takes seconds).
_user_dir = os.path.join(ROOT, 'build', 'user')
if not os.path.exists(KERNEL) or not os.path.isdir(_user_dir):
    sys.exit('build/kernel.elf or build/user missing -- run `python build.py` first')
_progs = sorted(f for f in os.listdir(_user_dir) if '.' not in f)
_mk = os.path.join(ROOT, 'tools', 'mkfatimg.py')
_pairs = ['/bin/%s=%s' % (p, os.path.join(_user_dir, p)) for p in _progs]
subprocess.run([sys.executable, _mk, DISK] + _pairs, check=True)
print('fresh zero-account image: %s' % DISK)

EXPECT = [
    ('help',        'commands:'),
    ('ps',          'idle'),
    ('spawn',       'spawned pid'),
    ('spawner out', 'all children reaped'),
    ('fault_div',   'killed by sig8'),
    ('fault_segv',  'killed by SIGSEGV'),
    ('stackovf guard kill', '(stackovf): page fault'),   # v1.1.4
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
    # v1.1.1 line editing: the follow-up output proves the kill keys worked
    ('ctrl-u rerun', 'cu-ok-7'),
]

# v1.1.1: exactly-N checks -- the Ctrl-U/Ctrl-W victim text may appear only
# as the typing echo (never as a command's output or a later reprint)
EXACT_EXPECT = [
    ('ctrl-u killed line', 'cu-line-9', 1),
    ('ctrl-w killed word', 'kw-victim', 1),
]

# checks that need N occurrences, not just one
COUNT_EXPECT = [
    # history recall: the command is echoed once while typed and once more
    # when Up redraws it (then re-runs)
    ('history recall', 'echo hist-recall-9', 2),
    # Tab-completed 'spawn forke<Tab>' re-runs forker: once from the ktest
    # cow_isolation case, once from the shell session
    ('tab forker rerun', 'COW-ISOLATION-OK', 2),
    # v1.1.1: Home arrives pre-parsed and is a no-op for this append-only
    # editor, so 'da' Home 'te' spells date -- a second wall-clock line
    ('home no-op + date', ' local (unix ', 2),
]

# session 1: text that must NOT appear anywhere in the log
ABSENT_EXPECT = [
    # v1.0.1 (F3): the kernel shell reaps exited `spawn` children before
    # each prompt and again before each dispatched command, so no `ps` in
    # the session can ever catch a ZOMBIE row. Before the fix every
    # exited child stayed a zombie forever, holding its slot and pid.
    ('no zombie rows in ps', 'ZOMBIE'),
]

# session 2 (v1.0.0): first-boot registration, then the login world
LOGIN_EXPECT = [
    # the registration wizard (fresh image: zero accounts in /etc/passwd)
    ('firstboot banner',     'no user accounts exist yet'),
    ('register root prompt', 'New password for root: '),
    ('register root set',    'register: root password set'),
    ('register name prompt', 'Choose a username (empty for a root-only system): '),
    ('register user pw',     'New password: '),
    ('register user created', "register: account 'ktest' created (uid 100, home /home/ktest)"),
    ('register done',        'Registration complete. Please log in.'),
    # then the normal login flow
    ('login issue',        'authorized users only'),
    ('login prompt',       'login: '),
    ('password prompt',    'Password: '),
    ('bad password',       'Login incorrect'),
    ('login shell prompt', 'ktest@novix:/home/ktest$'),
    ('id shows ktest',     'uid=100(ktest) gid=100(ktest)'),
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
    # v1.0.0: osh debug reports (syscalls 39-41 -> ps/cpus/mem/upt/kill)
    ('osh ps header',      'PID   PPID  STATE'),
    ('osh ps shell row',   'osh\r\n'),      # getty exec'd login exec'd osh
    ('osh cpus header',    'CPU  APIC  ONLINE'),
    ('osh mem lines',      'pages free'),
    ('osh upt',            'uptime: '),
    ('osh kill no task',   'kill: no such task (pid 999)'),
    # v1.0.1: strict parse (pid 0 is not a pid, rejected client-side) and
    # the uid check in sys_kill: ktest (uid 100) may not signal kswapd
    # (pid 2, a uid-0 kernel thread -- before the check this killed it)
    ('osh kill bad pid',   "kill: bad pid '0'"),
    ('osh kill mortal guard', 'kill: no such task (pid 2)'),
    # v1.1.0: interactive toolbox (SYS_GETDENTS/UNLINK/DATETIME + TCCLEAR)
    ('osh ls dir tag',       '<dir>'),          # `ls /` entry-type tags
    ('osh pwd home',         '/home/ktest\r\n'), # login home via SYS_GETCWD
    ('osh cd tmp prompt',    '@novix:/tmp$'),    # the prompt tracks the cwd
    ('osh touch ls',         'tb0.txt'),        # created, then listed
    ('osh cp+mv roundtrip',  'toolbox-line'),   # cp wrote it, mv moved it, cat
                                                # proves the content travelled
    ('osh rm missing',       'rm: no-such-file: failed'),
    # v1.1.2: rename fast path + rmdir refusal (empty-dir removal is proven
    # by the exact counts: rd1/rd2 must appear only in the echoed commands)
    ('osh rmdir nonempty',   'rmdir: rd2: directory not empty'),
    # v1.1.5: export reaches children through envp; unexported stays local;
    # breaking PATH blinds external commands, restoring finds them again
    ('osh export in env',    'GR=hi-env-9'),
    ('osh PATH blind',       'pwd: not found'),
    # v1.1.6: the text toolbox -- deterministic outputs on a 2-line file
    ('osh wc lines',         '2 /tmp/hh.txt'),
    ('osh which',            '/bin/which'),
    ('osh find',             '/tmp/fd9/needle-fx'),
    # v1.1.8: procfs answers with live kernel numbers
    ('osh procfs meminfo',   'pages_free '),
    ('osh procfs cpuinfo',   'ncpu 2'),
    ('osh procfs version',   'x86_64 -- a real OS'),
    ('osh date local',       'local (unix'),    # the RTC line, kernel-formatted
    ('osh uname',            'Novix 1.1.8'),    # uname and the banners agree
                                                # on the version again (v1.1.0)
    # v1.1.1: the history ring numbers entries chronologically and retires
    # the oldest past 32 -- the listing shows the raw (pre-expansion) lines
    # (two-space indent: prompt lines end in '$ ', never match)
    ('osh history',          '  echo osh-edit-a'),
    # v1.1.1 editor: Ctrl-U ran `date` instead of the cancelled line (the
    # second RTC line of this session)
    ('osh ctrl-u clear',     ' local (unix '),
]
LOGIN_COUNT_EXPECT = [
    # register's own retype prompt, then passwd's roundtrip one
    ('register retypes', 'Retype new password: ', 3),
    # logout unwinds the getty->login->sh chain back to init, which
    # respawns getty: the banner prints a second time
    ('getty respawn', 'authorized users only', 2),
    # wrong password at first login + the dead old password after the
    # passwd roundtrip
    ('old password dead', 'Login incorrect', 2),
    # v1.0.0: 'cat /etc/motd | grep Novix > /tmp/x' + cat -- the needle
    # hits the boot banner (1), motd at each of the two logins (2) and the
    # redirected copy (1)
    ('osh motd pipeline', 'Novix 1.1.8 -- a real OS', 4),
    # variable + redirect + pipe + grep + cat: two cats of derived files;
    # the echoed command lines only ever contain '$p-pipe', never the value
    ('osh var+pipe+redirect', 'zz-pipe', 2),
    # v1.1.1: typing echo + output + Up-Up redraw + rerun output + the two
    # `history` listings' entries = at least 5 occurrences of osh-edit-a
    ('osh up-up recall + history', 'osh-edit-a', 5),
    # Home + 'echo ' insert: typing echo + the command's output
    ('osh home insert', 'osh-mid-9', 2),
    # v1.1.2: rename same-dir + two cats prove the content followed the
    # inode (fast path) and then the copy (cross-instance fallback)
    ('osh rename content', 'rename-line-9', 3),
]
LOGIN_EXACT_EXPECT = [
    # v1.1.1: killed text appears only as the typing echo -- never rerun
    ('osh ctrl-u killed line', 'cu2-cancel-me', 1),
    ('osh ctrl-w killed word', 'osh-kw-victim', 1),
    # v1.1.2: the rmdir'd directories appear ONLY in the echoed commands
    # (mkdir/rmdir/touch/rm lines) -- never in the `ls` output that follows
    # echoes + the two `history` listings; never the `ls` output
    ('osh rmdir empty dir', 'rd1', 6),
    ('osh rmdir emptied dir', 'rd2', 16),   # +1: the not-empty refusal
    # v1.1.5: the unexported variable appears only in its assignment echo
    ('osh unexported stays local', 'local-only-1', 1),
    # v1.1.6: head/tail pick their exact line; cut fields its token
    ('osh head line', 'hh-line-1', 2),
    ('osh tail line', 'hh-line-2', 2),
    ('osh cut field', 'yy-2', 2),
]

# session 3 (v1.0.0): fresh boot -- the accounts registered in session 2
# survived poweroff (/etc lives on the FAT32 disk); the wizard must not
# re-run and only the password changed in session 2 works
PERSIST_EXPECT = [
    ('new password after reboot', 'ktest@novix:/home/ktest$'),
    ('id after reboot',           'uid=100(ktest) gid=100(ktest)'),
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
# v1.1.0: event-driven sequencing -- a burst-during-burst race was observed
# once (the exception reports plus spawner's children flooded the guest
# serial while these commands were typed: 'kill 999' arrived as 'k...' and
# six needles lost their text). Wait for each fault's report instead of
# trusting a fixed sleep.
wait_for(out, b'killed by sig8')
send_typed('spawn fault_segv', wait=1.0)
wait_for(out, b'killed by SIGSEGV')
# v1.1.4: unbounded recursion dies on the stack guard page -- the report
# names the program, so the needle is distinct from fault_segv's kill
send_typed('spawn stackovf', wait=1.5)
wait_for(out, b'(stackovf): page fault')
send_typed('kill 999', wait=0.8)
send_typed('kill 0', wait=0.8)
send_typed('kill 1', wait=0.8)
send_typed('policy 1', wait=0.8)
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
# v1.1.1: Ctrl-U kills the line, Ctrl-W kills the last word. Each victim
# string must stay in the log EXACTLY once (the typing echo) -- the follow-up
# commands' outputs prove the line really ran without the killed text.
send_typed('echo cu-line-9', enter=False, wait=0.2)
send('\x15', wait=0.3)                             # Ctrl-U: kill line
send_typed('echo cu-ok-7')
send_typed('echo kw-victim', enter=False, wait=0.2)
send('\x17', wait=0.3)                             # Ctrl-W: kill word
send('\n', wait=0.5)                               # bare echo: empty output
# Home is pre-parsed by the input layer; this editor is append-only, so the
# sequence 'da' Home 'te' must still spell 'date'
send_typed('da', enter=False, wait=0.2)
send('\x1b[H', wait=0.3)
send_typed('te', wait=0.5)
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

# ---- session 2: first-boot registration, then the login world ----
proc, out, reader_done = start_qemu('root=/dev/hd0 interactive')
PROMPT = b'ktest@novix:/home/ktest$'

# gates C-J run first, then init respawns getty, which finds the empty
# /etc/passwd and execs the registration wizard instead of login
wait_for(out, b'no user accounts exist yet')
wait_for(out, b'New password for root: ')
send_typed('root-smoke-1', wait=0.8)              # step 1: root's password
wait_for(out, b'Retype new password: ')
send_typed('root-smoke-1', wait=0.8)
wait_for(out, b'Choose a username (empty for a root-only system): ')
send_typed('ktest', wait=0.6)                     # step 2: the first user
wait_for(out, b'New password: ')                  # not the root prompt: it
send_typed('ktest-pass', wait=0.8)                # has " for root" in between
wait_for(out, b'Retype new password: ', count=2)
send_typed('ktest-pass', wait=1.2)
wait_for(out, b'Registration complete. Please log in.')

# the wizard exited -> init respawns getty -> the issue banner + login
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
# ---- v1.0.0: osh debug reports (kernel syscalls 39-41) ----
send_typed('ps', wait=0.8)
send_typed('cpus', wait=0.8)
send_typed('mem', wait=0.8)
send_typed('upt', wait=0.8)
send_typed('kill 999', wait=0.8)
send_typed('kill 0', wait=0.8)
send_typed('kill 2', wait=0.8)
# ---- v1.1.0: interactive toolbox (/bin tools over the new syscalls) ----
send_typed('ls /', wait=0.8)                 # <dir> tags via SYS_GETDENTS
send_typed('pwd', wait=0.5)                  # /home/ktest: login's chdir
send_typed('cd /tmp', wait=0.5)
send_typed('touch tb0.txt', wait=0.5)        # relative paths land in the cwd
send_typed('echo toolbox-line > tb1.txt', wait=0.8)
send_typed('cp tb1.txt tb2.txt', wait=0.8)
send_typed('mv tb2.txt tb3.txt', wait=0.8)
send_typed('cat tb3.txt', wait=0.8)          # toolbox-line: cp+mv delivered
send_typed('rm tb0.txt', wait=0.5)
send_typed('rm no-such-file', wait=0.5)      # the error path reads cleanly
# ---- v1.1.2: rename(2)/rmdir(2) ----
# mv inside one directory is rename(2) now (same inode, no copy); across
# instances (/tmp ramfs pin <-> FAT root) it falls back to copy+unlink
send_typed('echo rename-line-9 > rn1.txt', wait=0.8)
send_typed('mv rn1.txt rn2.txt', wait=0.8)        # same dir -> RENAME fast path
send_typed('cat rn2.txt', wait=0.5)
send_typed('mv rn2.txt /home/ktest/rn3.txt', wait=0.8)   # cross-instance
send_typed('cat /home/ktest/rn3.txt', wait=0.5)
send_typed('rm /home/ktest/rn3.txt', wait=0.5)
send_typed('mkdir rd1', wait=0.5)
send_typed('rmdir rd1', wait=0.5)                 # empty dir: gone
send_typed('mkdir rd2', wait=0.5)
send_typed('touch rd2/inside.txt', wait=0.5)
send_typed('rmdir rd2', wait=0.5)                 # not empty: clean refusal
send_typed('rm rd2/inside.txt', wait=0.5)
send_typed('rmdir rd2', wait=0.5)                 # now empty: gone
send_typed('ls', wait=0.8)                   # default listing: the cwd (/tmp)
send_typed('date', wait=0.5)
send_typed('uname', wait=0.5)
send_typed('sleep 1', wait=2.0)
send_typed('clear', wait=0.5)                # ioctl TCCLEAR: no serial bytes,
                                             # the next command proves survival
send_typed('history', wait=0.8)
# ---- v1.1.1: the editor's interactive recall and kill keys ----
# Up Up recalls the SECOND-newest raw line ('echo osh-edit-a') and re-runs it
send_typed('echo osh-edit-a')
send_typed('echo osh-edit-b')
send('\x1b[A', wait=0.3); send('\x1b[A', wait=0.4)
send('\n', wait=0.8)
# Home then 'echo ' inserts the command at line start (mid-line insert)
send_typed('osh-mid-9', enter=False, wait=0.2)
send('\x1b[H', wait=0.3)
send_typed('echo ', enter=False, wait=0.4)
send('\n', wait=0.6)
# Ctrl-U kills the half-typed line; date runs instead
send_typed('echo cu2-cancel-me', enter=False, wait=0.2)
send('\x15', wait=0.3)
send_typed('date')
# Ctrl-W kills the trailing word; a bare echo runs
send_typed('echo osh-kw-victim', enter=False, wait=0.2)
send('\x17', wait=0.3)
send('\n', wait=0.6)
send_typed('history', wait=0.8)               # raw lines survived the editing
# ---- v1.1.5: environment variables ----
send_typed('export GR=hi-env-9', wait=0.5)    # set + export in one word
send_typed('env | grep GR', wait=0.8)         # the child saw the variable
send_typed('echo $GR', wait=0.5)
send_typed('LOCl=local-only-1', wait=0.5)     # unexported: stays shell-local
send_typed('env | grep LOCl', wait=0.6)       # grep finds nothing (exit 1)
send_typed('echo $?')                         # ... and says so via $?
send_typed('export PATH=/no-such-dir', wait=0.5)
send_typed('pwd', wait=0.6)                   # PATH-blind: external tool not found
send_typed('export PATH=/bin', wait=0.5)      # restore
send_typed('pwd', wait=0.5)                   # found again
# ---- v1.1.6: the text toolbox (pure userland, existing syscalls) ----
send_typed('echo hh-line-1 > /tmp/hh.txt', wait=0.6)
send_typed('echo hh-line-2 >> /tmp/hh.txt', wait=0.6)
send_typed('wc -l /tmp/hh.txt', wait=0.5)
send_typed('head -n 1 /tmp/hh.txt', wait=0.5)
send_typed('tail -n 1 /tmp/hh.txt', wait=0.5)
send_typed('which ls which', wait=0.5)        # both resolve under /bin
send_typed('mkdir fd9', wait=0.5)
send_typed('touch fd9/needle-fx', wait=0.5)
send_typed('find /tmp -name needle-fx', wait=0.8)
send_typed('echo xx-1:yy-2:zz-3 | cut -d : -f 2', wait=0.8)
send_typed('rm fd9/needle-fx', wait=0.5)
send_typed('rmdir fd9', wait=0.5)
send_typed('rm /tmp/hh.txt', wait=0.5)
# ---- v1.1.8: the read-only /proc (generated on read) ----
send_typed('cat /proc/meminfo', wait=0.5)
send_typed('cat /proc/cpuinfo', wait=0.5)
send_typed('cat /proc/version', wait=0.5)
send_typed('cd /home/ktest', wait=0.6)       # home again for passwd/logout
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

# ---- session 3: fresh boot -- the registration survived poweroff ----
# (/etc/passwd and /etc/shadow live on the FAT32 disk; the wizard must NOT
# re-run -- waiting for the bare login prompt is itself that assertion)
proc, out, reader_done = start_qemu('root=/dev/hd0 interactive')

wait_for(out, b'login: ')
send_typed('ktest', wait=0.6)
send_typed('smoke-pw-77', wait=1.5)               # only the new password works
wait_for(out, PROMPT)
send_typed('id')
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
def run_checks(text, expect, count_expect, absent=(), exact=()):
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
    for label, needle in absent:
        ok = needle not in text
        print(f"[{'ok' if ok else 'FAIL'}] {label}")
        failed += 0 if ok else 1
    for label, needle, want in exact:            # v1.1.1: exactly-N
        got = text.count(needle)
        ok = got == want
        print(f"[{'ok' if ok else 'FAIL'}] {label} (x{got}, want exactly {want})")
        failed += 0 if ok else 1
    return failed

print('---- session 1 (kernel shell) ----')
print(text1)
print('---- session 2 (login world) ----')
print(text2)
print('---- session 3 (password survives poweroff) ----')
print(text3)
print('---- checks ----')
failed = run_checks(text1, EXPECT, COUNT_EXPECT, ABSENT_EXPECT, EXACT_EXPECT)
failed += run_checks(text2, LOGIN_EXPECT, LOGIN_COUNT_EXPECT, (),
                     LOGIN_EXACT_EXPECT)
failed += run_checks(text3, PERSIST_EXPECT, [])

total = (len(EXPECT) + len(COUNT_EXPECT) + len(ABSENT_EXPECT) + len(EXACT_EXPECT)
         + len(LOGIN_EXPECT) + len(LOGIN_COUNT_EXPECT) + len(LOGIN_EXACT_EXPECT)
         + len(PERSIST_EXPECT))
print(f'\nshell test: {total - failed}/{total} checks passed')
sys.exit(0 if failed == 0 and rc1 in (0, 1) and rc2 in (0, 1)
         and rc3 in (0, 1) else 1)
