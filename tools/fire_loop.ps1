# Hands-free XNU test on fire: boot_b = XNU, boot_a = TWRP (reads the XNU log from pstore).
# XNU resets itself 5 s after PID 1 exits (TOPRGU); LK then falls back to slot a.
# Usage: powershell -File tools/fire_loop.ps1 [-img out/xnu_arm64_boot.img] [-noflash]
param([string]$img = "out/xnu_arm64_boot.img", [switch]$noflash, [int]$timeout = 420)

function Wait-Until([scriptblock]$cond, [int]$secs) {
    for ($t = 0; $t -lt $secs; $t += 3) { if (& $cond) { return $true }; Start-Sleep 3 }
    return $false
}

# Get to fastboot: from TWRP via adb, otherwise it must already be there.
if (-not (fastboot devices)) {
    if ((adb devices) -match "`t(recovery|device)") {
        # heartbeat skips XNU while the old "DarwinFire XNU console" marker is in the zone: erase it first
        adb shell "rm -f /sys/fs/pstore/console-ramoops-0" | Out-Null
        adb reboot bootloader | Out-Null
    }
    if (-not (Wait-Until { fastboot devices } 60)) { Write-Output "LOOP: phone not in fastboot/TWRP (press Power+VolDown)"; exit 2 }
}
if (-not $noflash) {
    fastboot flash boot_b $img
    if (-not $?) { Write-Output "LOOP: flash failed (USB?)"; exit 3 }
}
fastboot --set-active=b | Out-Null
fastboot reboot | Out-Null
Start-Sleep 20
if (-not (Wait-Until { (adb devices) -match "`trecovery" } $timeout)) { Write-Output "LOOP: no TWRP after $timeout s (XNU hung?)"; exit 4 }
Start-Sleep 3
adb pull /sys/fs/pstore/console-ramoops-0 out/xnulog.txt | Out-Null
Write-Output ("LOOP: log lines " + (Get-Content out/xnulog.txt | Measure-Object -Line).Lines)
