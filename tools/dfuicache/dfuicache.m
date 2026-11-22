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
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc > 1 && !strcmp(argv[1], "seed")) {
            run_seed();
            return 0;
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
