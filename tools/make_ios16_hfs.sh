#!/bin/bash
# ==============================================================================
# DarwinFire: Automated iOS 16 HFS+ RootFS Packager for Redmi 12 ("fire")
# Run this script on your Hackintosh (macOS) with: sudo bash make_ios16_hfs.sh
# ==============================================================================

set -e

echo "============================================================"
echo "  DARWIN-FIRE iOS 16 HFS+ ROOTFS BUILDER FOR REDMI 12"
echo "============================================================"

# Check if IPSW or DMG exists in current directory
IPSW_FILE=$(ls *.ipsw 2>/dev/null | head -n 1 || true)
ROOTFS_DMG="rootfs.dmg"
CRYPTEX_DMG="system_cryptex.dmg"

if [ ! -f "$ROOTFS_DMG" ]; then
    if [ -n "$IPSW_FILE" ]; then
        echo "[*] Found IPSW package: $IPSW_FILE"
        echo "[*] Extracting RootFS DMG from IPSW..."
        unzip -p "$IPSW_FILE" 087-86698-044.dmg > rootfs.dmg
        echo "[*] Extracting System Cryptex DMG from IPSW..."
        unzip -p "$IPSW_FILE" 087-86593-047.dmg > system_cryptex.dmg
    else
        echo "[!] Error: Neither rootfs.dmg nor an .ipsw file found in current directory!"
        echo "    Please place either iPhone10,3..._Restore.ipsw OR rootfs.dmg here."
        exit 1
    fi
fi

echo "[*] Step 1: Mounting iOS 16 RootFS DMG..."
MOUNT_ROOT=$(hdiutil attach -noverify -nobrowse "$ROOTFS_DMG" | grep -o '/Volumes/.*' | head -n 1)
echo "[+] RootFS mounted at: $MOUNT_ROOT"

MOUNT_CRYPTEX=""
if [ -f "$CRYPTEX_DMG" ]; then
    echo "[*] Step 2: Mounting System Cryptex DMG..."
    MOUNT_CRYPTEX=$(hdiutil attach -noverify -nobrowse "$CRYPTEX_DMG" | grep -o '/Volumes/.*' | head -n 1 || true)
    echo "[+] System Cryptex mounted at: $MOUNT_CRYPTEX"
fi

echo "[*] Step 3: Creating 8.2GB HFS+ target image (ios16_root.dmg)..."
# Redmi 12 "fire" super partition is 8707 MB (8.5 GB). 8200m fits RootFS (5.25GB) + dyld_shared_cache (2.6GB) with 350MB headroom.
rm -f ios16_root.dmg ios16_root.raw.dmg ios16_root.img
hdiutil create -size 8200m -fs HFS+ -volname fire -layout NONE ios16_root.dmg

echo "[*] Step 4: Mounting target HFS+ image..."
MOUNT_TARGET=$(hdiutil attach -noverify -nobrowse ios16_root.dmg | grep -o '/Volumes/.*' | head -n 1)
echo "[+] Target mounted at: $MOUNT_TARGET"

echo "[*] Step 5: Copying ALL iOS 16 RootFS directories & files (including /sbin, /bin, /usr, /dev)..."
# Ensure all essential root directories exist
mkdir -p "$MOUNT_TARGET"/dev "$MOUNT_TARGET"/sbin "$MOUNT_TARGET"/bin "$MOUNT_TARGET"/usr "$MOUNT_TARGET"/private "$MOUNT_TARGET"/etc "$MOUNT_TARGET"/var "$MOUNT_TARGET"/tmp

# Copy all root directories explicitly to prevent bash glob omissions
for d in "$MOUNT_ROOT"/*; do
    name=$(basename "$d")
    echo "    -> Copying $name..."
    cp -a "$d" "$MOUNT_TARGET"/ || true
done

# Copy any root system entries that glob might skip
for item in bin sbin usr dev etc var private tmp Applications Library System Developer; do
    if [ -e "$MOUNT_ROOT/$item" ] && [ ! -e "$MOUNT_TARGET/$item" ]; then
        echo "    -> Explicitly copying $item..."
        cp -a "$MOUNT_ROOT/$item" "$MOUNT_TARGET"/ || true
    fi
done

if [ -n "$MOUNT_CRYPTEX" ]; then
    echo "[*] Step 6: Merging dyld_shared_cache from System Cryptex..."
    mkdir -p "$MOUNT_TARGET/System/Library/Caches/com.apple.dyld"
    if [ -d "$MOUNT_CRYPTEX/System/Library/Caches/com.apple.dyld" ]; then
        cp -a "$MOUNT_CRYPTEX/System/Library/Caches/com.apple.dyld"/* "$MOUNT_TARGET/System/Library/Caches/com.apple.dyld/"
    fi
fi

echo "[*] Step 7: Applying security shims (disabling SEP keybagd)..."
if [ -f "$MOUNT_TARGET/System/Library/LaunchDaemons/com.apple.keybagd.plist" ]; then
    mv "$MOUNT_TARGET/System/Library/LaunchDaemons/com.apple.keybagd.plist" "$MOUNT_TARGET/System/Library/LaunchDaemons/com.apple.keybagd.plist.bak"
    echo "[+] Disabled com.apple.keybagd.plist"
fi

echo "[*] Step 8: Detaching all volumes..."
hdiutil detach "$MOUNT_TARGET"
hdiutil detach "$MOUNT_ROOT"
if [ -n "$MOUNT_CRYPTEX" ]; then
    hdiutil detach "$MOUNT_CRYPTEX"
fi

echo "[*] Step 9: Converting to raw flashable image (ios16_root.img)..."
hdiutil convert ios16_root.dmg -format UDRW -o ios16_root.raw.dmg
mv ios16_root.raw.dmg ios16_root.img
rm -f ios16_root.dmg

echo "============================================================"
echo "  [SUCCESS] ios16_root.img CREATED SUCCESSFULLY!"
echo "  Size: $(ls -lh ios16_root.img | awk '{print $5}')"
echo "  Copy ios16_root.img back to PC and flash via:"
echo "    fastboot flash super ios16_root.img"
echo "============================================================"
