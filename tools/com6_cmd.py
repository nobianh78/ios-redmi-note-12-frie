#!/usr/bin/env python3
import serial, sys, time

cmd = sys.argv[1]
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0
ser = serial.Serial('COM6', 115200, timeout=0.5, write_timeout=5.0)
time.sleep(0.2)
ser.write(('\n' + cmd + '\n').encode('utf-8'))
t0 = time.time()
out = b''
while time.time() - t0 < dur:
    n = ser.in_waiting
    if n:
        out += ser.read(n)
    else:
        time.sleep(0.1)
ser.close()
print(out.decode('utf-8', 'replace'))
