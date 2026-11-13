# Builds out/init/fire_init.static: the static arm64 Mach-O PID 1 for the fire XNU port. Run from repo root.
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force out/init | Out-Null
clang --target=arm64-apple-macos11 -O2 -ffreestanding -fno-stack-protector -fno-builtin -mcpu=cortex-a55 `
    -c "$PSScriptRoot/fire_init.c" -o out/init/fire_init.o; if (!$?) { exit 1 }
# rust-lld's ld64 flavor ignores -static, so link normally and let macho_static.js drop dyld / LC_MAIN.
$lld = Get-ChildItem "$env:USERPROFILE\.rustup\toolchains" -Recurse -Filter rust-lld.exe | Select-Object -First 1 -ExpandProperty FullName
if (!$lld) { throw 'Need rust-lld.exe (install Rust via rustup)' }
& $lld -flavor darwin -arch arm64 -platform_version macos 11.0 11.0 -e _start -o out/init/fire_init out/init/fire_init.o; if (!$?) { exit 1 }
node tools/macho_static.js out/init/fire_init out/init/fire_init.static; if (!$?) { exit 1 }
