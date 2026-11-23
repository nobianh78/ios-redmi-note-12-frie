#!/usr/bin/env python3
"""
DarwinFire: Enable com.apple.SpringBoard in /System/Library/xpc/launchd.plist
in-place inside ios16_root.img (keeping size <= existing 121-block extent so no
HFS allocation/bitmap changes are required), AND bundle:
  1. QuartzCore SWContext CPU renderer patch (.03)
  2. ManagedConfiguration non-blocking profiled bypass (.09)
  3. SetupAssistant BYSetupAssistantNeedsToRun=NO bypass (.11)
  4. MobileActivation kMAActivationStateActivated bypass (.21)
into a single tiny delta sparse image `out/quartzcore_sw_sb.simg`.
"""
import os
import plistlib
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hfs_inject_cache import Img, BS
from hfs_fill_empty import catalog
import patch_dsc_sw_full as qc

IMG_PATH = 'ios16_root.img'
OUT_SIMG = 'out/quartzcore_sw_sb.simg'


def enc_adrp(pc, target_page, rd=8):
    imm = (target_page >> 12) - (pc >> 12)
    immlo = imm & 3
    immhi = (imm >> 2) & 0x7ffff
    return (1 << 31) | (immlo << 29) | (0x10 << 24) | (immhi << 5) | (rd & 0x1f)


def enc_add_imm(rd, rn, imm12):
    return 0x91000000 | ((imm12 & 0xfff) << 10) | ((rn & 0x1f) << 5) | (rd & 0x1f)


def enc_cbz(rt, pc, target, nz=False):
    imm19 = ((target - pc) >> 2) & 0x7ffff
    return (0xb5000000 if nz else 0xb4000000) | (imm19 << 5) | (rt & 0x1f)


def main():
    # 1. First apply clean QuartzCore SWContext patch (no magenta debug bar)
    qc.main()

    # 2. Read /System/Library/xpc/launchd.plist and extra daemons
    im = Img(IMG_PATH, False)
    files, path = catalog(im)
    by_path = {path(parent, name): (rec_disk, rec) for rec_disk, parent, name, rec in files}

    def read_hfs_file(target):
        rec_disk, rec = by_path[target]
        sz = struct.unpack('>Q', rec[88:96])[0]
        exts = [struct.unpack('>II', rec[104 + 8 * i:112 + 8 * i]) for i in range(8)]
        exts = [(s, c) for s, c in exts if c]
        data = bytearray()
        for s, c in exts:
            im.f.seek(s * BS)
            data += im.f.read(c * BS)
        return bytes(data[:sz]), exts, rec_disk

    def find_va(target_va):
        for p, (rec_disk, rec) in sorted(by_path.items()):
            if not p.startswith('/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64'):
                continue
            exts = [struct.unpack('>II', rec[104 + 8 * i:112 + 8 * i]) for i in range(8)]
            exts = [(s, c) for s, c in exts if c]
            if not exts:
                continue
            im.f.seek(exts[0][0] * BS)
            hdr = im.f.read(4096)
            if not hdr.startswith(b'dyld_v1'):
                continue
            mapping_off, mapping_cnt = struct.unpack('<II', hdr[16:24])
            for i in range(mapping_cnt):
                addr, size, foff = struct.unpack('<QQQ', hdr[mapping_off + i * 32 : mapping_off + i * 32 + 24])
                if addr <= target_va < addr + size:
                    file_pos = foff + (target_va - addr)
                    cur = 0
                    for s, c in exts:
                        eb = c * BS
                        if cur <= file_pos < cur + eb:
                            return p, s * BS + (file_pos - cur)
                        cur += eb
        raise RuntimeError(f'VA {hex(target_va)} not found')

    dsc_extra_patches = [
        # CoreFoundation (.03): guard mmap() returning MAP_FAILED (-1) before ldr w8, [x0] at 0x186ee4b74
        (0x186ee4b70, struct.pack('<I', 0x14501978), 'CF mmap MAP_FAILED guard -> branch to cave 0x1882eb150'),
        (0x1882eb150, struct.pack('<IIIIIII', 0xb100041f, 0x54000080, 0xaa0003f4, 0xb9400008, 0x17afe686, 0xd2800015, 0x17afe6a8),
         'CF mmap MAP_FAILED cave: if x0==-1 close(fd) & return NULL'),
        # QuartzCore (.03): prevent CA::Render::Server & CA::WindowServer::Server from blanking the display
        (0x1883c7fec, struct.pack('<II', 0x52800000, 0xd65f03c0), 'CA::WindowServer::Display::is_blanked -> return 0'),
        (0x1883c7ff8, struct.pack('<II', 0x3900c01f, 0xd65f03c0), 'CA::WindowServer::Display::set_blanked_flag -> force 0'),
        (0x1884f4e7c, struct.pack('<II', 0x3900c11f, 0x52800008), 'CA::WindowServer::Server::render clear [display,#0x30] at entry'),
        (0x1884f4ff4, struct.pack('<I', 0x5280002a), 'CA::WindowServer::Server::render force [x19,#0x80]=1 (active)'),
        (0x1884f5014, struct.pack('<II', 0x3900c11f, 0x52800008), 'CA::WindowServer::Server::render force [x19,#0x84]=0 (unblanked)'),
        (0x1884f6688, struct.pack('<II', 0xf9003a7f, 0x52800001), 'CA::Render::Server clear blank flags [x19,#0x70] & pass w1=0'),
        (0x1882f9144, struct.pack('<I', 0x52800015), 'CA::WindowServer::Server::set_blanked force w21=0 (unblanked)'),
        # SpringBoard (.21): prevent 30-second auto-lock idle timer when @autoLockTimeout returns nil
        (0x1c593cb48, struct.pack('<IIII', 0xd2e81fa8, 0x9e670100, 0xd503201f, 0xd503201f), 'SpringBoard autoLockTimeout default -> 118784.0s (branch 1)'),
        (0x1c593cbd8, struct.pack('<IIII', 0xd2e81fa8, 0x9e670100, 0xd503201f, 0xd503201f), 'SpringBoard autoLockTimeout default -> 118784.0s (branch 2)'),
        # QuartzCore (.03): disable libEDR (OLED HDR brightness server) which deadlocks backboardd display.primary thread
        (0x1884cb708, struct.pack('<I', 0xd65f03c0), 'CA::WindowServer::Display EDR dispatch_once init -> ret'),
        (0x18833a168, struct.pack('<II', 0x52800000, 0xd65f03c0), 'CA::WindowServer::Display::edr_enabled -> return 0'),
        # ManagedConfiguration (.09): skip synchronous XPC checkIn to profiled & MC migration check queue disk loop
        (0x19ef715ac, struct.pack('<I', 0xd65f03c0), '-[MCProfileConnection checkInIfNeeded] -> ret'),
        (0x19ef7281c, struct.pack('<II', 0x52800000, 0xd65f03c0), '-[MCProfileConnection isDoneMigrating] -> return 0'),
        (0x19efe5de0, struct.pack('<II', 0x52800020, 0xd65f03c0), '-[MCProfileConnection mustWaitForMigrator] -> return 1'),
        (0x19f076f80, struct.pack('<II', 0x52800020, 0xd65f03c0), 'objc_msgSend$mustWaitForMigrator (MC) -> return 1'),
        (0x19ef87f5c, struct.pack('<II', 0xd2800000, 0xd65f03c0), '-[MCProfileConnection syncProfiled] -> return nil'),
        (0x19f078240, struct.pack('<II', 0xd2800000, 0xd65f03c0), 'objc_msgSend$xpcConnection (MC) -> return nil'),
        # SetupAssistant (.11): BYSetupAssistantNeedsToRun -> NO, HasCompletedInitialRun -> YES
        (0x1a3af1270, struct.pack('<II', 0x52800000, 0xd65f03c0), 'BYSetupAssistantNeedsToRun -> return 0'),
        (0x1a3af1558, struct.pack('<II', 0x52800020, 0xd65f03c0), 'BYSetupAssistantHasCompletedInitialRun -> return 1'),
        (0x1a3afda94, struct.pack('<II', 0x52800000, 0xd65f03c0), 'Daemon_BYSetupAssistantNeedsToRun -> return 0'),
        # MobileActivation (.21): return kMAActivationStateActivated (@"Activated") & brickState = 0
        (0x1c90d2150, struct.pack('<III', enc_adrp(0x1c90d2150, 0x1da36e000), 0xf9470d00, 0xd65f03c0),
         'MA createActivationState -> return kMAActivationStateActivated'),
        (0x1c90d37fc, struct.pack('<II', 0x52800000, 0xd65f03c0), 'MAEGetBrickState -> return 0'),
        # SpringBoardFoundation (.05): report no passcode set & unlocked since boot (clean Lockscreen, no Enter Passcode)
        (0x18e61a100, struct.pack('<II', 0x52800020, 0xd65f03c0), '-[SBFMobileKeyBag hasBeenUnlockedSinceBoot] -> return 1'),
        (0x18e60ba34, struct.pack('<II', 0x52800000, 0xd65f03c0), '-[SBFMobileKeyBag hasPasscodeSet] -> return 0'),
        (0x18e6179a4, struct.pack('<II', 0x52800000, 0xd65f03c0), '-[SBFMobileKeyBagState lockState] -> return 0'),
        (0x18e6171f4, struct.pack('<II', 0x52800000, 0xd65f03c0), '-[SBFMobileKeyBagState isEffectivelyLocked] -> return 0'),
        (0x18e611028, bytes.fromhex('000440f9adf40214'), 'RESTORE -[SBFUserAuthenticationController hasPasscodeSet]'),
        (0x18e613ff4, bytes.fromhex('000440f9b2e80214'), 'RESTORE -[SBFUserAuthenticationController hasAuthenticatedAtLeastOnceSinceBoot]'),
        (0x18e609ec8, bytes.fromhex('eefd0214f44fbea9'), 'RESTORE -[SBFUserAuthenticationController isAuthenticated]'),
        (0x18e60928c, bytes.fromhex('081440f91f0100f1'), 'RESTORE -[SBFUserAuthenticationController isAuthenticatedCached]'),
        (0x18e607e84, bytes.fromhex('f657bda9f44f01a9'), 'RESTORE -[SBFUserAuthenticationController _isUserAuthenticated]'),
        # MobileKeyBag (.13): skip synchronous XPC call to com.apple.mobile.keybagd.xpc & report unlocked/no-passcode
        (0x1ae3df468, struct.pack('<II', 0x52800060, 0xd65f03c0), 'MKBGetDeviceLockState -> return 3 (kMobileLockStateDisabled)'),
    # FBS identityForProcessIdentity: nil processIdentity -> return nil instead of brk (SB unlock crash)
    (0x19b916914, struct.pack('<II', 0xd2800014, 0x17fffff9), 'FBS identityForProcessIdentity: nil -> return nil (no brk)'),
    # SBSceneManager.m:1002 NSAssert(application) -> return nil sceneHandle (x24=0, x21=0, jump to release/epilogue)
    (0x1c5915468, struct.pack('<III', 0xd2800018, 0xd2800015, 0x17ffffd3), 'SBSceneManager nil application -> return nil (no NSAssert)'),
        (0x1ae3e7024, struct.pack('<II', 0x52800060, 0xd65f03c0), 'MKBGetDeviceLockStateIndirect -> return 3 (kMobileLockStateDisabled)'),
        (0x1ae3df824, struct.pack('<II', 0x52800020, 0xd65f03c0), 'MKBDeviceUnlockedSinceBoot -> return 1'),
        (0x1ae3dfb0c, struct.pack('<II', 0x52800020, 0xd65f03c0), 'MKBUserUnlockedSinceBoot -> return 1'),
        (0x1ae3dfa58, struct.pack('<II', 0x52800000, 0xd65f03c0), 'MKBDeviceFormattedForContentProtection -> return 0'),
        (0x1ae3ef908, struct.pack('<II', 0xd2800000, 0xd65f03c0), 'MKB XPC method 0x1ae3ef908 -> return nil'),
        (0x1ae413cc0, struct.pack('<II', 0xd2800000, 0xd65f03c0), 'objc_msgSend$sharedClient (MKB) -> return nil'),
        (0x1ae413ee0, struct.pack('<II', 0xd2800000, 0xd65f03c0), 'objc_msgSend$synchronousRemoteObjectProxy (MKB) -> return nil'),
        # BaseBoard (.05): force homeButtonType == 2 (iPhone X: Home Bar, Quick Actions, top-right Control Center)
        (0x18dd51dcc, struct.pack('<II', 0xd2800040, 0xd65f03c0), '-[BSPlatform homeButtonType] -> return 2'),
        # CoverSheet (.21): enable Lockscreen Flashlight & Camera quick-action buttons even without Apple torch/camera HW
        (0x1c56261c4, struct.pack('<II', 0x52800020, 0xd65f03c0), '-[CSQuickActionsViewController hasFlashlight] -> return 1'),
        (0x1c560d95c, struct.pack('<II', 0x52800020, 0xd65f03c0), '-[CSQuickActionsViewController hasCamera] -> return 1'),
        (0x1c564b150, struct.pack('<II', 0x52800020, 0xd65f03c0), '-[CSQuickActionsViewController allowsFlashlight] -> return 1'),
        (0x1c564b4b8, struct.pack('<II', 0x52800020, 0xd65f03c0), '-[CSQuickActionsViewController _supportsCamera] -> return 1'),
        # ContainerManagerCommon (.21): skip 20x sleep(1) retry loop waiting for absent usermanagerd in -[MCMUserIdentityCache _lock_resync]
        (0x1c4c676bc, struct.pack('<I', 0x14000044), '-[MCMUserIdentityCache _lock_resync] -> b 0x1c4c677cc (0 retries, no 20s sleep)'),
        # UIKitCore (.03): always return _UIStatusBarVisualProvider_Split1125 from _Split (eliminates red rdar:45025538 fallback bar at 720p & 1080p)
        (0x189ab566c, struct.pack('<III', enc_adrp(0x189ab566c, 0x1dce07000), 0xf9441d00, 0xd65f03c0),
         '+[_UIStatusBarVisualProvider_Split visualProviderSubclassForScreen:visualProviderInfo:] -> _Split1125'),
        # SpringBoard (.21): use in-process CPU PBUIWallpaperViewController (skip Metal PosterBoard) + clearColor window bg fallback
        (0x1c5b03670, struct.pack('<I', qc.enc_bl(0x1c5b03670, 0x1c622fa40)), 'SBWallpaperController _SBWallpaperSecureWindow bg -> clearColor'),
        (0x1c5b036a0, struct.pack('<I', qc.enc_b(0x1c5b036a0, 0x1c5b03754)), 'SBWallpaperController -> b PBUIWallpaperViewController (skip PosterBoard Metal)'),
        # PaperBoardUI: fallback color wallpaper -> clearColor (so GPU composites df_wall_px if cpbitmap missing, zero bleed!)
        (0x1b0d04fc8, struct.pack('<I', qc.enc_bl(0x1b0d04fc8, 0x1b0d9b040)), 'PBUI fallbackWallpaperConfigurationForVarient: -> clearColor'),
        # ChronoServices (.05): disable tight reconnect loop to absent com.apple.chronoservices (was burning 100% CPU in SpringBoard + launchd!)
        (0x192123a4c, struct.pack('<I', 0xd65f03c0), '-[CHSChronoServicesConnection _queue_createConnection] -> ret'),
        (0x19212c0ec, struct.pack('<I', 0xd65f03c0), '-[CHSWidgetEventServicePublisher/Tool _queue_createConnection] -> ret'),
        # CoreFoundation (.03): __CFGetProductName() AND its 6 inlined CFBundle call sites all invoke
        # dispatch_once block __CFGetProductName_block_invoke @ 0x186ec82bc (storing into [0x1e1d48300]).
        # Patch BOTH the dispatch_once block (0x186ec82bc) AND __CFGetProductName (0x186ed0168) to set/return
        # constant CFSTR("iphone") @ 0x1df281c30 so ALL ~iphone resource lookups (DefaultModuleSettings~iphone.plist
        # for Control Center 2x2/1x2 layout, DefaultIconState~iphone.plist, etc.) succeed!
        (0x186ec82bc, struct.pack('<IIIII',
            enc_adrp(0x186ec82bc, 0x1df281000, rd=8),
            enc_add_imm(8, 8, 0xc30),
            enc_adrp(0x186ec82c4, 0x1e1d48000, rd=9),
            0xf9018128,  # str x8, [x9, #0x300]
            0xd65f03c0,  # ret
        ), '__CFGetProductName_block_invoke -> [0x1e1d48300] = @"iphone" (fixes ALL 6 inlined CFBundle ~iphone sites!)'),
        (0x186ed0168, struct.pack('<III', enc_adrp(0x186ed0168, 0x1df281000, rd=0), enc_add_imm(0, 0, 0xc30), 0xd65f03c0),
         '__CFGetProductName -> return @"iphone"'),
        # UIKitCore (.03): enable Apple built-in software coordinate edge detection (options |= 1) on UIScreenEdgePanGestureRecognizer,
        # bypassing _UISEEdgeTypeFailGestureFeature & _UISEPerTypeEdgesFailGestureFeature (which failed every FocalTech touch because _edgeType==0!),
        # and widen edgeRegionSize / bottomEdgeRegionSize from 13/11pt to 28pt (iPhone X thin-bezel comfort zone for Home Bar swipe-up).
        (0x188efa754, struct.pack('<I', 0x52800036), 'UIScreenEdgePanGR _commonInit: and w22,w2,#1 -> mov w22,#1 (pure-geometry Home Bar edge detection)'),
        (0x1896c5604, struct.pack('<I', 0x52800002), '_UIScreenEdgePanRecognizer initWithType:settings:: shouldUseGrapeFlags=0'),
        (0x188e80194, struct.pack('<I', 0x1e679008), '_UISEGestureFeatureSettings init: edgeRegionSize 13.0pt -> 28.0pt'),
        (0x189ea9950, struct.pack('<d', 28.0), '_UISEGestureFeatureSettings init: bottomEdgeRegionSize 11.0pt -> 28.0pt'),
        # IconServices (.13): generate real app icons directly in-process via -[ISGenerationRequest generateImage]
        # instead of XPC to disabled iconservicesagent (which crashed on nil container_system_group_path_for_identifier).
        # 1) -[ISConcreteIcon generateImageWithDescriptor:] @ 0x1a7b4d654 (x19=self, x20=desc+1, x21=req+1 -> epilogue @ 0x1a7b4d714)
        (0x1a7b4d654, struct.pack('<15I',
            0xaa1503e0,                          # mov  x0, x21  (ISGenerationRequest*)
            qc.enc_bl(0x1a7b4d658, 0x1a7b5c954), # bl   -[ISGenerationRequest generateImage]
            qc.enc_bl(0x1a7b4d65c, 0x1a946491c), # bl   objc_claimAutoreleasedReturnValue
            0xaa0003f6,                          # mov  x22, x0  (IFImage* +1)
            enc_cbz(22, 0x1a7b4d664, 0x1a7b4d680, nz=True), # cbnz x22, done
            0xaa1303e0,                          # mov  x0, x19  (self)
            0xaa1403e2,                          # mov  x2, x20  (descriptor)
            0x52800003,                          # mov  w3, #0   (markAsPlaceholder=NO)
            qc.enc_bl(0x1a7b4d674, 0x1a7b89cc0), # bl   -[_placeholderImageWithImageDescriptor:markAsPlaceholder:]
            qc.enc_bl(0x1a7b4d678, 0x1a946491c), # bl   objc_claimAutoreleasedReturnValue
            0xaa0003f6,                          # mov  x22, x0
            qc.enc_bl(0x1a7b4d680, 0x1a9464a00), # bl   objc_release_x21 (req)
            qc.enc_bl(0x1a7b4d684, 0x1a94649f4), # bl   objc_release_x20 (descriptor)
            0xaa1603f5,                          # mov  x21, x22 (epilogue moves x21->x0 & tail-calls objc_autoreleaseReturnValue)
            qc.enc_b(0x1a7b4d68c, 0x1a7b4d714),  # b    epilogue
        ), '-[ISConcreteIcon generateImageWithDescriptor:] -> in-process [req generateImage]'),
        # 2) -[ISConcreteIcon generateImageWithDescriptor:completion:] @ 0x1a7b4aeac (x20=self, x22=desc+1, x19=completion+1, x21=req+1 -> epilogue @ 0x1a7b4af80)
        (0x1a7b4aeac, struct.pack('<21I',
            0xaa1503e0,                          # mov  x0, x21  (ISGenerationRequest*)
            qc.enc_bl(0x1a7b4aeb0, 0x1a7b5c954), # bl   -[ISGenerationRequest generateImage]
            qc.enc_bl(0x1a7b4aeb4, 0x1a946491c), # bl   objc_claimAutoreleasedReturnValue
            0xaa0003f7,                          # mov  x23, x0  (IFImage* +1)
            enc_cbz(23, 0x1a7b4aebc, 0x1a7b4aed8, nz=True), # cbnz x23, invoke
            0xaa1403e0,                          # mov  x0, x20  (self)
            0xaa1603e2,                          # mov  x2, x22  (descriptor)
            0x52800003,                          # mov  w3, #0   (markAsPlaceholder=NO)
            qc.enc_bl(0x1a7b4aecc, 0x1a7b89cc0), # bl   -[_placeholderImageWithImageDescriptor:markAsPlaceholder:]
            qc.enc_bl(0x1a7b4aed0, 0x1a946491c), # bl   objc_claimAutoreleasedReturnValue
            0xaa0003f7,                          # mov  x23, x0
            enc_cbz(19, 0x1a7b4aed8, 0x1a7b4aeec, nz=False), # cbz  x19, cleanup
            0xaa1303e0,                          # mov  x0, x19  (completion block)
            0xf9400a68,                          # ldr  x8, [x19, #0x10] (block->invoke)
            0xaa1703e1,                          # mov  x1, x23  (IFImage*)
            0xd63f0100,                          # blr  x8
            qc.enc_bl(0x1a7b4aeec, 0x1a9464a18), # bl   objc_release_x23 (img)
            qc.enc_bl(0x1a7b4aef0, 0x1a9464a00), # bl   objc_release_x21 (req)
            qc.enc_bl(0x1a7b4aef4, 0x1a9464a0c), # bl   objc_release_x22 (descriptor)
            qc.enc_bl(0x1a7b4aef8, 0x1a94649e8), # bl   objc_release_x19 (completion)
            qc.enc_b(0x1a7b4aefc, 0x1a7b4af80),  # b    epilogue
        ), '-[ISConcreteIcon generateImageWithDescriptor:completion:] -> in-process [req generateImage]'),
    ]
    resolved_dsc_patches = []
    for va, data, desc in dsc_extra_patches:
        p, off = find_va(va)
        resolved_dsc_patches.append((va, off, data, desc))

    raw_lp, exts_lp, rec_lp = read_hfs_file('/System/Library/xpc/launchd.plist')
    raw_pd, exts_pd, rec_pd = read_hfs_file('/System/Library/LaunchDaemons/com.apple.powerd.plist')
    extra_daemons = [
        '/System/Library/LaunchDaemons/com.apple.SpringBoard.plist',
        '/System/Library/LaunchDaemons/com.apple.lsd.plist',
        '/System/Library/LaunchDaemons/com.apple.lsd.system.plist',
        '/System/Library/LaunchDaemons/com.apple.distnoted.xpc.daemon.plist',
        '/System/Library/LaunchDaemons/com.apple.cache_delete.plist',
        '/System/Library/LaunchDaemons/com.apple.securityd.plist',
    ]
    loaded_daemons = {}
    for dp in extra_daemons:
        raw_d, _, _ = read_hfs_file(dp)
        loaded_daemons[dp] = plistlib.loads(raw_d)
    im.f.close()

    assert len(exts_lp) == 1, f'Expected 1 extent, got {exts_lp}'
    lp_start_blk, lp_blk_cnt = exts_lp[0]
    max_bytes = lp_blk_cnt * BS

    lp = plistlib.loads(raw_lp)
    lp['LaunchDaemons'].pop('/System/Library/LaunchDaemons/com.apple.mobile.usermanagerd.plist', None)
    lp['LaunchDaemons'].pop('/System/Library/LaunchDaemons/com.apple.managedconfiguration.profiled.plist', None)
    lp['LaunchDaemons'].pop('/System/Library/LaunchDaemons/com.apple.mobile.keybagd.plist', None)
    lp['LaunchDaemons'].pop('/System/Library/LaunchDaemons/com.apple.powerd.plist', None)
    lp['LaunchDaemons'].pop('/System/Library/LaunchDaemons/com.apple.mobile.installd.plist', None)
    lp['LaunchDaemons'].pop('/System/Library/LaunchDaemons/com.apple.installcoordinationd.plist', None)
    lp['LaunchDaemons'].pop('/System/Library/LaunchDaemons/com.apple.iconservices.iconservicesagent.plist', None)
    for dp, d in loaded_daemons.items():
        d.pop('LimitLoadFromHardware', None)
        d.pop('_LimitLoadFromClarityMode', None)
        if 'iconservicesagent' in dp:
            d.pop('UserName', None)  # run as root so systemgroup.com.apple.lsd.iconscache is writable
        if 'SpringBoard' in dp:
            d['EmbeddedHomeScreen'] = True
            d['RunAtLoad'] = False
            d['KeepAlive'] = True
        else:
            d['RunAtLoad'] = False
        lp['LaunchDaemons'][dp] = d

    # Seed /private/var dirs (90 dirs) + 12 iOS 16.7.16 (20H392) prefs at boot
    lp['LaunchDaemons']['/System/Library/LaunchDaemons/com.darwinfire.seed.plist'] = {
        'Label': 'com.darwinfire.seed',
        'ProgramArguments': ['/usr/bin/zprint', 'seed'],
        'RunAtLoad': True,
        'StandardOutPath': '/dev/console',
        'StandardErrorPath': '/dev/console',
    }

    # Keep SpringBoard & backboardd console quiet (avoid 115200-baud /dev/console spam slowing UI/boot);
    # mirror lsd os_log to /private/var/tmp/lsd.log for boot-time profiling.
    for k, d in lp['LaunchDaemons'].items():
        if k.endswith(('com.apple.SpringBoard.plist', 'com.apple.backboardd.plist', 'com.apple.containermanagerd.plist')):
            env = d.get('EnvironmentVariables')
            if isinstance(env, dict):
                env.pop('OS_ACTIVITY_DT_MODE', None)
            d.pop('StandardErrorPath', None)
            d.pop('StandardOutPath', None)
        elif k.endswith('com.apple.lsd.plist'):
            env = d.get('EnvironmentVariables')
            if isinstance(env, dict):
                env.pop('OS_ACTIVITY_DT_MODE', None)
            d.pop('StandardErrorPath', None)
            d.pop('StandardOutPath', None)

    new_raw = plistlib.dumps(lp, fmt=plistlib.FMT_BINARY)
    if len(new_raw) > max_bytes:
        lp['AppRemovalServices'] = {}
        new_raw = plistlib.dumps(lp, fmt=plistlib.FMT_BINARY)
    if len(new_raw) > max_bytes:
        exts_dict = lp.get('AppExtensions', {})
        for k in list(exts_dict.keys()):
            if len(new_raw) <= max_bytes:
                break
            if not any(keep in k for keep in ('SpringBoard', 'BackBoard', 'ControlCenter', 'CoverSheet', 'Notification')):
                del exts_dict[k]
                new_raw = plistlib.dumps(lp, fmt=plistlib.FMT_BINARY)

    assert len(new_raw) <= max_bytes, f'Still too big: {len(new_raw)} > {max_bytes}'
    print(f'[+] launchd.plist: {len(raw_lp)} -> {len(new_raw)} bytes (max {max_bytes}, fits in-place @ blk {lp_start_blk}..{lp_start_blk + lp_blk_cnt - 1})')
    print(f'[+] LaunchDaemons ({len(lp["LaunchDaemons"])}): {sorted(lp["LaunchDaemons"].keys())}')

    # 3. Write patches + new launchd.plist in-place and update logicalSize in catalog record
    total_blocks = os.path.getsize(IMG_PATH) // BS
    touched_blocks = set([642803, 642804, 643086, 643150])  # QuartzCore blocks

    with open(IMG_PATH, 'r+b') as f:
        for va, off, data, desc in resolved_dsc_patches:
            f.seek(off)
            f.write(data)
            blk_start = off // BS
            blk_end = (off + len(data) - 1) // BS
            for b in range(blk_start, blk_end + 1):
                touched_blocks.add(b)
            print(f'[+] Patched {hex(va)} @ disk {hex(off)} (blk {blk_start}..{blk_end}): {desc}')

        padded = new_raw + b'\x00' * (max_bytes - len(new_raw))
        f.seek(lp_start_blk * BS)
        f.write(padded)
        for b in range(lp_start_blk, lp_start_blk + lp_blk_cnt):
            touched_blocks.add(b)

        f.seek(rec_lp + 88)
        f.write(struct.pack('>Q', len(new_raw)))
        cat_blk = (rec_lp + 88) // BS
        touched_blocks.add(cat_blk)
        print(f'[+] Updated catalog logicalSize @ {hex(rec_lp + 88)} (blk {cat_blk})')

        # Disable /System/Library/LaunchDaemons/com.apple.powerd.plist in-place so launchd cannot spawn powerd
        pd_disabled = plistlib.dumps({'Label': 'com.apple.powerd', 'Disabled': True, 'Program': '/usr/bin/true'}, fmt=plistlib.FMT_BINARY)
        pd_blk = exts_pd[0][0]
        assert len(pd_disabled) <= exts_pd[0][1] * BS
        f.seek(pd_blk * BS)
        f.write(pd_disabled + b'\x00' * (exts_pd[0][1] * BS - len(pd_disabled)))
        touched_blocks.add(pd_blk)
        f.seek(rec_pd + 88)
        f.write(struct.pack('>Q', len(pd_disabled)))
        pd_cat_blk = (rec_pd + 88) // BS
        touched_blocks.add(pd_cat_blk)
        print(f'[+] Disabled com.apple.powerd.plist @ blk {pd_blk} (cat blk {pd_cat_blk}, {len(pd_disabled)}B)')

        blocks = sorted(touched_blocks)
        chunks = []
        pos = 0
        for b in blocks:
            if b > pos:
                chunks.append(('skip', pos, b))
            chunks.append(('raw', b, b + 1))
            pos = b + 1
        if pos < total_blocks:
            chunks.append(('skip', pos, total_blocks))

        os.makedirs(os.path.dirname(OUT_SIMG), exist_ok=True)
        with open(OUT_SIMG, 'wb') as out:
            out.write(struct.pack('<IHHHHIIII', 0xED26FF3A, 1, 0, 28, 12, BS, total_blocks, len(chunks), 0))
            for kind, s, e in chunks:
                n = e - s
                if kind == 'skip':
                    out.write(struct.pack('<HHII', 0xCAC3, 0, n, 12))
                else:
                    out.write(struct.pack('<HHII', 0xCAC1, 0, n, 12 + n * BS))
                    f.seek(s * BS)
                    out.write(f.read(n * BS))

    print(f'[+] Wrote delta sparse image: {OUT_SIMG} ({os.path.getsize(OUT_SIMG)} bytes, {len(blocks)} blocks)')


if __name__ == '__main__':
    main()
