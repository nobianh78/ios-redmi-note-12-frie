# Hardware Target Specification: Redmi 12 ("fire")

## 1. Device Overview
* **Model Name:** Xiaomi Redmi 12 (4G)
* **Codename:** `fire` (internal alternative: `heat`)
* **Model Numbers:** `23053RN02A`, `23053RN02I`, `23053RN02L`, `23053RN02Y`
* **SoC:** MediaTek Helio G88 (MT6769H / MT6768 architecture)

---

## 2. Processor Architecture & ISA Limitations

### CPU Core Layout (DynamIQ big.LITTLE)
* **2x ARM Cortex-A75** @ 2.0 GHz (Performance Cluster)
* **6x ARM Cortex-A55** @ 1.8 GHz (Efficiency Cluster)
* **Architecture:** ARMv8.2-A (64-bit)

### Critical ISA Incompatibilities with iOS:
1. **Pointer Authentication Code (PAC / arm64e):**
   * *Status:* **UNSUPPORTED**
   * Modern iOS binaries (iPhone XS / A12 and newer) are compiled with `arm64e` containing PAC instructions (`pacia`, `autia`, etc.).
   * Running `arm64e` binaries on Helio G88 triggers CPU illegal instruction fault (`SIGILL`).
   * *Required Target:* Must use iOS firmware built for `arm64` (non-PAC), such as iPhone 6s to iPhone 8 / iPhone X, or patch/strip PAC instructions from binaries.
2. **Page Size Configuration:**
   * *Redmi 12 Default:* **4KB** virtual memory page size.
   * *iOS 64-bit Default:* **16KB** page size hardcoded in `dyld_shared_cache`.
   * *Consequence:* An unmodified iOS dyld will abort upon loading segment mappings. A custom Mach-O loader or 16KB emulation layer is required.

---

## 3. Memory & Storage Subsystem
* **RAM:** 4GB / 6GB / 8GB LPDDR4X @ 1800 MHz
* **Storage:** 128GB / 256GB eMMC 5.1
* **File System in Android:** ext4 / f2fs
* **iOS Required File System:** Apple File System (APFS) with hardware cryptographic binding.

---

## 4. Interrupt & Boot Controller
* **Interrupt Controller:** ARM GICv2 / GICv3 standard.
* **Apple Requirement:** Apple AIC (Apple Interrupt Controller).
* **Clock Management:** MediaTek CCF (Common Clock Framework) controlled via MT6357/MT6358 PMIC registers.
* **UART Debug Port:** MediaTek High-Speed UART (`0x11002000` / `0x11003000`).
