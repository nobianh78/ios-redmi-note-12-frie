// DarwinFire dfuicache: register /Applications/*.app with LaunchServices (like uicache).
// The fire root never went through the iOS installer, so the LS database is empty:
// SpringBoard gets nil SBApplications and the Homescreen has no icons.
// Built by .github/workflows/dfuicache.yml (iOS SDK), ad-hoc signed with entitlements.plist.
#import <Foundation/Foundation.h>
#include <stdio.h>

@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (BOOL)registerApplicationDictionary:(NSDictionary *)dict;
- (BOOL)_LSPrivateRebuildApplicationDatabasesForSystemApps:(BOOL)system internal:(BOOL)internal user:(BOOL)user;
- (NSArray *)allApplications;
- (NSArray *)allInstalledApplications;
@end

static void out(NSString *s)
{
    fprintf(stdout, "%s\n", s.UTF8String);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        BOOL rebuild = argc > 1 && !strcmp(argv[1], "rebuild");
        LSApplicationWorkspace *ws = [LSApplicationWorkspace defaultWorkspace];
        out([NSString stringWithFormat:@"dfuicache: workspace %@", ws]);
        if (!ws) {
            return 1;
        }
        if (rebuild && [ws respondsToSelector:@selector(_LSPrivateRebuildApplicationDatabasesForSystemApps:internal:user:)]) {
            BOOL ok = [ws _LSPrivateRebuildApplicationDatabasesForSystemApps:YES internal:YES user:YES];
            out([NSString stringWithFormat:@"dfuicache: rebuild -> %d", ok]);
        }

        NSFileManager *fm = NSFileManager.defaultManager;
        NSArray *names = [fm contentsOfDirectoryAtPath:@"/Applications" error:nil];
        int okc = 0, failc = 0;
        for (NSString *name in names) {
            if (![name hasSuffix:@".app"]) {
                continue;
            }
            NSString *path = [@"/Applications" stringByAppendingPathComponent:name];
            NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:
                                  [path stringByAppendingPathComponent:@"Info.plist"]];
            NSString *bid = info[@"CFBundleIdentifier"];
            if (!bid) {
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
            out([NSString stringWithFormat:@"dfuicache: %@ %@ -> %d", bid, name, ok]);
            if (ok) {
                okc++;
            } else {
                failc++;
            }
        }
        NSArray *apps = [ws respondsToSelector:@selector(allInstalledApplications)] ?
            [ws allInstalledApplications] : [ws allApplications];
        out([NSString stringWithFormat:@"dfuicache: registered %d ok, %d failed; LS now lists %lu apps",
             okc, failc, (unsigned long)apps.count]);
    }
    return 0;
}
