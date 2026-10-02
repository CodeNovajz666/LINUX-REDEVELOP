#!/usr/bin/env python3
"""One-shot check: does `python build.py run` mode actually exit on poweroff?
Boots with the same QEMU flags as cmd_run, waits for the prompt, sends
"poweroff", and reports the exit code."""
import os, sys, subprocess, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(ROOT, 'build', 'kernel.elf')
QEMU = os.environ.get('QEMU') or os.path.join(
    ROOT, '..', 'tools', 'qemu-pkg', 'qemu-system-x86_64.exe')

proc = subprocess.Popen(
    [QEMU, '-kernel', KERNEL, '-m', '128M', '-append', 'interactive',
     '-serial', 'stdio', '-display', 'none', '-device', 'isa-debug-exit'],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

out = bytearray()
def reader():
    while True:
        b = proc.stdout.read(1)
        if not b:
            break
        out.extend(b)
threading.Thread(target=reader, daemon=True).start()

deadline = time.time() + 60
while time.time() < deadline and b'novix>' not in bytes(out):
    time.sleep(0.2)

proc.stdin.write(b'poweroff\n')
proc.stdin.flush()
try:
    rc = proc.wait(timeout=20)
except subprocess.TimeoutExpired:
    proc.kill()
    rc = 'STILL RUNNING'

text = out.decode('utf-8', 'replace')
print('bye. seen:', 'bye.' in text)
print('exit code:', rc)
sys.exit(0 if rc in (0, 1) and 'bye.' in text else 1)
