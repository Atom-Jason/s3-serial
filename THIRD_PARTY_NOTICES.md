# 第三方组件与许可

本仓库原创代码、文档与自制图示采用根目录 MIT 许可证。此声明不重新许可依赖或硬件资料。

构建依赖 ESP-IDF v5.5.1，固定提交 `fcae32885b0296b32044cb99ecbdc50d98dddb83`。SDK 由使用者独立获取，没有将 SDK 源码复制进本仓库。固件会链接 SDK 的相关组件。

| 组件 | 上游许可来源 |
|---|---|
| ESP-IDF | 上游 LICENSE，Apache-2.0 |
| FreeRTOS 内核 | SDK 中 FreeRTOS-Kernel/LICENSE.md |
| lwIP | SDK 中 lwip/COPYING |
| cJSON | SDK 中 cJSON/LICENSE |
| Mbed TLS | SDK 中 mbedtls/LICENSE |
| Espressif Wi-Fi / PHY 库 | SDK 对应 lib/LICENSE |

保留的原始许可文本与构建组件清单放在 `LICENSES/`。具体文件以对应 SDK 提交中的许可和版权声明为准；修改或重新分发 SDK 组件时应继续保留上游要求的声明。

仓库横幅与接线图为本项目自行绘制；截图来自本项目网页。文档结构参考 morluto/rea，未复制其品牌图像或产品实现代码。README 的外部状态徽章由 shields.io 提供，仅用于 GitHub 展示，固件运行不请求这些资源。
