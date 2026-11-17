# Phase 6 (graphics) status, 2026-11-17

## Working
- Kernel (main): `__mac_syscall("Sandbox")` returns 0 (no Sandbox.kext); this removed launchd "denied lookup ... error 159".
- launchd.plist = out/shell/launchd_bb7.plist (not in git): shell, notifyd, backboardd, cfprefsd,
  MobileGestaltHelper, containermanagerd. UserName removed from all (spawn as mobile fails: no opendirectoryd).
- Running stably: backboardd, cfprefsd, MobileGestaltHelper, containermanagerd, notifyd, ACCHWComponentAuthService.
- logd crashes in a loop (SIGTRAP, brk at main+0x160); left out.

## SpringBoard test (run from the COM6 shell)
- `/System/Library/CoreServices/SpringBoard.app/SpringBoard > /private/var/tmp/sb.log 2>&1 &`
- Spawned as pid 32, signature OK, launchd created its exec source, no crash for 60+ s.
- Only error seen: failed lookup com.apple.logd (error 3, logd not running).
- After it starts, the COM6 shell stops answering (no `fire#`, no ps output). Unknown: hang or console stall.
- Screen still shows the text console: there is no IOMobileFramebuffer driver in the kernel.

## Next
1. Check whether the shell/kernel is alive after SpringBoard starts (press Enter on COM6, watch the LCD counter).
2. Write an IOMobileFramebuffer stub kext in XNU backed by the MT6768 framebuffer
   (the same one the df console draws into: 1080x2400).
