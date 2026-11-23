// DarwinFire dfuicache (/usr/bin/zprint):
//   zprint seed            - seed /private/var dirs (85 dirs w/ uid/gid/mode) + iOS 16.7.16 (20H392) prefs
//   zprint [rebuild]       - register /Applications/*.app + /var/staged_system_apps/*.app with LaunchServices
//   zprint boot            - seed + wait for lsd + register all apps
//   zprint list            - list registered apps in LaunchServices
//   zprint launch <bid>    - open an app by CFBundleIdentifier
// Built by .github/workflows/dfuicache.yml (iOS SDK), ad-hoc signed with entitlements.plist.
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/arm/thread_status.h>
#include <mach-o/dyld_images.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

@interface LSApplicationProxy : NSObject
- (NSString *)applicationIdentifier;
- (NSURL *)bundleURL;
@end

@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (BOOL)registerApplicationDictionary:(NSDictionary *)dict;
- (BOOL)registerApplication:(NSURL *)url;
- (BOOL)_LSPrivateRebuildApplicationDatabasesForSystemApps:(BOOL)system internal:(BOOL)internal user:(BOOL)user;
- (BOOL)openApplicationWithBundleID:(NSString *)bundleID;
- (NSArray *)allApplications;
- (NSArray *)allInstalledApplications;
@end

static void out(NSString *s)
{
    fprintf(stdout, "%s\n", s.UTF8String);
    fflush(stdout);
}

struct SeedDir {
    const char *rel;
    uid_t uid;
    gid_t gid;
    mode_t mode;
};

// Derived from UniXNU-merlinx out/merlinx_data_seed.json (85 dirs, exact uid/gid/mode)
static const struct SeedDir kSeedDirs[] = {
    { "Keychains", 64, 0, 0755 },
    { "Managed Preferences", 0, 0, 0755 },
    { "MobileAsset", 0, 0, 0755 },
    { "MobileDevice", 0, 0, 0755 },
    { "MobileSoftwareUpdate", 0, 0, 0755 },
    { "PersonaVolumes", 0, 0, 0755 },
    { "containers", 0, 0, 0755 },
    { "containers/Bundle", 0, 0, 0755 },
    { "containers/Bundle/Application", 0, 0, 0755 },
    { "containers/Data", 0, 0, 0755 },
    { "containers/Data/System", 0, 0, 0755 },
    { "containers/Data/Application", 0, 0, 0755 },
    { "containers/Data/SystemGroup", 0, 0, 0755 },
    { "containers/Data/InternalDaemon", 0, 0, 0755 },
    { "containers/Data/PluginKitPlugin", 0, 0, 0755 },
    { "containers/Data/Temp", 0, 0, 0755 },
    { "containers/Shared", 0, 0, 0755 },
    { "containers/Shared/SystemGroup", 0, 0, 0755 },
    { "containers/Shared/SystemGroup/systemgroup.com.apple.lsd.iconscache", 0, 0, 0755 },
    { "containers/Shared/SystemGroup/systemgroup.com.apple.lsd.iconscache/Library", 0, 0, 0755 },
    { "containers/Shared/SystemGroup/systemgroup.com.apple.lsd.iconscache/Library/Caches", 0, 0, 0755 },
    { "containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache", 0, 0, 0755 },
    { "containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library", 0, 0, 0755 },
    { "containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches", 0, 0, 0755 },
    { "containers/Temp", 0, 0, 0755 },
    { "db", 0, 0, 0755 },
    { "db/dslocal", 0, 0, 0755 },
    { "db/dslocal/nodes", 0, 0, 0755 },
    { "db/dslocal/nodes/Default", 0, 0, 0755 },
    { "db/dslocal/nodes/Default/users", 0, 0, 0700 },
    { "db/dslocal/nodes/Default/groups", 0, 0, 0755 },
    { "db/dslocal/nodes/Default/config", 0, 0, 0755 },
    { "db/timezone", 0, 0, 0755 },
    { "db/diagnostics", 0, 0, 0755 },
    { "db/uuidtext", 0, 0, 0755 },
    { "db/com.apple.xpc.launchd", 0, 0, 0755 },
    { "db/com.apple.xpc.launchd/disabled", 0, 0, 0755 },
    { "db/com.apple.xpc.launchd/system", 0, 0, 0755 },
    { "db/com.apple.xpc.launchd/user", 0, 0, 0755 },
    { "db/com.apple.xpc.launchd/log", 0, 0, 0755 },
    { "db/lsd", 501, 501, 0775 },
    { "dmd", 0, 0, 0755 },
    { "empty", 0, 0, 0555 },
    { "folders", 0, 0, 0755 },
    { "hardware", 0, 0, 0755 },
    { "installd", 0, 0, 0755 },
    { "installd/Library", 0, 0, 0755 },
    { "installd/Library/Caches", 0, 0, 0755 },
    { "keybags", 0, 0, 0700 },
    { "keybags/backup", 0, 0, 0700 },
    { "Library", 0, 0, 0755 },
    { "local", 0, 0, 0755 },
    { "lock", 0, 0, 0755 },
    { "logs", 0, 0, 0755 },
    { "log", 0, 0, 0755 },
    { "log/com.apple.xpc.launchd", 0, 0, 0755 },
    { "mobile", 501, 501, 0755 },
    { "mobile/Containers", 501, 501, 0755 },
    { "mobile/Containers/Data", 501, 501, 0755 },
    { "mobile/Containers/Data/Application", 501, 501, 0755 },
    { "mobile/Containers/Data/InternalDaemon", 501, 501, 0755 },
    { "mobile/Containers/Data/PluginKitPlugin", 501, 501, 0755 },
    { "mobile/Containers/Shared", 501, 501, 0755 },
    { "mobile/Containers/Shared/AppGroup", 501, 501, 0755 },
    { "mobile/Documents", 501, 501, 0755 },
    { "mobile/Library", 501, 501, 0755 },
    { "mobile/Library/Caches", 501, 501, 0755 },
    { "mobile/Library/Caches/com.apple.LaunchServices", 501, 501, 0755 },
    { "mobile/Library/Caches/com.apple.IconsCache", 501, 501, 0755 },
    { "mobile/Library/Application Support", 501, 501, 0755 },
    { "mobile/Library/Logs", 501, 501, 0755 },
    { "mobile/Library/Logs/CrashReporter", 501, 501, 0755 },
    { "mobile/Library/Preferences", 501, 501, 0755 },
    { "mobile/Library/SpringBoard", 501, 501, 0755 },
    { "mobile/Library/ControlCenter", 501, 501, 0755 },
    { "mobile/Library/BulletinBoard", 501, 501, 0755 },
    { "mobile/Media", 501, 501, 0755 },
    { "mobile/tmp", 501, 501, 0755 },
    { "msgs", 0, 0, 0755 },
    { "networkd", 0, 0, 0755 },
    { "preboot", 0, 0, 0755 },
    { "preferences", 0, 0, 0755 },
    { "root", 0, 0, 0750 },
    { "root/Library", 0, 0, 0755 },
    { "root/Library/Preferences", 0, 0, 0755 },
    { "run", 0, 0, 0755 },
    { "spool", 0, 0, 0755 },
    { "tmp", 0, 0, 01777 },
    { "vm", 0, 0, 0755 },
    { "wireless", 0, 0, 0755 },
};

static void write_plist(NSString *rel, uid_t uid, gid_t gid, mode_t mode, NSDictionary *dict)
{
    NSString *full = [@"/private/var" stringByAppendingPathComponent:rel];
    NSData *data = [NSPropertyListSerialization dataWithPropertyList:dict
                                                              format:NSPropertyListBinaryFormat_v1_0
                                                             options:0
                                                               error:nil];
    if (!data) {
        return;
    }
    [data writeToFile:full atomically:YES];
    chown(full.UTF8String, uid, gid);
    chmod(full.UTF8String, mode);
}

static BOOL unpack_zlib_file(const char *src_path, const char *dst_path, uid_t uid, gid_t gid, mode_t mode, size_t *out_sz)
{
    NSData *z = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:src_path]];
    if (!z || z.length < 16) return NO;
    const uint8_t *zb = (const uint8_t *)z.bytes;
    // Format: 8-byte little-endian uncompressed length followed by zlib stream
    uint64_t raw_len = 0;
    memcpy(&raw_len, zb, 8);
    if (raw_len == 0 || raw_len > 32 * 1024 * 1024) return NO;
    NSMutableData *raw = [NSMutableData dataWithLength:(NSUInteger)raw_len];
    if (!raw) return NO;
    uLongf dest_len = (uLongf)raw_len;
    int rc = uncompress((Bytef *)raw.mutableBytes, &dest_len, zb + 8, (uLong)(z.length - 8));
    if (rc != Z_OK || dest_len != raw_len) return NO;
    if (![raw writeToFile:[NSString stringWithUTF8String:dst_path] atomically:NO]) return NO;
    chown(dst_path, uid, gid);
    chmod(dst_path, mode);
    if (out_sz) *out_sz = (size_t)dest_len;
    return YES;
}

static void seed_prebaked_ls_csstore(void)
{
    // Pre-baked container metadata & compressed .csstore blobs stored in unused DemoApp dummy files
    NSString *metaSrc = @"/Applications/DemoApp.app/LaunchImage-800-Portrait-736h@3x.png";
    const char *sysSrc  = "/Applications/DemoApp.app/LaunchImage-700@2x.png";
    const char *mainSrc = "/Applications/DemoApp.app/LaunchImage-800-667h@2x.png";
    NSData *metaData = [NSData dataWithContentsOfFile:metaSrc];
    if (!metaData || metaData.length < 64) return;
    NSDictionary *meta = [NSPropertyListSerialization propertyListWithData:metaData options:0 format:nil error:nil];
    NSString *uuid = [meta isKindOfClass:[NSDictionary class]] ? meta[@"MCMMetadataUUID"] : nil;
    if (![uuid isKindOfClass:[NSString class]] || uuid.length < 8) return;

    NSString *base = [@"/private/var/mobile/Containers/Data/InternalDaemon" stringByAppendingPathComponent:uuid];
    NSString *caches = [base stringByAppendingPathComponent:@"Library/Caches"];
    NSString *mainDst = [caches stringByAppendingPathComponent:@"com.apple.LaunchServices-4035-v2.csstore"];
    struct stat st;
    if (stat(mainDst.UTF8String, &st) == 0 && st.st_size > 100000) return;

    NSArray<NSString *> *dirs = @[
        base,
        [base stringByAppendingPathComponent:@"Documents"],
        [base stringByAppendingPathComponent:@"Library"],
        caches,
        [caches stringByAppendingPathComponent:@"OpenOperationStaging"],
        [base stringByAppendingPathComponent:@"SystemData"],
        [base stringByAppendingPathComponent:@"tmp"],
    ];
    for (NSString *d in dirs) {
        mkdir(d.UTF8String, 0755);
        chown(d.UTF8String, 501, 501);
        chmod(d.UTF8String, 0755);
    }
    NSString *metaDst = [base stringByAppendingPathComponent:@".com.apple.mobile_container_manager.metadata.plist"];
    [metaData writeToFile:metaDst atomically:NO];
    chown(metaDst.UTF8String, 501, 501);
    chmod(metaDst.UTF8String, 0644);

    NSString *sysDst = [caches stringByAppendingPathComponent:@"SystemDataOnly-com.apple.LaunchServices-4035-v2.csstore"];
    size_t sz1 = 0, sz2 = 0;
    BOOL ok1 = unpack_zlib_file(sysSrc, sysDst.UTF8String, 501, 501, 0660, &sz1);
    BOOL ok2 = unpack_zlib_file(mainSrc, mainDst.UTF8String, 501, 501, 0660, &sz2);
    out([NSString stringWithFormat:@"dfuicache: seeded prebaked LS csstore (%@, sys=%d/%zuB, main=%d/%zuB)",
         uuid, ok1, sz1, ok2, sz2]);
}

static void run_seed(void)
{
    int created = 0;
    size_t n = sizeof(kSeedDirs) / sizeof(kSeedDirs[0]);
    for (size_t i = 0; i < n; i++) {
        char path[512];
        snprintf(path, sizeof(path), "/private/var/%s", kSeedDirs[i].rel);
        if (mkdir(path, kSeedDirs[i].mode) == 0) {
            created++;
        }
        chown(path, kSeedDirs[i].uid, kSeedDirs[i].gid);
        chmod(path, kSeedDirs[i].mode);
    }

    // Unpack pre-baked LaunchServices .csstore FIRST (takes <0.3s, well before lsd spawns!)
    seed_prebaked_ls_csstore();

    NSString *buildVer = @"20H392";
    NSDictionary *migrationDict = @{
        @"LastSystemVersion": buildVer,
        @"DMLastMigrationResults": @{ @"success": @YES, @"buildVersion": buildVer },
        @"DMUserDataDisposition": @{ @"SyntheticDidUpgrade": @NO, @"buildVersion": buildVer },
    };
    write_plist(@"mobile/Library/Preferences/com.apple.DataMigration.plist", 501, 501, 0600, migrationDict);
    write_plist(@"mobile/Library/Preferences/com.apple.migration.plist", 501, 501, 0600, migrationDict);

    write_plist(@"mobile/Library/Preferences/com.apple.purplebuddy.plist", 501, 501, 0600, @{
        @"SetupDone": @YES,
        @"SetupFinishedAllSteps": @YES,
        @"SetupVersion": @11,
        @"SetupState": @"SetupUsingAssistant",
        @"ForceNoBuddy": @YES,
        @"daemonizeNeedsToRun": @NO,
    });
    write_plist(@"mobile/Library/Preferences/com.apple.purplebuddy.notbackedup.plist", 501, 501, 0600, @{
        @"ShouldRunDiagnosticsMiniBuddy": @NO,
        @"ShouldRunHSA2MiniBuddy": @NO,
        @"ShouldRunKeychainSyncMiniBuddy": @NO,
        @"ShouldRunMesaMiniBuddy": @NO,
        @"ShouldRunPaymentMiniBuddy": @NO,
        @"ShouldRunStandardMiniBuddy": @NO,
        @"SilentICDPUpgrade": @NO,
    });
    write_plist(@"mobile/Library/Preferences/com.apple.managedconfiguration.notbackedup.plist", 501, 501, 0600, @{
        @"LockdownActivationIndicatesCloudConfigurationAvailable": @NO,
    });
    write_plist(@"mobile/Library/Preferences/com.apple.springboard.plist", 501, 501, 0600, @{
        @"FBLaunchWatchdogScale": @100.0,
        @"BKNoWatchdogs": @YES,
        @"FBLaunchWatchdogExceptions": @{
            @"com.apple.purplebuddy": @200.0,
            @"com.apple.PosterBoard": @200.0,
            @"com.apple.SpringBoard": @200.0,
        },
        @"MCForceShowCloudConfigurationUI": @NO,
        @"SBDisableAutoDim": @YES,
        @"SBDontDimOrLockOnAC": @YES,
        @"SBDisableIdleSleep": @YES,
        @"SBShowBatteryPercentage": @YES,
    });
    write_plist(@"mobile/Library/Preferences/com.apple.frontboard.plist", 501, 501, 0600, @{
        @"FBLaunchWatchdogScale": @100.0,
    });
    NSDictionary *caPrefs = @{
        @"CAEnableDeepFramebuffer": @NO,
        @"CAEnableFramebufferCompression": @NO,
        @"CAEnableIOSurfaceCompression": @NO,
    };
    write_plist(@"mobile/Library/Preferences/backboardd.plist", 501, 501, 0644, caPrefs);
    write_plist(@"mobile/Library/Preferences/.GlobalPreferences.plist", 501, 501, 0644, caPrefs);
    write_plist(@"root/Library/Preferences/backboardd.plist", 0, 0, 0644, caPrefs);
    write_plist(@"root/Library/Preferences/.GlobalPreferences.plist", 0, 0, 0644, caPrefs);

    // Default Control Center module configuration so CC is populated immediately
    write_plist(@"mobile/Library/ControlCenter/ModuleConfiguration.plist", 501, 501, 0644, @{
        @"version": @2,
        @"module-identifiers": @[
            @"com.apple.control-center.FlashlightModule",
            @"com.apple.mobiletimer.controlcenter.timer",
            @"com.apple.control-center.CalculatorModule",
            @"com.apple.control-center.CameraModule",
        ],
    });

    // Fix permissions on existing System container subdirs (containermanagerd created some 0000)
    // so lsd (UID 501) can persist its .csstore cache across reboots.
    {
        NSFileManager *fm = NSFileManager.defaultManager;
        NSString *sysRoot = @"/private/var/containers/Data/System";
        for (NSString *uuid in [fm contentsOfDirectoryAtPath:sysRoot error:nil]) {
            NSString *cdir = [sysRoot stringByAppendingPathComponent:uuid];
            chmod(cdir.UTF8String, 0755);
            for (NSString *sub in @[@"Library", @"Library/Caches", @"Library/Caches/com.apple.LaunchServices", @"tmp"]) {
                NSString *sp = [cdir stringByAppendingPathComponent:sub];
                struct stat st;
                if (stat(sp.UTF8String, &st) == 0 && S_ISDIR(st.st_mode)) {
                    chmod(sp.UTF8String, 0777);
                }
            }
        }
    }

    // Seed native iOS 16 static wallpaper (.cpbitmap) from SydneyWallpaper.jpg so
    // PBUIWallpaperViewController draws the real wallpaper natively inside SpringBoard
    // (giving real blurred wallpaper in Control Center / Dock / Quick Actions and zero bleed).
    {
        NSString *lockCp = @"/private/var/mobile/Library/SpringBoard/LockBackground.cpbitmap";
        NSString *homeCp = @"/private/var/mobile/Library/SpringBoard/HomeBackground.cpbitmap";
        struct stat st;
        if (stat(lockCp.UTF8String, &st) != 0 || st.st_size < 4096) {
            @try {
                NSString *src = @"/System/Library/PrivateFrameworks/Settings/DisplayAndBrightnessSettings.framework/ContinuousExposeLayout-Light-LTR.ca/assets/SydneyWallpaper.jpg";
                UIImage *img = [UIImage imageWithContentsOfFile:src];
                void *h = dlopen("/System/Library/PrivateFrameworks/PaperBoardUI.framework/PaperBoardUI", RTLD_NOW);
                Class storeCls = h ? NSClassFromString(@"PBUIWallpaperDirectoryDataStore") : Nil;
                if (img && storeCls) {
                    NSURL *sbDir = [NSURL fileURLWithPath:@"/private/var/mobile/Library/SpringBoard" isDirectory:YES];
                    SEL initSel = NSSelectorFromString(@"initWithDirectoryURL:");
                    SEL setSel = NSSelectorFromString(@"setWallpaperImage:forVariant:wallpaperMode:");
                    id rawStore = [storeCls alloc];
                    if ([rawStore respondsToSelector:initSel]) {
                        typedef id (*InitFn)(id, SEL, NSURL *);
                        InitFn initFn = (InitFn)[rawStore methodForSelector:initSel];
                        id store = initFn(rawStore, initSel, sbDir);
                        if (store && [store respondsToSelector:setSel]) {
                            typedef BOOL (*SetImgFn)(id, SEL, UIImage *, NSInteger, NSInteger);
                            SetImgFn fn = (SetImgFn)[store methodForSelector:setSel];
                            BOOL ok0 = fn(store, setSel, img, 0, 0);
                            BOOL ok1 = fn(store, setSel, img, 1, 0);
                            chown(lockCp.UTF8String, 501, 501);
                            chmod(lockCp.UTF8String, 0644);
                            chown(homeCp.UTF8String, 501, 501);
                            chmod(homeCp.UTF8String, 0644);
                            out([NSString stringWithFormat:@"dfuicache: seeded native wallpaper cpbitmap lock=%d home=%d", ok0, ok1]);
                        }
                    }
                }
            } @catch (NSException *ex) {
                out([NSString stringWithFormat:@"dfuicache: wallpaper seed skipped (%@)", ex]);
            }
        }
    }

    out([NSString stringWithFormat:@"dfuicache: seeded %zu dirs (%d new) + 12 prefs (build %@)",
         n, created, buildVer]);
}

static BOOL should_skip_app(NSString *name)
{
    static NSSet *skip = nil;
    if (!skip) {
        skip = [NSSet setWithArray:@[
            // DarwinFire shell tool placeholders in /Applications
            @"DemoApp.app", @"PaperBoard.app", @"ClarityCamera.app",
            @"ClarityPhotos.app", @"PDUIApp.app", @"EventViewService.app",
            // Setup / pre-boot apps that should not hijack Homescreen
            @"PreBoard.app", @"Setup.app", @"CheckerBoard.app", @"CheckerBoardRemoteSetup.app",
        ]];
    }
    return [skip containsObject:name];
}

static int register_dir(LSApplicationWorkspace *ws, NSString *dir, int *okc, int *failc, int *skipc)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    NSArray *names = [[fm contentsOfDirectoryAtPath:dir error:nil]
                      sortedArrayUsingSelector:@selector(compare:)];
    for (NSString *name in names) {
        if (![name hasSuffix:@".app"]) {
            continue;
        }
        if (should_skip_app(name)) {
            (*skipc)++;
            continue;
        }
        NSString *path = [dir stringByAppendingPathComponent:name];
        NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:
                              [path stringByAppendingPathComponent:@"Info.plist"]];
        NSString *bid = info[@"CFBundleIdentifier"];
        if (!bid) {
            (*skipc)++;
            continue;
        }
        NSMutableDictionary *d = [info mutableCopy];
        d[@"ApplicationType"] = @"System";
        d[@"Path"] = path;
        d[@"CFBundleIdentifier"] = bid;
        d[@"IsDeletable"] = @NO;
        d[@"SignerOrganization"] = @"Apple Inc.";
        d[@"CodeInfoIdentifier"] = bid;
        d[@"_LSBundleFlags"] = @0;

        BOOL ok = [ws registerApplicationDictionary:d];
        if (!ok && [ws respondsToSelector:@selector(registerApplication:)]) {
            ok = [ws registerApplication:[NSURL fileURLWithPath:path]];
        }
        if (ok) {
            (*okc)++;
        } else {
            (*failc)++;
            out([NSString stringWithFormat:@"dfuicache: FAIL %@ (%@)", bid, path]);
        }
    }
    return *okc;
}

static int run_register(BOOL rebuild)
{
    LSApplicationWorkspace *ws = [LSApplicationWorkspace defaultWorkspace];
    out([NSString stringWithFormat:@"dfuicache: workspace %@", ws]);
    if (!ws) {
        return 1;
    }
    if (rebuild && [ws respondsToSelector:@selector(_LSPrivateRebuildApplicationDatabasesForSystemApps:internal:user:)]) {
        BOOL ok = [ws _LSPrivateRebuildApplicationDatabasesForSystemApps:YES internal:YES user:YES];
        out([NSString stringWithFormat:@"dfuicache: rebuild -> %d", ok]);
    }
    int okc = 0, failc = 0, skipc = 0;
    register_dir(ws, @"/Applications", &okc, &failc, &skipc);
    register_dir(ws, @"/var/staged_system_apps", &okc, &failc, &skipc);
    NSArray *apps = [ws respondsToSelector:@selector(allInstalledApplications)] ?
        [ws allInstalledApplications] : [ws allApplications];
    out([NSString stringWithFormat:@"dfuicache: registered %d ok, %d failed, %d skipped; LS now lists %lu apps",
         okc, failc, skipc, (unsigned long)apps.count]);
    return 0;
}

static int run_z64(const char *path, int start_chunk, int end_chunk)
{
    NSData *raw = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path]];
    if (!raw) {
        fprintf(stdout, "Z64_ERR open %s\n", path);
        fflush(stdout);
        return 1;
    }
    uLong orig_crc = crc32(0L, Z_NULL, 0);
    orig_crc = crc32(orig_crc, (const Bytef *)raw.bytes, (uInt)raw.length);
    uLongf comp_cap = compressBound((uLong)raw.length);
    NSMutableData *comp = [NSMutableData dataWithLength:(NSUInteger)comp_cap];
    int rc = compress2((Bytef *)comp.mutableBytes, &comp_cap, (const Bytef *)raw.bytes, (uLong)raw.length, 6);
    if (rc != Z_OK) {
        fprintf(stdout, "Z64_ERR compress %d\n", rc);
        fflush(stdout);
        return 1;
    }
    const size_t kChunk = 768; // -> 1024 base64 chars per line
    int total = (int)((comp_cap + kChunk - 1) / kChunk);
    if (start_chunk < 0) start_chunk = 0;
    if (end_chunk < 0 || end_chunk > total) end_chunk = total;
    fprintf(stdout, "Z64_BEGIN %lu %lu %08lx %d\n",
            (unsigned long)raw.length, (unsigned long)comp_cap, (unsigned long)orig_crc, total);
    fflush(stdout);
    const uint8_t *cb = (const uint8_t *)comp.bytes;
    for (int i = start_chunk; i < end_chunk; i++) {
        size_t off = (size_t)i * kChunk;
        size_t len = (off + kChunk <= comp_cap) ? kChunk : (comp_cap - off);
        uLong ccrc = crc32(0L, cb + off, (uInt)len);
        NSData *slice = [NSData dataWithBytesNoCopy:(void *)(cb + off) length:len freeWhenDone:NO];
        NSString *b64 = [slice base64EncodedStringWithOptions:0];
        fprintf(stdout, "Z64 %d %08lx %s\n", i, (unsigned long)ccrc, b64.UTF8String);
        fflush(stdout);
        usleep(22000); // ~47 KB/s, well under 102 KB/s USB CDC ring limit
    }
    fprintf(stdout, "Z64_END\n");
    fflush(stdout);
    return 0;
}

static int run_sample(pid_t pid, int iters)
{
    mach_port_t task = MACH_PORT_NULL;
    kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);
    if (kr != KERN_SUCCESS) {
        fprintf(stdout, "SAMPLE_ERR task_for_pid(%d)=0x%x\n", pid, kr);
        fflush(stdout);
        return 1;
    }
    struct task_dyld_info dyld_info;
    mach_msg_type_number_t dcnt = TASK_DYLD_INFO_COUNT;
    uint64_t dsc_slide = 0;
    if (task_info(task, TASK_DYLD_INFO, (task_info_t)&dyld_info, &dcnt) == KERN_SUCCESS &&
        dyld_info.all_image_info_addr) {
        struct dyld_all_image_infos aii;
        vm_size_t outsz = 0;
        memset(&aii, 0, sizeof(aii));
        if (vm_read_overwrite(task, (vm_address_t)dyld_info.all_image_info_addr,
                              sizeof(aii), (vm_address_t)&aii, &outsz) == KERN_SUCCESS) {
            dsc_slide = (uint64_t)aii.sharedCacheSlide;
        }
    }
    fprintf(stdout, "SAMPLE_BEGIN pid=%d dsc_slide=0x%llx\n", pid, (unsigned long long)dsc_slide);
    for (int it = 0; it < iters; it++) {
        thread_act_array_t threads = NULL;
        mach_msg_type_number_t tcnt = 0;
        if (task_threads(task, &threads, &tcnt) != KERN_SUCCESS) break;
        for (mach_msg_type_number_t i = 0; i < tcnt; i++) {
            thread_basic_info_data_t bi;
            mach_msg_type_number_t bic = THREAD_BASIC_INFO_COUNT;
            int cpu = -1, rs = -1;
            if (thread_info(threads[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &bic) == KERN_SUCCESS) {
                cpu = bi.cpu_usage;
                rs = bi.run_state;
            }
            arm_thread_state64_t st;
            mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
            memset(&st, 0, sizeof(st));
            if (thread_get_state(threads[i], ARM_THREAD_STATE64, (thread_state_t)&st, &sc) == KERN_SUCCESS) {
                uint64_t pc = (uint64_t)arm_thread_state64_get_pc(st);
                uint64_t lr = (uint64_t)arm_thread_state64_get_lr(st);
                uint64_t fp = (uint64_t)arm_thread_state64_get_fp(st);
                uint64_t f0[2] = {0, 0}, f1[2] = {0, 0};
                vm_size_t osz = 0;
                if (fp && vm_read_overwrite(task, (vm_address_t)fp, 16, (vm_address_t)f0, &osz) == KERN_SUCCESS && f0[0]) {
                    vm_read_overwrite(task, (vm_address_t)f0[0], 16, (vm_address_t)f1, &osz);
                }
                if (cpu > 0 || rs == TH_STATE_RUNNING || it == 0) {
                    fprintf(stdout, "S[%d] t%u cpu=%d rs=%d pc=0x%llx lr=0x%llx bt1=0x%llx bt2=0x%llx\n",
                            it, i, cpu, rs,
                            (unsigned long long)(pc - dsc_slide),
                            (unsigned long long)(lr - dsc_slide),
                            (unsigned long long)(f0[1] ? f0[1] - dsc_slide : 0),
                            (unsigned long long)(f1[1] ? f1[1] - dsc_slide : 0));
                }
            }
            mach_port_deallocate(mach_task_self(), threads[i]);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)threads, tcnt * sizeof(thread_act_t));
        fflush(stdout);
        if (it + 1 < iters) usleep(100000);
    }
    fprintf(stdout, "SAMPLE_END\n");
    fflush(stdout);
    mach_port_deallocate(mach_task_self(), task);
    return 0;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc > 1 && !strcmp(argv[1], "seed")) {
            run_seed();
            return 0;
        }
        if (argc > 2 && !strcmp(argv[1], "z64")) {
            int s = (argc > 3) ? atoi(argv[3]) : 0;
            int e = (argc > 4) ? atoi(argv[4]) : -1;
            return run_z64(argv[2], s, e);
        }
        if (argc > 2 && !strcmp(argv[1], "sample")) {
            int iters = (argc > 3) ? atoi(argv[3]) : 5;
            return run_sample((pid_t)atoi(argv[2]), iters);
        }
        if (argc > 1 && !strcmp(argv[1], "boot")) {
            run_seed();
            return run_register(NO);
        }
        if (argc > 1 && !strcmp(argv[1], "list")) {
            LSApplicationWorkspace *ws = [LSApplicationWorkspace defaultWorkspace];
            NSArray *apps = [ws respondsToSelector:@selector(allInstalledApplications)] ?
                [ws allInstalledApplications] : [ws allApplications];
            out([NSString stringWithFormat:@"dfuicache: %lu apps in LS:", (unsigned long)apps.count]);
            for (LSApplicationProxy *app in apps) {
                NSString *bid = [app respondsToSelector:@selector(applicationIdentifier)] ?
                    [app applicationIdentifier] : [app description];
                out([NSString stringWithFormat:@"  %@", bid]);
            }
            return 0;
        }
        if (argc > 2 && !strcmp(argv[1], "launch")) {
            LSApplicationWorkspace *ws = [LSApplicationWorkspace defaultWorkspace];
            NSString *bid = [NSString stringWithUTF8String:argv[2]];
            BOOL ok = [ws openApplicationWithBundleID:bid];
            out([NSString stringWithFormat:@"dfuicache: launch %@ -> %d", bid, ok]);
            return ok ? 0 : 1;
        }
        BOOL rebuild = (argc > 1 && !strcmp(argv[1], "rebuild"));
        return run_register(rebuild);
    }
}
