# 参与贡献

欢迎修复问题、完善文档与补充实物测试。开发环境固定 ESP-IDF v5.5.1；前端测试使用 Node.js 22，辅助脚本使用 Python 3。

## 提交前检查

```sh
node --test tests/*.test.mjs
gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 -I main main/serial_core.c tests/serial_core_test.c -o /tmp/s3-serial-core-test
/tmp/s3-serial-core-test
python tools/check_release.py
idf.py build
```

Windows 下可将 C 测试输出路径替换为本地临时目录下的 `.exe`。构建与工具链配置见 docs/BUILD.md。

修改串口时序、控制权、队列、CRC 或历史逻辑时，应补充覆盖实际失效情形的测试。新增功能注明内存开销、断线行为与验证范围。硬件测试请记录型号、接线、波特率、校验位、发送字节、接收字节和持续时间；不要把浏览器模拟截图当作真实总线证据。

提交 issue 时使用仓库模板。日志中删除设备密码、Cookie、路由器密码和私人网络信息。贡献内容应具有相应提交权限，并按本项目 MIT 许可证提供；依赖继续保留其原有许可。
