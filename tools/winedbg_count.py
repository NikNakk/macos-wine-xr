#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Count winedbg breakpoint hits without ever interrupting the debuggee
(killing winedbg kills the process it is attached to).

  winedbg_count.py <wine> <winpid> <stops> <addr>... [-- <cmd on stop>...]

Every address must be hit often (include one hit every frame), so the
debuggee always stops on its own. Commands after -- run at every stop of
the second and later breakpoints.
"""
import re, subprocess, sys, threading, time, collections

wine, pid, stops = sys.argv[1], sys.argv[2], int(sys.argv[3])
rest = sys.argv[4:]
sep = rest.index('--') if '--' in rest else len(rest)
addrs, oncmds = rest[:sep], rest[sep + 1:]

p = subprocess.Popen([wine, 'winedbg'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, text=True, bufsize=1)
out = []
cv = threading.Condition()
def reader():
    for line in p.stdout:
        with cv:
            out.append(line); cv.notify_all()
threading.Thread(target=reader, daemon=True).start()

def send(cmd):
    p.stdin.write(cmd + '\n'); p.stdin.flush()

def wait_for(pattern, start, timeout):
    end = time.time() + timeout
    with cv:
        while time.time() < end:
            for i in range(start, len(out)):
                m = re.search(pattern, out[i])
                if m:
                    return i, m
            cv.wait(0.2)
    return None, None

send(f'attach {pid}')
i, _ = wait_for(r'attached to pid', 0, 15)
if i is None:
    print(''.join(out)); sys.exit('attach failed')
for a in addrs:
    send(f'break *{a}')
time.sleep(1)
hits = collections.Counter()
extra = []
pos = len(out)
for n in range(stops):
    send('cont')
    i, m = wait_for(r'Stopped on breakpoint (\d+)', pos, 20)
    if i is None:
        print('no stop within 20 s; leaving the debugger attached would kill the process, so detaching blindly')
        break
    pos = i + 1
    bp = int(m.group(1))
    hits[bp] += 1
    if bp != 1 and oncmds and hits[bp] <= 3:
        mark = len(out)
        for c in oncmds:
            send(c)
        time.sleep(1.5)
        extra.append(''.join(out[mark:]))
        pos = len(out)
for k in range(len(addrs)):
    send(f'delete {k + 1}')
send('detach'); time.sleep(1)
send('quit')
p.stdin.close()
try:
    p.wait(15)
except subprocess.TimeoutExpired:
    print('winedbg did not exit')
print('hits:', {addrs[k - 1]: v for k, v in sorted(hits.items())})
for e in extra:
    print(e)
