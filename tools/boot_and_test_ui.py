#!/usr/bin/env python3
import time
import sys
import subprocess
import ctypes
from ctypes import wintypes
import winreg

kernel32 = ctypes.windll.kernel32

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
FILE_ATTRIBUTE_NORMAL = 0x80

class COMMTIMEOUTS(ctypes.Structure):
    _fields_ = [
        ("ReadIntervalTimeout", wintypes.DWORD),
        ("ReadTotalTimeoutMultiplier", wintypes.DWORD),
        ("ReadTotalTimeoutConstant", wintypes.DWORD),
        ("WriteTotalTimeoutMultiplier", wintypes.DWORD),
        ("WriteTotalTimeoutConstant", wintypes.DWORD),
    ]

def get_ports():
    ports = []
    try:
        key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM")
        for i in range(256):
            try:
                val, data, _ = winreg.EnumValue(key, i)
                ports.append(data)
            except OSError:
                break
        winreg.CloseKey(key)
    except Exception:
        pass
    return ports

def main():
    print("=" * 60)
    print("  DarwinFire: Automated Boot & UI Launch Test")
    print("=" * 60)

    # 1. Check fastboot and reboot if attached
    res = subprocess.run(["fastboot", "devices"], capture_output=True, text=True)
    if "fastboot" in res.stdout:
        print("[*] Device in fastboot detected. Issuing: fastboot reboot...")
        subprocess.run(["fastboot", "reboot"], capture_output=True, text=True)
    else:
        print("[*] Device not in fastboot. Checking existing serial ports...")

    # 2. Wait for serial port
    print("[*] Waiting for Darwin/XNU CDC-ACM serial port to appear...")
    start_wait = time.time()
    target_port = None
    while time.time() - start_wait < 70:
        candidates = [p for p in get_ports() if p != "COM1"]
        if candidates:
            target_port = candidates[0]
            print(f"[+] Found serial port {target_port} after {time.time() - start_wait:.1f}s!")
            break
        time.sleep(0.5)

    if not target_port:
        print("[-] Timed out waiting for CDC-ACM port.")
        return

    time.sleep(0.5)
    port_name = f"\\\\.\\{target_port}"
    handle = kernel32.CreateFileW(
        port_name,
        GENERIC_READ | GENERIC_WRITE,
        0,
        None,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        None
    )

    if handle == -1 or handle == 0xFFFFFFFFFFFFFFFF:
        err = kernel32.GetLastError()
        print(f"[-] Failed to open {target_port}: WinError {err}")
        return

    timeouts = COMMTIMEOUTS(10, 0, 500, 0, 500)
    kernel32.SetCommTimeouts(handle, ctypes.byref(timeouts))

    buf = ctypes.create_string_buffer(4096)
    bytes_read = wintypes.DWORD()
    bytes_written = wintypes.DWORD()

    print(f"[+] Opened {target_port}. Streaming console output...")
    print("=" * 60)

    sb_launched = False
    captured_text = ""
    silence_count = 0

    try:
        while True:
            success = kernel32.ReadFile(handle, buf, 4096, ctypes.byref(bytes_read), None)
            if success and bytes_read.value > 0:
                chunk = buf.raw[:bytes_read.value].decode("utf-8", errors="replace")
                sys.stdout.write(chunk)
                sys.stdout.flush()
                captured_text += chunk
                silence_count = 0

                # Check if prompt arrived and we haven't launched SpringBoard yet
                if not sb_launched and ("fire#" in captured_text or "dash over USB" in captured_text):
                    print("\n" + "=" * 60)
                    print("[+] Shell prompt detected! Launching SpringBoard in 1 second...")
                    print("=" * 60)
                    time.sleep(1.0)
                    cmd = "/System/Library/CoreServices/SpringBoard.app/SpringBoard &\n".encode("utf-8")
                    kernel32.WriteFile(handle, cmd, len(cmd), ctypes.byref(bytes_written), None)
                    sb_launched = True
            else:
                silence_count += 1
                if silence_count > 60: # 30s silence
                    if sb_launched:
                        print("\n[*] 30s silence after SpringBoard launch.")
                        break
                    elif time.time() - start_wait > 90:
                        print("\n[*] Boot timeout.")
                        break
            time.sleep(0.05)
    except KeyboardInterrupt:
        print("\n[*] Stopped by user.")
    finally:
        kernel32.CloseHandle(handle)

if __name__ == "__main__":
    main()
