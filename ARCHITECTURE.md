# Architecture & Subsystem Interaction Model

This document outlines the software and hardware boundary interactions within **Project DarwinFire**.

```mermaid
graph TD
    subgraph "Apple iOS Userland Layer"
        SB["SpringBoard.app (UI Compositor)"]
        LD["/sbin/launchd (PID 1)"]
        DYLD["/usr/lib/dyld (Dynamic Linker)"]
        SEC["securityd & coreauthd"]
    end

    subgraph "DarwinFire Compatibility & Translation Shims"
        M2V["Metal-to-Vulkan Shader & Framebuffer Shim"]
        SEPM["/dev/sep_mock (Secure Enclave Emulator)"]
        MSYNC["Mach Trap to POSIX Syscall Bridge"]
        VM16["16KB to 4KB Virtual Memory Page Adapter"]
    end

    subgraph "Darwin / XNU Kernel Space"
        MACH["Mach Microkernel (Tasks, Threads, IPC)"]
        BSD["BSD Subsystem (VFS, POSIX Signals, Networking)"]
        PE["Platform Expert: pe_fire.c"]
    end

    subgraph "Redmi 12 (fire) Hardware & SoC"
        CPU["MediaTek Helio G88 (2x A75 + 6x A55)"]
        GIC["ARM Generic Interrupt Controller (GICv2)"]
        UART["MTK High-Speed UART (0x11002000)"]
        GPU["ARM Mali-G52 MC2 (Vulkan 1.1 / DRM / KMS)"]
        LCD["1080x2460 DSI LCD Panel"]
    end

    %% Interactions
    SB --> M2V
    SEC --> SEPM
    LD --> MSYNC
    DYLD --> VM16

    M2V --> GPU
    GPU --> LCD
    SEPM --> MACH
    MSYNC --> BSD
    VM16 --> MACH

    PE --> UART
    PE --> GIC
    PE --> CPU
```

## System Boot Phasing
1. **Stage 1 (Bootloader Handoff):** Fastboot / LittleKernel transfers execution to XNU entry at `0x40080000`.
2. **Stage 2 (Platform Expert Init):** `pe_fire_identify_machine()` configures serial logging on MTK UART and maps GICv2.
3. **Stage 3 (VM Setup):** Mach VM initializes physical memory layout using the 4GB DRAM base (`0x40000000`).
4. **Stage 4 (RootFS Mount):** APFS container image mounted as the root partition.
5. **Stage 5 (Userland Spawn):** `launchd` starts and Mach message routing begins.
6. **Stage 6 (Hardware Shims Active):** `sep_mock` handles encryption queries; `Metal-to-Vulkan` redirects graphics rendering to the Mali GPU.
