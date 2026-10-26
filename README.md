# Project DarwinFire: iOS & XNU Research Framework for Redmi 12 ("fire")

[![Target Hardware](https://img.shields.io/badge/Target-Redmi%2012%20(fire)-FF6900.svg)](docs/01_HARDWARE_TARGET_FIRE.md)
[![SoC](https://img.shields.io/badge/SoC-MediaTek%20Helio%20G88-blue.svg)](docs/01_HARDWARE_TARGET_FIRE.md)
[![Kernel](https://img.shields.io/badge/Kernel-Apple%20XNU%20Darwin-black.svg)](docs/02_XNU_PORTING_SPEC.md)
[![Reverse Engineering](https://img.shields.io/badge/REA-Integrated-purple.svg)](https://github.com/morluto/rea)

**DarwinFire** is a specialized systems-engineering and reverse-engineering framework dedicated to porting, adapting, and analyzing **Apple XNU / Darwin kernel components and iOS userland binaries** on the **Xiaomi Redmi 12** (Codename: `fire`, MediaTek Helio G88 MT6769H).

---

## 🏗️ Repository Architecture

```text
d:\Code\IOS/
├── docs/                           # Technical Specifications & Engineering Studies
│   ├── 01_HARDWARE_TARGET_FIRE.md  # MT6769H / Helio G88 layout & CPU limitations (No PAC, 4KB page)
│   ├── 02_XNU_PORTING_SPEC.md     # Platform Expert (pexpert), Apple AIC to ARM GIC, DeviceTree
│   ├── 03_IPSW_USERLAND_PIPELINE.md# Extracting launchd, dyld, RootFS from iPhone IPSW
│   ├── 04_SECURITY_AND_SEP.md     # Secure Enclave (SEP) mocking & AMFI bypass
│   └── 05_GRAPHICS_AND_SPRINGBOARD.md # Metal to Mali-G52 Vulkan/DRM display translation
├── tools/                          # Analysis & Automation Toolchain
│   ├── macho_analyzer.js          # Pure-JS Mach-O 64-bit binary parser & compatibility validator
│   ├── ipsw_helper.js             # IPSW target finder (iPhone 7/8 non-PAC) & extraction guide
│   ├── rea_bridge.js              # Integration with morluto/rea and local Ghidra engines
│   └── boot_simulator.js          # Interactive boot tracer of XNU on Helio G88 hardware
├── kernel/                         # Darwin / XNU Kernel Porting Layer
│   ├── include/
│   │   ├── mach/mach_types.h      # Mach IPC, ports, and trap headers
│   │   └── platform/fire_mt6769.h # MediaTek MT6769H MMIO addresses (UART, GIC, DSI, Mali)
│   └── pexpert/pe_fire.c          # Platform Expert implementation for Redmi 12
├── shims/                          # Hardware & Subsystem Compatibility Shims
│   ├── libsystem_shim/            # Mach syscall translation to Linux POSIX
│   └── sep_mock/                  # Simulated Apple Secure Enclave response daemon
└── test/
    └── fixtures/                  # Synthetic Mach-O binaries for automated verification
```

---

## ⚡ Quick Start & Toolchain

### 1. Inspect Mach-O Binaries for Hardware Incompatibilities
The `macho_analyzer` tool inspects any iOS binary (executable or dylib), checks for **ARM64 vs ARM64E (PAC)**, validates **16KB vs 4KB page alignments**, and detects proprietary Apple frameworks like **Metal**:

```bash
# Analyze compatible standard ARM64 binary:
npm run inspect test/fixtures/arm64_compatible_launchd.bin

# Analyze modern PAC-enabled binary (fails on Helio G88):
npm run inspect test/fixtures/arm64e_pac_springboard.bin
```

### 2. Guide to Downloading and Extracting iPhone Firmware
Query recommended non-PAC firmware versions (iPhone 7 / iPhone 8) and view the APFS RootFS extraction instructions:

```bash
npm run ipsw
```

### 3. Reverse Engineering with REA (`morluto/rea`)
Set up the REA Model Context Protocol (MCP) bridge to connect your local Ghidra / Hopper decompiler to your AI coding agent:

```bash
# Setup REA MCP agents:
npm run rea:setup

# View REA workflow guide:
npm run rea
```

### 4. Run the XNU Boot Chain Simulator
Simulate the boot sequence of Apple's XNU kernel on the Redmi 12 hardware:

```bash
# Bare metal (shows where kernel panics without shims):
npm run simulate

# With DarwinFire transformation shims enabled:
npm run simulate -- --shims
```

---

## 🔬 Core Engineering Milestones & Challenges

| Milestone | Hardware Barrier | DarwinFire Solution |
| :--- | :--- | :--- |
| **CPU Architecture** | Helio G88 lacks **PAC (Pointer Authentication)**. | Target `arm64` binaries (iPhone 7 / A10, iPhone 8 / A11) or patch PAC opcodes. |
| **Memory Subsystem** | Android kernel runs **4KB pages**; iOS binaries require **16KB**. | Custom `vm_map` adapter and patched `dyld` dynamic linker. |
| **Interrupt Controller** | Apple uses proprietary **AIC**; MTK uses **ARM GICv2**. | Implemented `pe_fire.c` mapping GICD (`0x0C000000`) and GICC (`0x0C002000`). |
| **Security Subsystem** | iOS requires **Secure Enclave (SEP)**; Redmi lacks SEP hardware. | `sep_mock` daemon intercepts Mach IPC queries and feeds synthetic keys. |
| **Display & Graphics** | SpringBoard requires **Metal**; Redmi uses **Mali-G52 GPU**. | Metal-to-Vulkan translation shim binding to Linux `/dev/dri/card0` (DRM/KMS). |

---

## 📖 In-Depth Documentation
* [Hardware Target Details (Redmi 12 "fire")](docs/01_HARDWARE_TARGET_FIRE.md)
* [XNU Kernel Porting & Platform Expert Specification](docs/02_XNU_PORTING_SPEC.md)
* [IPSW Firmware & Userland Extraction Pipeline](docs/03_IPSW_USERLAND_PIPELINE.md)
* [Security, AMFI, and SEP Emulation](docs/04_SECURITY_AND_SEP.md)
* [Graphics Stack & SpringBoard Display Architecture](docs/05_GRAPHICS_AND_SPRINGBOARD.md)
