#!/usr/bin/env python3
"""CI contract check: docs/syscall.md's table must match the SYS_* defines
in kernel/include/syscall.h one-to-one (number and name). ROADMAP v1.0.0
acceptance #4 -- "CI 校验系统调用编号表"."""
import re, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def header_calls():
    text = open(os.path.join(ROOT, 'kernel', 'include', 'syscall.h')).read()
    return {int(num): name for name, num in
            re.findall(r'^#define SYS_([A-Z0-9_]+)\s+(\d+)\s', text, re.M)}

def doc_calls():
    text = open(os.path.join(ROOT, 'docs', 'syscall.md'), encoding='utf-8').read()
    calls = {}
    for line in text.splitlines():
        m = re.match(r'^\|\s*(\d+)\s*\|\s*([A-Z0-9_]+)\s*\|', line)
        if m:
            num, name = int(m.group(1)), m.group(2)
            if num in calls:
                sys.exit('syscall.md: duplicate number %d' % num)
            calls[num] = name
    return calls

def main():
    hdr, doc = header_calls(), doc_calls()
    missing_doc = sorted(set(hdr) - set(doc))
    extra_doc = sorted(set(doc) - set(hdr))
    bad = [(n, hdr[n], doc[n]) for n in sorted(set(hdr) & set(doc))
           if hdr[n] != doc[n]]
    if missing_doc or extra_doc or bad:
        if missing_doc: print('missing in docs/syscall.md:', missing_doc)
        if extra_doc:   print('not in syscall.h:', extra_doc)
        for n, h, d in bad:
            print('name mismatch for %d: header=%s doc=%s' % (n, h, d))
        sys.exit(1)
    print('syscall doc check: %d/%d entries match kernel/include/syscall.h'
          % (len(hdr), len(hdr)))

if __name__ == '__main__':
    main()
