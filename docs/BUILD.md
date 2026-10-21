# 构建、烧录与恢复

## 固定环境

- ESP-IDF v5.5.1，提交 fcae32885b0296b32044cb99ecbdc50d98dddb83。
- 芯片 ESP32-S3，CMake + Ninja，使用 SDK 配套 Python 与 Xtensa 工具链。
- Flash 4 MB 地址空间，DIO 40 MHz，3 MB factory 应用分区；PSRAM 关闭，无 OTA。
- Node.js 22 只用于主机测试；Python 3 用于预览与资源打包。最终固件不依赖 Node。

安装及激活环境请参考 [官方 ESP-IDF 指南](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/get-started/index.html)。源码工程不要包含他人的 sdkconfig、build 或开发工具链目录。

## 标准构建

在 ESP-IDF 终端的工程根目录运行：

```sh
idf.py set-target esp32s3
idf.py build
idf.py size
idf.py -p YOUR_PORT flash monitor
```

Windows 将 YOUR_PORT 替换成 COM14 等实际端口，Linux 可为 /dev/ttyACM0，macOS 可为 /dev/cu.usbmodem...。选择 USB 数据线和正确接口；部分板有原生 USB 与 USB 转 UART 两个接口。公开配置支持 UART0 控制台与 SDK 的 USB Serial/JTAG 次级控制台。

Windows 也可使用脚本（在 PowerShell 中执行）：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\build.ps1 `
  -IdfPath 'C:\Espressif\frameworks\esp-idf-v5.5.1' `
  -ToolsPath 'C:\Espressif' `
  -BuildDir "$PWD\build"
```

脚本只改变当前进程环境。修改 web/ 后重新构建，CMake 自动将资源 gzip 嵌入固件，无需 npm build。最简单的跨平台方式仍是官方激活终端中的 idf.py。

## 打包已编译固件

```sh
python tools/package_firmware.py --build-dir build --output-dir firmware
python tools/verify_firmware.py --firmware-dir firmware
```

使用 SDK Python 环境，其中包含本项目打包脚本所需的 esptool v4。镜像偏移与 Flash 设置来自该次 flasher_args.json，不手写另一套地址。正式附件由源代码构建后生成。

## 使用固件附件

下载 v1.0.0 固件包后，进入 firmware 目录，使用 SDK 环境执行：

```sh
python -m esptool --chip esp32s3 -p YOUR_PORT -b 460800 write_flash "@flash_args"
```

包内独立镜像：bootloader/bootloader.bin @ 0x0、partition_table/partition-table.bin @ 0x8000、s3_serial_debugger.bin @ 0x10000。地址以实际包内清单为准。只更新应用且分区兼容时可只写 0x10000 的应用。

merged-flash.bin 从 0x0 写入，包含间隙填充，会覆盖 NVS 内已有设备配置，密码随后重新生成。需要保留配置时使用独立镜像清单且确认分区兼容。固件包不含统一密码，也不含预填的 NVS 数据。

## USB 连接与恢复

若进入 downloading / waiting for download，先松开 BOOT，再按 RST；必要时重新插拔 USB。某些 ESP32-S3 与 Windows 原生 USB 控制线组合在自动复位后仍停留下载模式，不能据此认定写入失败。查看 esptool 的写入校验与随后应用启动日志分别判断。

首次启动应出现 Hotspot、Device / Wi-Fi password、Open http://192.168.4.1 与 HTTP/WebSocket ready。串口被占用时关闭其他串口监视器。无已保存串口配置时使用初始 115200/8N1；这不证明从站参数。

修改分区或切换固件前可先备份 Flash，并自行保管；不要把 Flash/NVS 备份上传到仓库。当前工程 NVS 格式异常时会报告维护需求，不会自动整片擦除。
