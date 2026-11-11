# Builds out/heartbeat.bin and packs out/heartbeat_boot.img. Run from repo root.
$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
New-Item -ItemType Directory -Force out | Out-Null
# -mcmodel=tiny => adr only; image runs at load+0x40 so adrp page math would be wrong.
$cflags = '--target=aarch64-none-elf','-ffreestanding','-fno-builtin','-nostdlib','-mcmodel=tiny','-mgeneral-regs-only','-fno-pic','-O2'
clang @cflags -c "$src/start.S" -o out/hb_start.o; if (!$?) { exit 1 }
clang @cflags -c "$src/heartbeat.c" -o out/hb_main.o; if (!$?) { exit 1 }
clang @cflags -c "$src/fbcon.c" -o out/hb_fbcon.o; if (!$?) { exit 1 }
# WinLibs ld.lld is hardwired to MinGW mode; rustup's rust-lld honours -flavor gnu (ELF).
$lld = Get-ChildItem "$env:USERPROFILE\.rustup\toolchains" -Recurse -Filter rust-lld.exe | Select-Object -First 1 -ExpandProperty FullName
if (!$lld) { throw 'Need an ELF linker: install Rust (rustup) for rust-lld.exe' }
& $lld -flavor gnu -T "$src/link.ld" -static out/hb_start.o out/hb_main.o out/hb_fbcon.o -o out/heartbeat.elf; if (!$?) { exit 1 }
llvm-objcopy -O binary out/heartbeat.elf out/heartbeat.bin; if (!$?) { exit 1 }
node tools/bootimg_builder.js out/heartbeat.bin out/heartbeat_boot.img
