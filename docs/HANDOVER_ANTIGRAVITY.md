# Prompt bàn giao — DarwinFire (dán nguyên văn cho Antigravity)

Tiếp tục dự án **DarwinFire**: chạy **Apple iOS 16.7.16 (`20H392`, iPhone X `d221ap`, arm64)** với kernel **XNU mã nguồn mở `xnu-8796.141.3` (cấu hình `ARM64 / VMAPPLE`)** trên **Xiaomi Redmi 12 (`fire`, MediaTek Helio G88 / MT6768, 6×A55 + 2×A75, GPU Mali-G52 MC2, LCD 1080×2400 @ `0x7ce6a000`)**.
Trả lời ngắn gọn bằng **tiếng Việt**. Mỗi lần báo cáo ghi: % tổng thể, % mục đang làm, và "có tiến triển / đứng yên".

## Quy tắc bắt buộc
- Workspace `D:\Code\IOS`, Windows PowerShell 5.1: nối lệnh bằng `;`, không dùng `&&`.
- `xnu/fix-xcode16.sh` phải giữ **LF thuần** (không `\r`). Kiểm tra số byte `13` = 0 trước khi commit.
- Mọi delta `.simg` flash vào **`super`**. **Tuyệt đối không flash `userdata`.**
- `fastboot boot` không chạy trên máy này: luôn `fastboot flash boot_a …; fastboot flash boot_b …; fastboot reboot`. Cáp USB đôi khi chập chờn: nếu `boot_a` báo FAILED thì flash lại cả hai trước khi reboot.
- Người dùng tự đưa máy vào fastboot; lệnh tool nên ngắn, không chạy vòng lặp dài chặn terminal.
- Commit kết thúc bằng: `Co-Authored-By: <tên model của bạn>`.
- **Không chạy `/usr/bin/lsdiagnose` trên máy**: nó làm treo toàn bộ hệ thống.

## Build & flash
1. Kernel: `git push origin HEAD:main` → workflow `xnu-build` (~6 phút) → `gh run download <id> -n xnu-arm64-vmapple -D out/run_<id>`.
2. Đóng gói:
   ```
   node tools/xnu_flatten.js out/run_<id>/kernel.release.vmapple out/run_<id>/k.flat extracted_binaries/fire_devicetree.adt out/run_44889752226/fire-root-hfs/fire_root.img
   node tools/bootimg_builder.js out/heartbeat.bin out/xnu_arm64_boot.img - out/run_<id>/k.flat.ramdisk
   ```
3. Root (`ios16_root.img`, HFS+, flash vào `super`):
   - `python tools/patch_sb_launchd.py` → `out/quartzcore_sw_sb.simg` (launchd.plist + vá dyld shared cache).
   - Thay file có sẵn: `python -I tools/hfs_put_file.py ios16_root.img /đường/dẫn=file_local:755` (chỉ thay được file **đã tồn tại**, chưa tạo được file/thư mục mới).
   - Delta bất kỳ: `python -I tools/hfs_delta.py snapshot ios16_root.img out/before.hashes`, sửa image, rồi `python -I tools/hfs_delta.py delta ios16_root.img out/before.hashes out/x.simg`.
4. Device tree: `python tools/adt_builder.py` (8 CPU đã khai báo).

## Debug qua USB serial `COM6` (115200)
- Shell `dash` (PATH lạ: một số lệnh hiện tên `DemoApp`/`PaperBoard` trong log — vô hại). Không có `grep/head/sysctl/dmesg/sleep/cp/chown`.
- Phím: `Ctrl-T` vuốt giả, `Ctrl-Y` chạm giữa, `Ctrl-G` trạng thái touch, **`Ctrl-P` dò GPU + bật GPU compositor** (chỉ gửi **sau khi màn khóa hiện**; thường phải gửi 2 lần).
- Log kernel hữu ích: `DF_EXIT name[pid] rv sig ns code` (mọi tiến trình chết), `DF_BT` (backtrace khi SIGTRAP/SEGV), `SIGABRT … CRASH_INFO`, `df_smp:`, `mali:`.
- Ký hiệu hoá địa chỉ shared cache: `python -I tools/dsc_addr.py out/dsc 0 <addr…>`.
- Nếu COM6 báo "device not functioning": rút cắm lại cáp.

## Đã chạy trên máy thật (đừng phá)
| Mốc | Tag / ảnh boot |
|---|---|
| Cảm ứng FT8720 (SPI2: RST trước mỗi gói + chấp nhận idle/ACT-cleared) | — |
| 8 nhân SMP (`xnu/df_AppleARMSMP.cpp`: PSCI CPU_ON + GICv3 SGI; clean dcache set/way trước CPU_ON; `start.s` so MPIDR `0xFFFF`) | `v2-8core-gpu-probe` |
| Mali-G52 r1p0: bật nguồn MFG, MMU AS0, job WRITE_VALUE, job fragment | `v3`, `v4` |
| **GPU compositor**: shader Bifrost do Mesa biên dịch (`tools/mali_gen`, workflow `mali-gen.yml`, có `pandecode` tự kiểm), chạy dạng Pre-Frame DCD mỗi `SwapEnd` (~6,3 ms) | `v5-mali-compositor`, `out/xnu_v5_mali_compositor_boot.img` |
| Mở khoá vào Homescreen (vá DSC: FBS `identityForProcessIdentity` và `SBSceneManager` trả nil) | — |
| Hình nền iOS 16 ghép dưới pixel đen (`xnu/df_wallpaper.bin` 544×1200, căn trang 16 KB) | — |

Bẫy GPU đã gặp: texture tuyến tính cần **row stride chia hết 64 byte** (khung iOS 4320 B được đọc như texture 2160×1200 @ 8640 B; iOS **không** chấp nhận stride đệm); mảng kernel cho GPU phải **căn trang**; số sampler phải khớp shader.

## Việc tiếp theo (theo thứ tự)
### 1. Giao diện iOS đầy đủ (ưu tiên)
Nguyên nhân chính: LaunchServices **chưa đăng ký app nào** (không có `.csstore`) → Homescreen không icon, app tra ra `nil`.
- Áp dụng công thức **data seed `/private/var`** của bản port cùng chip MT6768 `https://github.com/Dracen-W/UniXNU-merlinx` (nhánh `ipados-stable-version`, bản sao ở `out/merlinx_data_seed.json`): 85 thư mục với đúng uid/gid (`db/lsd`, `installd`, `mobile/Library/Caches/com.apple.LaunchServices`, `containers/Shared/SystemGroup/systemgroup.com.apple.lsd.iconscache/Library/Caches`, …) và các preference: `com.apple.DataMigration`/`com.apple.migration` (`LastSystemVersion` = `20H392`), `com.apple.purplebuddy` (`SetupDone`, `SetupFinishedAllSteps`, `ForceNoBuddy`…), `com.apple.springboard` (`BKNoWatchdogs`, `FBLaunchWatchdogScale` 100), `backboardd`/`.GlobalPreferences` (`CAEnableDeepFramebuffer` = false…).
- Vì công cụ HFS chưa tạo được thư mục: chạy một **script seed lúc boot**, trước `lsd` (ví dụ qua job `com.darwinfire.shell`), hoặc viết thêm khả năng chèn catalog vào `tools/hfs_*`.
- Cân nhắc giữ **đủ danh sách daemon** của `launchd.plist` gốc (532 mục, lấy từ `ios16_root.img.bak`) và tắt bớt bằng `/var/db/com.apple.xpc.launchd/disabled.plist`, thay vì cắt còn 16 như hiện tại (`tools/patch_sb_launchd.py`).
- Đã có sẵn `dfuicache` (đăng ký app kiểu `uicache`, nguồn `tools/dfuicache`, CI `dfuicache.yml`), đã ghi đè lên `/usr/bin/zprint` và đã flash qua `out/dfuicache.simg` — **chưa chạy thử**: gọi `/usr/bin/zprint` (hoặc `/usr/bin/zprint rebuild`) từ shell sau khi seed xong.
- Sau đó: thanh trạng thái + 2 nút nhanh màn khoá (Đèn pin/Camera), Control Center (vuốt góc trên phải) và Notification Center (vuốt mép trên): kiểm tra toạ độ chạm mép trên trong `xnu/df_touch.c` (panel 1080×2460 vs framebuffer 1080×2400) và điều kiện trong SpringBoard.

### 2. Giảm lag nhanh
iOS vẽ ở 720×1600 và để shader ghép trên GPU phóng to lên 1080×2400.

### 3. Lộ trình dài hạn: CoreAnimation trên GPU
Xem `docs/gpu-coreanimation-roadmap.md` (G1–G5). Tham khảo kiến trúc UniXNU (Metal → `metal2vulkan` → driver Vulkan của Mesa; với Mali là PanVK) và thư mục `drivers/MaliMetal` trong repo merlinx.

## Tài liệu & memory
- `docs/gpu-coreanimation-roadmap.md`, `HANDOVER.md` (bàn giao cũ), memory Claude ở `C:\Users\AnhNobi\.claude\projects\D--Code-IOS\memory\` (đặc biệt `fire-smp-bringup.md`, `reference-unixnu-merlinx.md`).
- Repo: `origin/main`. Mốc an toàn: các tag `v1-ios16-lockscreen-stable` … `v5-mali-compositor`.
