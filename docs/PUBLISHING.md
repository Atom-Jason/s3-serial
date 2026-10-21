# GitHub 发布指南

当前交付已经按公开仓库整理，但尚未代建远程仓库、上传或发布 Release。目标仓库名称和地址由维护者决定，参考仓库 morluto/rea 不是本项目的发布目标。

## 上传源码

1. 解压 s3-serial-v1.0.0-source.zip，进入含 README.md 的目录。
2. 在自己的 GitHub 账号创建空仓库，建议简短名称 s3-serial，但名称可自定。
3. 通过 Git 上传目录中的源码、docs、LICENSE、LICENSES 与隐藏的 .github 等文件；不要上传整个工作区。
4. 下方 YOUR_ACCOUNT/YOUR_REPOSITORY 仅为占位符，请替换为自己的仓库地址。

```sh
git init
git add .
git commit -m "Release S3 Serial v1.0.0"
git branch -M main
git remote add origin https://github.com/YOUR_ACCOUNT/YOUR_REPOSITORY.git
git push -u origin main
```

隐藏的 .github/workflows/ci.yml 为自动检查配置。首次推送后查看 Actions；本地执行通过不等于 GitHub 云端工作流已执行。没有仓库绑定信息前 README 不展示虚假的绿色 CI 徽章。

## 发布 v1.0.0

以提交创建 v1.0.0 标签，在 GitHub Releases 新建公开发行。标题 S3 Serial v1.0.0，正文使用 RELEASE_v1.0.0.md。附上单独固件包与对应 SHA256 清单，仓库源码已有 GitHub 自动生成的源码归档。

发行附件建议：

- s3-serial-v1.0.0-firmware.zip：独立镜像、合并镜像、偏移与哈希、使用说明、第三方许可。
- s3-serial-v1.0.0-source.zip：便于手动上传的干净源码，含说明及截图。
- s3-serial-v1.0.0-SHA256SUMS.txt：上述附件 SHA256。

当前工程的 .gitignore 忽略 firmware 与本地构建产物，固件更适合作为 Release 附件。不要将完整 Flash/NVS 备份、原始控制台日志、设备密码、工具链或私人附件加入仓库。

## 发布前核对

运行 python tools/check_release.py；执行前端与 C 测试，并完成 ESP-IDF 构建。核对页脚、接口、CMake、变更记录与固件都是 1.0.0。确认图片链接正确，未把浏览器演示和实体手机、真实从站验收混淆。维护者按实际测试更新 VERIFICATION.md，不提前勾选未测项目。
