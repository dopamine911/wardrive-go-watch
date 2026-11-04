"""Bench: read the watch's USB port, optionally reset it first, and send timed lines without opening a session (the
bench diagnostics' `diag ...` / `bench ...` commands; docs/memory-baseline.md). Needs pyserial.
usage: python bench_serial.py <port> boot|log <seconds> [t:command ...]   e.g. 10:diag 65:"diag dump"
Prints everything received; lines it sent are echoed as '>>> command' with the time. Never opens the port with RTS
high and DTR low, which resets the chip ("boot" pulses RTS on purpose)."""
import sys
import time

import serial

port, mode, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
plan = []
for a in sys.argv[4:]:
    t, c = a.split(":", 1)
    plan.append((float(t), c))
plan.sort()
s = serial.Serial(port, 115200, timeout=0.05, dsrdtr=False, rtscts=False)
s.rts = False   # RTS before DTR: never RTS=1 with DTR=0 (that resets the chip)
s.dtr = False
if mode == "boot":
    s.rts = True
    time.sleep(0.2)
    s.rts = False
out = sys.stdout
t0 = time.time()
while time.time() - t0 < secs:
    data = s.read(4096)
    if data:
        out.write(data.decode("latin1"))
        out.flush()
    while plan and time.time() - t0 >= plan[0][0]:
        c = plan.pop(0)[1]
        s.write((c + "\n").encode())
        out.write("\n>>> %s (t=%.1f)\n" % (c, time.time() - t0))
        out.flush()
