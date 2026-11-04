"""Bench: N real BLE disconnect/reconnect cycles on a bench diagnostics image (docs/memory-soak-results.md). Drops the
link with `bench drop` only after it has been up (notifications on) for HOLD seconds; a cycle counts once the phone is
back to `notifications on`. Prints every line received, then a summary. Needs pyserial.
usage: python bench_reconnect.py <port> <cycles> <hold_s> <max_s>"""
import re
import sys
import time

import serial

port, cycles, hold, max_s = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
s = serial.Serial(port, 115200, timeout=0.05, dsrdtr=False, rtscts=False)
s.rts = False
s.dtr = False
t0 = time.time()
up_since = time.time()          # assume linked at start; the first drop waits HOLD anyway
dropped_at = None
done, waits, buf = 0, [], ""
s.write(b"diag\n")
while done < cycles and time.time() - t0 < max_s:
    buf += s.read(4096).decode("latin1")
    while "\n" in buf:
        line, buf = buf.split("\n", 1)
        print(line, flush=True)
        if re.search(r"WardriveESP: notifications on", line):
            if dropped_at is not None:
                waits.append(round(time.time() - dropped_at, 1))
                done += 1
                dropped_at = None
            up_since = time.time()
        elif re.search(r"WardriveESP: phone disconnected", line) and dropped_at is None:
            up_since = None          # dropped by someone else: wait for it to come back before counting
    if dropped_at is None and up_since is not None and time.time() - up_since >= hold:
        s.write(b"bench drop\n")
        print(">>> bench drop (t=%.1f)" % (time.time() - t0), flush=True)
        dropped_at = time.time()
        up_since = None
s.write(b"diag\n")
end = time.time() + 2
while time.time() < end:
    sys.stdout.write(s.read(4096).decode("latin1"))
print("\nSUMMARY cycles=%d of %d in %.0f s; seconds from drop to notifications on: %s" % (done, cycles, time.time() - t0, waits))
