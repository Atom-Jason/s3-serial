# 测试说明

## 自动测试

```sh
node --test tests/*.test.mjs
gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 -I main main/serial_core.c tests/serial_core_test.c -o /tmp/s3-serial-core-test
/tmp/s3-serial-core-test
python tools/check_release.py
idf.py build
```

前端 17 项测试覆盖五种语言文案与占位符完整性、英语默认值、语言偏好保存、切换后字节保持一致、严格 HEX/Base64、标准 CRC 向量、演示历史中的真实请求和应答 CRC、位宽及长度限制、定时边界、跨块 UTF-8、日志淘汰、导出和宏导入。C11 套件覆盖 CRC、0～1024 字节 Base64、位宽、线时间和环形历史。

`check_release.py` 检查公开文档的本地链接、必要图片、版本一致性与公开目录敏感内容。它不能证明不存在所有秘密或漏洞。

## 实物验收

1. 按 docs/HARDWARE.md 核对线色与 GPIO，确认 TTL 电平。
2. 断开转换模块，GPIO5/6 做 TTL 回环，逐字节比较。
3. 接回模块，核对从站电源、A/B、8 数据位、1 停止位以及实际波特率、校验位。
4. 单次发送已知只读请求，记录请求/应答并校验 CRC 与寄存器内容。
5. 使用可配置测试节点验证快速应答、尾字节和方向释放。
6. 115200/8N1 已知连续流十分钟，1 控制者加 2 观察者，以独立计数与校验对照。该条件不代表实际从站参数。
7. 验证断线、锁屏、后台、撤销控制、日志淘汰、参数保存与断电重启。
8. 连续运行两小时，检查内存、UART 错误、看门狗复位和掉线情况。

每项按实测结果记录，不将“已启动”“已发送”写成通信成功。当前结果见 docs/VERIFICATION.md。
