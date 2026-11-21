#!/usr/bin/env python3
import serial, sys, time

dur = int(sys.argv[1]) if len(sys.argv) > 1 else 540
ser = None
for _ in range(180):
    try:
        ser = serial.Serial('COM6', 115200, timeout=0.2, write_timeout=5.0)
        break
    except Exception:
        time.sleep(0.5)
if ser is None:
    raise RuntimeError('COM6 not available')

print('[+] Connected to COM6', flush=True)
t0 = time.time()
saw_sel50 = None
sent_diag = False
saw_nonzero_swap = False
buf = ''
KEYS = (
    'Swap', 'mfb:', 'mfb sel', 'ios:', 'ios sel', 'SpringBoard', 'backboardd',
    'EXC', 'ABORT', 'SEGV', 'BRK', 'panic', 'PANIC', 'DF_PUTS', 'EL1', 'SERROR',
    'caller 0x', 'FIRE:', 'trap', 'killing', 'exited', 'GUIHANG',
    'posterboard', 'SystemSound', 'TextInput', 'lsd', 'emmc:',
    'iconservices', 'securityd', 'Thread 0x', 'CoverSheet', 'SpringBoardHome',
    'UIKitCore', 'QuartzCore', 'Footprint', 'Process:', 'ManagedConfiguration',
    'MobileKeyBag', 'BackBoard', 'FrontBoard', 'Spindump', 'csstore'
)

while time.time() - t0 < dur:
    n = ser.in_waiting
    if n:
        c = ser.read(n)
        s = c.decode('utf-8', 'replace')
        buf += s
        while '\n' in buf:
            line, buf = buf.split('\n', 1)
            line = line.strip('\r')
            el = time.time() - t0
            if 'mfb sel 50' in line and saw_sel50 is None:
                saw_sel50 = el
            if 'SwapEnd' in line and 'nz=0 ' not in line:
                saw_nonzero_swap = True
            if (saw_sel50 is not None and line.strip()) or any(k in line for k in KEYS):
                print(f'[{el:5.1f}s] {line[:220]}', flush=True)
    else:
        time.sleep(0.05)

ser.close()
