# DarwinFire: CoreAnimation trên GPU Mali-G52 — lộ trình dài hạn

Cập nhật: 2026-11-22. Mốc xuất phát: tag `v5-mali-compositor`.

## Hiện trạng

| Thành phần | Trạng thái |
|---|---|
| 8 nhân CPU (6×A55 + 2×A75) | Chạy (`xnu/df_AppleARMSMP.cpp`) |
| Mali-G52 r1p0: nguồn, MMU, job manager | Chạy (`xnu/df_mali.c`) |
| Shader + descriptor do Mesa sinh | Chạy (`tools/mali_gen`, workflow `mali-gen.yml`, có `pandecode` tự kiểm) |
| Ghép khung iOS + hình nền trên GPU (`SwapEnd`) | Chạy, ~6,3 ms/khung |
| **iOS vẽ giao diện (CoreAnimation)** | **CPU — nguyên nhân lag** |

## Vì sao không giả lập Metal

QuartzCore trên iOS 16 vẽ bằng Metal (shader AIR, IOGPU/AGX). Giả lập cần trình dịch
AIR → Bifrost và toàn bộ giao diện AGX: quá lớn. Thay vào đó ta tăng tốc **nhánh vẽ phần mềm**
`CA::OGL::SWContext` mà iOS đang dùng: số thao tác vẽ hữu hạn, mỗi thao tác ánh xạ được sang
một job GPU với shader dựng sẵn.

## Kiến trúc đích

```
backboardd (QuartzCore)
  CA::OGL::SWContext::draw_*  --(trampoline vá trong DSC)-->  libDFGPU.dylib
                                                                 | gom lệnh vẽ của 1 khung
                                                                 v
                                     DFIOAcceleratorUC selector "submit" (kernel)
                                                                 v
                              df_mali.c: cấp VA GPU, map texture, dựng job chain
                              (vertex/tiler + fragment), chờ IRQ, báo fence
                                                                 v
                                        Mali-G52 vẽ thẳng vào khung iOS
```

Thao tác chưa hỗ trợ → `libDFGPU` gọi lại hàm CPU gốc (an toàn, từng bước).

## Các giai đoạn (mỗi giai đoạn kiểm chứng trên máy thật)

### G1 — Hạ tầng GPU trong kernel
- Bộ cấp phát VA GPU + map trang bất kỳ (kernel, user/IOSurface) vào AS0; huỷ map.
- Dùng IRQ GPU (SPI 274–277) thay vì vòng chờ; nhiều job đang chạy cùng lúc; fence.
- Selector `submit` trên `DFIOAcceleratorUC`: nhận bộ đệm lệnh từ user, kiểm tra, dựng job.
- **Xong khi:** user-space gửi lệnh "tô hình chữ nhật" và thấy trên màn hình.

### G2 — Pipeline vẽ hình học (Bifrost)
- `mali_gen`: thêm vertex shader + tiler job (hoặc IDVS), varying, viewport/scissor.
- Shader: tô màu đặc; vẽ texture có ma trận biến đổi + opacity; trộn premultiplied alpha.
- **Xong khi:** vẽ 100 quad có texture/tháng trong < 5 ms.

### G3 — Thư viện chèn vào backboardd (`libDFGPU.dylib`)
- Build bằng CI macOS (iOS SDK), nạp qua `DYLD_INSERT_LIBRARIES` trong `launchd.plist`.
- Dò `CA::OGL::SWContext` trong `out/dsc` (vtable, các hàm `draw_*`, cách lưu target/texture).
- Vá trampoline trong DSC (giống `tools/patch_sb_launchd.py`) trỏ sang dylib.
- **Xong khi:** log được danh sách thao tác vẽ của 1 khung màn khóa.

### G4 — Chuyển thao tác sang GPU, từ dễ đến khó
1. Tô màu / clear.
2. Vẽ ảnh (icon, chữ đã rasterize, nội dung layer) — phần lớn giao diện.
3. Mặt nạ bo góc (rounded rect, SDF trong shader).
4. Làm mờ (Gaussian 2 pass) — dock, thanh thông báo, nền khoá.
5. Đổ bóng, gradient, color matrix.
- **Xong khi:** khung màn khóa + Homescreen vẽ hoàn toàn trên GPU, CPU không còn vòng vẽ.

### G5 — Hiệu năng
- Giữ texture trên GPU giữa các khung (cache theo layer), chỉ vẽ lại vùng thay đổi.
- Đo FPS khi vuốt; mục tiêu ≥ 30 fps ở 1080×2400.

## Việc làm ngay song song (ngắn)
- Cho iOS vẽ ở 720×1600 và để GPU phóng to trong shader ghép: giảm ~2,25× tải CPU
  trong lúc G1–G4 đang làm.

## Rủi ro chính
- `SWContext` là mã C++ riêng của Apple, không có symbol: dò ngược tốn công nhất (G3).
- Đồng bộ giữa CPU và GPU khi trộn thao tác hai bên trong cùng một khung.
- Bộ nhớ: 1 GB RAM; texture phải dùng chung, tránh sao chép.
