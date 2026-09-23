# X1 SYS Island v0.6.0

Windows x64 overlay cho Intel CPU, System RAM, Intel iGPU và nhiệt độ CPU trực tiếp. Giao diện Win32/GDI rộng **560 px** (bằng X1 AI Island: thu gọn 560×46, mở rộng 560×128), logo Intel và About 3S.

## Chạy

Giữ **IntelMSR.bin** cạnh **X1-SYS-Island.exe**. Mở EXE, chấp nhận Administrator để đọc cảm biến CPU qua PawnIO đã cài trên máy. Không cần HWiNFO, LibreHardwareMonitor hay web server.

- Ctrl+Shift+D: ẩn/hiện. Có phím dự phòng trong menu nếu bị chiếm.
- Hover 1 giây: ẩn 5 giây rồi hiện lại; phải rời chuột và vào lại để kích hoạt tiếp.
- Kéo chuột trái để di chuyển; nhấp đúp mở/thu chi tiết.
- Mở EXE lần nữa để khôi phục cửa sổ đang ẩn.
- Chuột phải: mở/thu, reset về góc trái, ẩn, đổi shortcut, About, Exit.

Phần mở rộng gồm ba dòng, hai cột:

| Cột trái | Cột phải |
|---|---|
| CPU Load | CPU Package temperature |
| RAM dùng/tổng và % | iGPU Load |
| Fan Mode | Fan 1 và Fan 2 (RPM) |

## Màu

Viền dùng **max(CPU %, RAM %)**. Dưới 50%: xanh Intel, chu kỳ sáng 2,4 giây; 50–79%: vàng, 1,4 giây; từ 80%: đỏ, 1,05 giây. Ví dụ CPU 15%, RAM 60% thì viền vàng. Nhiệt độ, iGPU và quạt không tham gia tính mức viền.

Nhãn CPU trong thanh thu gọn dùng cùng logic với nhãn RTX 3080 của AI Island: chỉ dựa vào tải CPU, dưới 50% xanh Intel cố định, 50–79% vàng nhấp nhịp, từ 80% đỏ nhấp nhịp; thiếu dữ liệu thì xám. Số % CPU và các số liệu còn lại giữ trắng RGB(242,242,245). Logo Intel giữ màu gốc, About giữ màu như AI Island.

## Nguồn số liệu

- CPU: PDH Processor(_Total).
- RAM vật lý: GlobalMemoryStatusEx; G trong thanh thu gọn tương ứng GiB.
- Intel iGPU: DXGI xác định LUID, PDH GPU Engine cộng process trên cùng engine rồi chọn engine bận nhất.
- CPU Package: MSR 0x1A2 (TjMax) trừ digital readout hợp lệ từ 0x1B1, qua module IntelMSR đã ký/PawnIO. Đã kiểm tra trên i9-11950H một package. Không đoán TjMax hoặc dùng nhiệt độ core/GPU thay thế.
- Quạt: **chỉ đọc** mapping Global\X1FanTelemetryV1 version 2 do X1FanService xuất. FILE_MAP_READ ở cả mở mapping và map view; không có hàm yêu cầu/chuyển chế độ. Kiểm tra sequence ổn định và tuổi dữ liệu dưới 5 giây; nguồn lỗi/mất thì hiện N/A. Nhiệt độ CPU không phụ thuộc dịch vụ quạt.

SYS không cài/chỉnh service, không điều khiển quạt. Chế độ quạt thay đổi bên AI Island sẽ tự phản ánh trên SYS. Nguồn thiếu/quyền thiếu hiện N/A.

## Build và cấu trúc

Visual Studio 2022 C++ Build Tools + Windows SDK. `build.bat` tạo EXE ở thư mục gốc, file trung gian ở build/. Có thể dùng `build.bat build\X1-SYS-Island.exe` khi bản cũ đang chạy. Thoát bản cũ trước khi chạy bản mới.

- x1_sys_island.cpp: giao diện, About, shortcut và hover.
- telemetry.h, cpu_temperature.h, fan_reader.h: bộ đọc nền, nhiệt độ và quạt chỉ đọc.
- assets/: ảnh nguồn và bitmap nhúng; không cần khi chỉ chạy EXE.
- licenses/: giấy phép/module source bắt buộc kèm khi phân phối.
- tests/: kiểm tra chức năng; không được biên dịch vào app.
- build/: output trung gian, chương trình test và preview mẫu; không có trong gói phân phối.

Chạy tests\test.bat để kiểm tra telemetry, tuổi/version dữ liệu quạt, chuyển đổi cảm biến. `tests\test.bat --require-temperature` cần PowerShell/Command Prompt Administrator. tests\test-hover.bat kiểm tra bố cục giá trị cực đại, viền, timer 1 giây/5 giây, rê lại, kéo/menu và ẩn thủ công; vị trí chuột được mô phỏng trên WndProc/cửa sổ/timer thật. Preview là dữ liệu mẫu để kiểm tra giao diện.

Đã build v0.5 không cảnh báo và khởi động lại bằng Administrator. Kiểm tra nhiệt độ CPU Package trực tiếp, CPU/RAM/iGPU, Fan Mode/Fan RPM thật, giao diện 560 px, About, Ctrl+Shift+D, không chồng AI Island và hover 1 giây/5 giây đều đạt. Ảnh kiểm tra giao diện thật ghi nhận CPU Package 60°C và Fan Mode BIOS Auto.

## v0.6.0 — giảm hoạt động nền và dung lượng

- Cắm sạc: lấy mẫu mỗi 1 giây. Chạy pin/Battery Saver hoặc không xác định được nguồn điện: mỗi 3 giây, màu viền/CPU đứng yên. Timer 1 giây trên pin chỉ giữ hành vi hover, không vẽ lại liên tục.
- Khi ẩn (kể cả hover) hoặc suspend: worker đóng bộ đọc PDH/PawnIO và chờ sự kiện, không lấy mẫu định kỳ. Khi hiện lại cần một chu kỳ lấy mẫu để có số liệu mới; trong thời gian đó số cũ có thể còn hiển thị.
- UI nhận thông báo khi có mẫu mới, bỏ timer đọc snapshot độc lập. Worker ưu tiên thấp hơn bình thường; bộ đệm GPU tái sử dụng.
- Exit báo dừng ngay, chờ worker kết thúc rồi đóng event, mutex và tài nguyên GDI. Query PDH và handle PawnIO được đóng bởi destructor; mapping quạt được unmap/close sau mỗi lần đọc. Không tạo tiến trình con.
- Không cài, start, stop hay xóa service. PawnIO và X1FanService là nguồn dùng chung có sẵn; đóng handle của SYS không đồng nghĩa unload driver PawnIO khỏi kernel. Không có cơ sở xác định SYS sở hữu riêng những service này, nên không tự dừng chúng khi Exit.
- Build ưu tiên kích thước, tối ưu toàn chương trình và loại mã không dùng; bỏ thư viện đọc file C++ stream khỏi đường nạp IntelMSR. Vẫn dùng Win32 và giữ yêu cầu Administrator cho cảm biến.
- `package.ps1` tạo gói chạy trong dist/, chỉ chứa EXE, IntelMSR.bin, README và giấy phép/source module bắt buộc. Không kèm build/, tests/, ảnh nguồn hoặc các ZIP phiên bản cũ. Không xóa bản lưu của người dùng.

Kiểm tra trên máy hiện tại: build không cảnh báo; test CPU/RAM/iGPU/quạt và giải mã cảm biến đạt. Test cửa sổ kiểm tra hover, ẩn/hiện, suspend/resume và tín hiệu dừng. Đọc nhiệt độ thật chưa xác nhận ở phiên không có quyền Administrator (trả N/A đúng dự kiến). Chưa đo watt, thời lượng pin hay khả năng unload của driver; IOCTL đồng bộ còn phụ thuộc driver trả về, không bảo đảm thời gian Exit nếu driver bị treo.
