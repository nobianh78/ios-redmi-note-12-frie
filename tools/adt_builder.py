#!/usr/bin/env python3

"""
Apple Device Tree (ADT / EDT) Generator for MediaTek MT6769H (Redmi 12)
Constructs an authentic Apple-formatted binary Device Tree required by XNU arm_init()
"""

import sys
import os
import struct

class ADTNode:
    def __init__(self, name=""):
        self.properties = []
        self.children = []
        if name:
            self.add_property("name", name.encode("ascii") + b"\x00")

    def add_property(self, name: str, value: bytes):
        self.properties.append((name, value))

    def add_child(self, child_node):
        self.children.append(child_node)

    def serialize(self) -> bytes:
        out = bytearray()
        
        # Header: uint32 nProperties, uint32 nChildren
        out.extend(struct.pack("<II", len(self.properties), len(self.children)))

        # Serialize Properties
        for prop_name, prop_val in self.properties:
            # Name: fixed 32 bytes, null padded
            name_bytes = prop_name.encode("ascii")[:31].ljust(32, b"\x00")
            val_len = len(prop_val)
            out.extend(name_bytes)
            out.extend(struct.pack("<I", val_len))
            out.extend(prop_val)
            # Pad to 4-byte alignment
            rem = val_len % 4
            if rem != 0:
                out.extend(b"\x00" * (4 - rem))

        # Serialize Children recursively
        for child in self.children:
            out.extend(child.serialize())

        return bytes(out)

def build_fire_adt() -> bytes:
    """
    Constructs the Apple Device Tree specifically tailored for
    Xiaomi Redmi 12 ("fire" / MediaTek Helio G88 MT6769H)
    """
    root = ADTNode()
    root.add_property("name", b"device-tree\x00")
    root.add_property("model", b"Xiaomi Redmi 12 (fire)\x00")
    root.add_property("compatible", b"MT6769H\x00arm64\x00")
    root.add_property("#address-cells", struct.pack("<I", 2))
    root.add_property("#size-cells", struct.pack("<I", 2))

    # 1. /chosen node
    chosen = ADTNode("chosen")
    boot_args_str = b"debug=0x14e serial=2 amfi_get_out_of_my_way=1 cs_enforcement_disable=1\x00"
    chosen.add_property("boot-args", boot_args_str)
    chosen.add_property("firmware-version", b"DarwinFire-v0.1-fire\x00")
    chosen.add_property("random-seed", os.urandom(32))
    
    # Machine timeouts required by XNU
    chosen.add_property("machine-timeouts", struct.pack("<I", 0))
    root.add_child(chosen)

    # 2. /memory node (MediaTek 4GB DRAM at 0x40000000)
    memory = ADTNode("memory")
    # reg: physical_base (0x40000000), size (0x100000000 = 4GB)
    reg_val = struct.pack(">QQ", 0x40000000, 0x100000000)
    memory.add_property("reg", reg_val)
    memory.add_property("device_type", b"memory\x00")
    root.add_child(memory)

    # 3. /cpus node (DynamIQ: 2x Cortex-A75 + 6x Cortex-A55)
    cpus = ADTNode("cpus")
    cpus.add_property("#address-cells", struct.pack("<I", 1))
    cpus.add_property("#size-cells", struct.pack("<I", 0))
    
    # Core 0 (Boot CPU)
    cpu0 = ADTNode("cpu@0")
    cpu0.add_property("device_type", b"cpu\x00")
    cpu0.add_property("reg", struct.pack("<I", 0))
    cpu0.add_property("compatible", b"arm,cortex-a75\x00")
    cpu0.add_property("clock-frequency", struct.pack("<I", 2000000000)) # 2.0 GHz
    cpus.add_child(cpu0)
    root.add_child(cpus)

    # 4. /arm-io node (MediaTek MT6769 MMIO Devices)
    arm_io = ADTNode("arm-io")
    arm_io.add_property("compatible", b"arm-io\x00")

    # MTK High-Speed UART (Serial Console)
    uart = ADTNode("uart0")
    uart.add_property("compatible", b"mediatek,mt6769-uart\x00")
    uart.add_property("reg", struct.pack(">QQ", 0x11002000, 0x1000))
    uart.add_property("interrupts", struct.pack("<I", 91))
    arm_io.add_child(uart)

    # ARM GICv2 (Generic Interrupt Controller)
    gic = ADTNode("aic") # Hooked as AIC so XNU recognizes it
    gic.add_property("compatible", b"arm,gic-400\x00")
    gic.add_property("reg", struct.pack(">QQQQ", 0x0C000000, 0x1000, 0x0C002000, 0x2000))
    arm_io.add_child(gic)

    root.add_child(arm_io)

    return root.serialize()

def main():
    print("============================================================")
    print("  DARWIN-FIRE APPLE DEVICE TREE (ADT) BUILDER")
    print("  Target: Xiaomi Redmi 12 (fire / MT6769H)")
    print("============================================================\n")

    adt_bytes = build_fire_adt()
    out_path = "extracted_binaries/fire_devicetree.adt"

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(adt_bytes)

    print(f"[+] Successfully generated Apple Device Tree binary:")
    print(f"    Path: {os.path.abspath(out_path)}")
    print(f"    Size: {len(adt_bytes)} bytes")
    print(f"    Format: Recursive Packed Apple DeviceTreeNode\n")

if __name__ == "__main__":
    main()
