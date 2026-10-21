[English](../../README.md) · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · [日本語](README.ja.md) · [한국어](README.ko.md)

<div align="center">

<img src="../assets/banner.svg" alt="S3 Serial — 无线串口调试器" width="100%" />

# S3 Serial

### 把 ESP32-S3 变成随身携带的 Wi-Fi 串口与 RS485 调试器。

**手机、电脑，一张网页。离线连接，查看每一个字节。**

[![Version](https://img.shields.io/badge/version-1.0.0-0e766e?style=flat-square)](CHANGELOG.md)
[![ESP32-S3](https://img.shields.io/badge/hardware-ESP32--S3-263844?style=flat-square)](docs/HARDWARE.md)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.5.1-c43b3b?style=flat-square)](docs/BUILD.md)
[![License](https://img.shields.io/badge/license-MIT-d4a437?style=flat-square)](LICENSE)

[快速开始](#快速开始) · [界面预览](#界面预览) · [硬件接线](../HARDWARE.md) · [使用指南](../USAGE.md) · [构建烧录](../BUILD.md) · [常见问题](../FAQ.md)

</div>

---

S3 Serial 基于 ESP-IDF，使用 ESP32-S3 的 UART1 连接 TTL 转 RS485 模块，通过本地 Wi-Fi 网页完成参数配置、HEX／文本收发、定时发送和日志导出。网页、脚本和样式全部嵌入固件，运行时不依赖互联网、云服务、npm 或外部 CDN。

> [!NOTE]
> v1.0.0 是首个公开版本。已完成构建、自动测试与此前实板启动验证；真实 RS485 从站往返、自动换向时序及长时间压力测试仍待完成。它提供原始串口调试与 Modbus CRC 辅助功能，尚未实现完整 Modbus 主站事务管理。

## 界面预览

<img src="../assets/desktop.jpg" alt="桌面端：串口参数、原始数据监视、发送与 CRC 预览" width="100%" />

<div align="center">
<img src="../assets/mobile.jpg" alt="手机宽度响应式页面：数据监视与发送区" width="320" />
</div>

以上为同一套网页的真实浏览器截图，均使用显式演示模式。手机图是在桌面浏览器中按手机视口捕获，未声称为实体手机测试。样例报文由浏览器生成，不代表已与真实从站通信。更多截图见 [界面与截图说明](../SCREENSHOTS.md)。

## 多语言

网页支持 English、简体中文、繁體中文、日本語、한국어，首次打开默认英语。右上角切换后会记住浏览器选择；切换不发送串口命令，不改动参数草稿或用户字节。历史日志、用户命令名称和设备原始诊断信息保留原文。

## 为什么使用 S3 Serial

| 功能 | v1.0.0 提供的能力 |
|---|---|
| 随手连接 | 有密码 Wi-Fi 热点，默认入口 `http://192.168.4.1`；支持 AP+STA |
| 串口参数 | 1200～115200，5～8 数据位，N/E/O 校验，1/2 停止位；常用下拉与自定义波特率 |
| 字节可见 | HEX、UTF-8、HEX+文本，TX/RX 区分，软件时间戳与 UART 错误计数 |
| Modbus 辅助 | CRC-16/MODBUS 预览与追加，低字节在前 |
| 重复操作 | 设备端定时发送、忙时跳过、32 条快捷命令及 JSON 导入导出 |
| 调试记录 | 浏览器 TXT/CSV 导出、有限历史恢复、丢失与淘汰提示 |
| 多端协作 | 最多 3 个调试页面，其中 1 个控制者、2 个观察者 |
| 离线网页 | 前端资源嵌入固件，电脑和手机共享响应式界面 |

## 快速开始

### 1. 准备硬件并接线

ESP32-S3-WROOM-1 开发板、EE Maker EKT010 TTL TO RS-485 模块、USB 数据线、杜邦线；测试真实通信还需要独立供电的 RS485 从站及其通信说明书。

| ESP32-S3 | 方向 | EKT010 | 本次线色 |
|---|:---:|---|---|
| 3V3 | → | VCC | 红色 |
| GND | — | GND | **蓝色** |
| GPIO5 / UART1 TX | → | RXD，模块输入 | **黑色** |
| GPIO6 / UART1 RX | ← | TXD，模块输出 | 黄色 |

**本模块蓝线是地线，黑线是 RXD。** 绿色端子 A+、B− 接从站对应信号，参考地按从站说明连接；B− 不是电源负极。从站按说明书单独供电。模块没有外接 DE/RE，本工程没有方向控制 GPIO。完整说明与布线图见 [硬件文档](../HARDWARE.md)。

### 2. 构建并烧录

安装并激活 **ESP-IDF v5.5.1**，在工程根目录运行：

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p YOUR_PORT flash monitor
```

将 `YOUR_PORT` 替换为实际端口，例如 Windows 的 `COM14` 或 Linux 的 `/dev/ttyACM0`。也可使用单独交付的固件包，详见 [构建与烧录](../BUILD.md)。当前分区使用 4 MB Flash 地址空间，不依赖 PSRAM。

### 3. 连接网页

在 USB 控制台查看首次启动信息，连接 `S3-Serial-xxxx` 热点，打开 **http://192.168.4.1**。设备会生成并保存随机密码；热点与网页登录使用同一密码，仓库没有通用固定密码。

登录后点击“申请控制权”，按从站手册核对波特率、校验位等参数，点击“应用参数”；需要跨重启保留时再点“保存到设备”。

### 4. 发送第一条请求

只有当从站地址为 1，并且支持从地址 `0x0000` 开始读取两个保持寄存器时，才使用以下示例：

```text
输入 HEX：01 03 00 00 00 02
勾选：追加 CRC-16/MODBUS
最终发送：01 03 00 00 00 02 C4 0B
```

如果已经输入完整八字节，就关闭 CRC 追加，避免重复添加。不要因为没有应答就直接判断模块损坏；先检查从站供电、A/B、串口参数、地址和寄存器。[完整 Modbus 示例 →](../MODBUS.md)

## 无硬件也能预览

```sh
python tools/preview.py
```

打开 `http://127.0.0.1:8765/?demo=1`。演示模式有醒目标记，所有收发在浏览器中模拟，不访问开发板。也可以在已烧录设备的地址后添加 `?demo=1` 查看演示；正常真实调试使用 `/`。

## 工作方式

```mermaid
flowchart LR
    B[手机 / 电脑浏览器] <-->|Wi-Fi · HTTP / WebSocket| W[ESP32-S3 网页服务]
    W <--> S[串口任务与有界历史]
    S <-->|UART1 · TX5 / RX6| T[EKT010]
    T <-->|RS485 · A+ / B−| D[目标从站]
```

网络与串口使用独立任务；控制者失联后撤销未开始的发送，已开始的报文允许完整发完。串口接收无需等待浏览器渲染。详细资源边界见 [架构设计](../ARCHITECTURE.md)。

## 测试与当前边界

| 项目 | 状态 |
|---|---|
| 前端 17 项测试，包含真实演示历史 CRC 回归 | 本地通过 |
| 串口 C11 逻辑套件 | 本地通过 |
| ESP-IDF v5.5.1 编译、链接与分区检查 | 通过，详见发行验证记录 |
| 电脑与手机视口页面展示 | 已截图验证 |
| 先前开发板启动、热点及 HTTP/WebSocket 初始化 | 已验证 |
| 本次公开版在开发板上烧录 | 以 [发行验证记录](../VERIFICATION.md) 为准 |
| 真实从站往返、快速应答、连续流与两小时运行 | 待实物验收 |

- “UART 已发完”不代表从站接收、解析或执行成功。
- RX 记录是读取的数据块，尚未按 Modbus RTU 完整帧分组，也不自动配对应答、校验 RX CRC 或解释异常码。
- 定时发送按周期调度，不等待从站应答。周期必须考虑从站处理与应答时间。
- 浏览器最多保留 5 MiB 原始字节 / 20,000 条；设备历史最多 32 KiB / 512 条，均有淘汰机制。断电丢失设备 RAM 历史。
- 尚未实现 BLE 网页通信、OTA 和持久离线采集。

更多测试命令与验收方法见 [测试说明](../../TESTING.md)。

## 文档导航

| 想了解什么 | 从这里开始 |
|---|---|
| 硬件型号、线色、GPIO、RS485 端子 | [硬件与接线](../HARDWARE.md) |
| 各按钮、控制权、参数保存、日志 | [使用指南](../USAGE.md) |
| 官方 SDK、源码构建、固件烧录与恢复 | [构建与烧录](../BUILD.md) |
| Modbus 请求、CRC、应答与轮询边界 | [Modbus 调试](../MODBUS.md) |
| 任务、资源限制和协议边界 | [架构](../ARCHITECTURE.md) · [接口协议](../../PROTOCOL.md) |
| 首次连接、乱码、无应答、升级后页面 | [常见问题](../FAQ.md) |
| 结果证据、未测范围与截图 | [验证记录](../VERIFICATION.md) · [截图说明](../SCREENSHOTS.md) |
| 发版、GitHub 上传与附件清单 | [发布指南](../PUBLISHING.md) · [发布说明](../RELEASE_v1.0.0.md) |
| 参与开发、报告问题 | [贡献指南](../../CONTRIBUTING.md) · [变更记录](../../CHANGELOG.md) |

## 许可证与致谢

项目原创代码、文档和自制图示采用 [MIT](../../LICENSE) 许可证，可使用、修改、分发和商用；再分发时保留版权与许可声明。ESP-IDF 及其他依赖保留各自许可，见 [第三方说明](../../THIRD_PARTY_NOTICES.md)。

感谢 Espressif 提供 ESP-IDF。仓库展示结构参考 [morluto/rea](https://github.com/morluto/rea) 的清晰导航和分层文档；本项目未使用其品牌素材或实现代码。
