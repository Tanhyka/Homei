# Homei — Engine Cờ Long Phượng (CLP)

**Homei** là engine cờ **Cờ Long Phượng (CLP)** viết bằng **C++20**, giao tiếp qua giao thức **`clp-ep 1`** để kết nối với giao diện (như [ColongphuongGUI_1](https://github.com/Tanhyka/ColongphuongGUI_1)) hoặc chạy tự động đối kháng/kiểm thử.

## Tính năng chính
- **Tìm kiếm đa luồng (Lazy SMP) & Lockless Transposition Table:** hỗ trợ nhiều luồng tìm kiếm song song và bảng băm chia sẻ không khóa.
- **Tìm kiếm chiếu hết chuyên sâu (DF-PN Mate Search):** giải các thế chiếu hết dài 15+ ply.
- **Đánh giá trung cuộc (Koma-gumi & Sabaki):** tối ưu hóa triển khai quân, giải phóng quân tầm xa (Long/Phượng/Xa), tránh xoay quân vào biên và quản lý thời gian động.
- **Chuẩn hóa giao thức `clp-ep 1`:** hỗ trợ `Depth`, `MultiPV`, `Threads`, `Hash`, `movetime`, `infinite`.
- **Bộ công cụ kiểm thử & đối kháng (`tests/`, `tools/`):** kiểm tra tính hợp lệ của luật, đối xứng đánh giá, suite chiến thuật và xuất kỳ phổ chuẩn **LPGF 2.0**.

## Tải bản chạy sẵn
Tải file **`Homei_v3-2.exe`** (Windows x64) tại mục [**Releases**](https://github.com/Tanhyka/Homei/releases).

## Tự biên dịch từ mã nguồn (CMake + C++20)
```powershell
cd HomeiEngine
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```
