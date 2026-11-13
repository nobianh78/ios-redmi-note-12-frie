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

def u32(v): return struct.pack("<I", v)
def u64(*vs): return b"".join(struct.pack("<Q", v) for v in vs)
def cstr(s): return s.encode("ascii") + b"\x00"


def build_fire_adt() -> bytes:
    """
    Minimal Apple Device Tree for the open-source XNU (xnu-8796, VMAPPLE config) on the
    Redmi 12 "fire" (MT6768, boot CPU Cortex-A55 r1p0, MPIDR 0x81000000, 13 MHz arch timer).
    All integers are little-endian, as XNU reads them in place.

    Only what XNU dereferences early is here (verified in the xnu-8796 sources):
      - pe_arm_get_soc_base_phys(): /arm-io "device_type" and "ranges" are read WITHOUT error
        checks, then *(ranges + 1) is used as the SoC base -> both are mandatory.
      - pe_identify_machine(): /cpus children with "state" == "running" give timebase/clock freq.
      - ml_parse_cpu_topology(): every /cpus child needs "reg" (panics otherwise); the boot CPU is
        the one whose "state" is "running".
      - early_random() (first call in arm_vm_init): /chosen "random-seed", 64 bytes.
    Interrupt controller / timer nodes are deliberately absent until XNU gets that far.
    """
    root = ADTNode("device-tree")
    root.add_property("model", cstr("Redmi 12 (fire)"))
    root.add_property("compatible", cstr("fire") + cstr("arm64"))
    root.add_property("target-type", cstr("fire"))
    root.add_property("#address-cells", u32(2))
    root.add_property("#size-cells", u32(2))

    chosen = ADTNode("chosen")
    chosen.add_property("firmware-version", cstr("DarwinFire heartbeat loader"))
    chosen.add_property("debug-enabled", u32(1))
    # early_random() -> bootseed_init_bootloader() panics unless PE_get_random_seed() returns
    # 64 bytes from /chosen "random-seed" (xnu-8796 osfmk/prng/prng_random.c). Fresh per build.
    chosen.add_property("random-seed", os.urandom(64))
    # arm_init() panics without these (osfmk/arm/arm_init.c, read as unsigned long). MT6768 DRAM
    # starts at 0x40000000; report 256MB, which covers the 128MB the loader hands to XNU.
    chosen.add_property("dram-base", u64(0x40000000))
    chosen.add_property("dram-size", u64(0x10000000))
    # Root device: IOFindBSDRoot turns /chosen/memory-map "RAMDisk" <base size> into /dev/md0 (boot-arg
    # rd=md0), which mockfs serves as /sbin/launchd. The base is unknown here: xnu_flatten.js patches
    # these 16 bytes with the ramdisk's kernel VA and size when it packs the payload.
    memory_map = ADTNode("memory-map")
    memory_map.add_property("RAMDisk", u64(0, 0))
    chosen.add_child(memory_map)
    root.add_child(chosen)

    cpus = ADTNode("cpus")
    cpus.add_property("#address-cells", u32(1))
    cpus.add_property("#size-cells", u32(0))
    cpu0 = ADTNode("cpu0")
    cpu0.add_property("device_type", cstr("cpu"))
    cpu0.add_property("compatible", cstr("arm,cortex-a55"))
    cpu0.add_property("reg", u32(0))                      # MPIDR affinity of the boot CPU
    cpu0.add_property("cpu-id", u32(0))
    cpu0.add_property("state", cstr("running"))
    cpu0.add_property("timebase-frequency", u32(13_000_000))  # CNTFRQ_EL0 measured on the phone
    cpu0.add_property("clock-frequency", u32(2_000_000_000))
    cpus.add_child(cpu0)
    root.add_child(cpus)

    # MT6768 peripherals live at 0x0c000000..0x1fffffff (GIC 0x0c000000, TOPRGU 0x10007000,
    # UART 0x11002000, DISP 0x14000000). ranges = <child-base parent-base size>, 64-bit each;
    # XNU uses the parent base as the SoC base and adds it to child "reg" offsets.
    soc_base = 0x0C000000
    arm_io = ADTNode("arm-io")
    arm_io.add_property("device_type", cstr("fire-io"))
    arm_io.add_property("compatible", cstr("fire-io"))
    arm_io.add_property("ranges", u64(0x0, soc_base, 0x20000000 - soc_base))
    arm_io.add_property("#address-cells", u32(2))
    arm_io.add_property("#size-cells", u32(2))

    # pe_init_fiq() (pexpert/arm/pe_fiq.c) panics without /arm-io/gic. reg = GICD off/size,
    # GICR off/size, relative to the SoC base. Values from the stock fire DTB (arm,gic-v3 node).
    gic = ADTNode("gic")
    gic.add_property("compatible", cstr("arm,gic-v3"))
    gic.add_property("reg", u64(0x0C000000 - soc_base, 0x40000, 0x0C040000 - soc_base, 0x200000))
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
