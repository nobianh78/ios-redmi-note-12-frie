# Security Architecture & Secure Enclave (SEP) Emulation

## 1. Apple Security Model Overview
The security of iOS relies on a hardware-anchored trust chain:
1. **BootROM:** Immutable Apple public root key.
2. **SEP (Secure Enclave Processor):** Dedicated ARM core running its own microkernel (`sepOS`).
3. **AMFI (Apple Mobile File Integrity):** Enforces code-signing, entitlements, and sandboxing.
4. **Data Protection / APFS Encryption:** Hardware AES engines tightly coupled to SEP UID.

---

## 2. SEP Mock Daemon Architecture (`sep_mock`)

On non-Apple hardware (Redmi 12), there is no SEP coprocessor. Any attempt by iOS userland daemons (`securityd`, `coreauthd`, `keybagd`) to communicate with `/dev/sep` over Mach IPC will fail.

### Mock Strategy:
* Implement a userland daemon or kernel device shim `/dev/sep_mock`.
* Intercept Mach messages targeting `com.apple.security.sep` port.
* Emulate essential SEP responses:
  * Key derivation queries (return static/test master keys).
  * Biometric status checks (report hardware unavailable or pass-through).
  * System boot measurement (return fake integrity attestation).

---

## 3. Kernel Security Mitigation Flags (boot-args)
When running modified XNU kernels, security mitigations that check Apple hardware signatures must be neutralized via boot arguments:
```text
amfi_get_out_of_my_way=1 cs_enforcement_disable=1 amfi_allow_any_signature=1 ipc_control_port_options=0
```
