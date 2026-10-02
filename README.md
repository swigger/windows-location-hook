# Windows Location Hook

仅支持 **Windows x64**，不提供 x86 或 ARM64 构建。

## 下载

在仓库的 GitHub Releases 页面下载 `windows-location-hook-windows-x64.zip`，完整解压后使用：

- `lfhookcfg.exe`：配置程序，需要管理员权限。
- `LocationServiceHook.dll`：Hook DLL，必须与 `lfhookcfg.exe` 放在同一目录。
- `LocationDemo.exe`：定位演示程序。

运行环境需要安装 Microsoft Visual C++ x64 运行库（与 Visual Studio 2026 / MSVC v145 兼容的版本）。发布时同时提供 ZIP 的 `.sha256` 校验文件。

## 自动编译与发布

GitHub Actions 工作流位于 `.github/workflows/build-release.yml`：

- 推送任意分支、提交 PR 或手动运行工作流：编译 `Release|x64`，启动两个应用验证中文界面文字、输入错误提示和图标，校验三个二进制文件的 x64 架构，上传 ZIP 和 SHA-256 文件到 Actions artifacts，保留 14 天。
- 推送 `v*` 标签：构建成功后自动创建 GitHub Release，生成发布说明并上传相同的 ZIP 和 SHA-256 文件。
- 标签包含 `-`（例如 `v1.0.0-rc.1`）时，新建的 Release 标记为预发布。
- 重新运行标签工作流时，会更新已有 Release 的同名附件。

例如，提交并推送代码后发布版本：

```sh
git tag v1.0.0
git push origin v1.0.0
```

工作流使用 GitHub 自动提供的 `GITHUB_TOKEN`，无需配置额外密钥。仅发布任务拥有 `contents: write` 权限；PR 和手动运行不会发布 Release。

界面验证截图和控件文字保存在 `windows-x64-ui-check` artifact 中，便于人工检查。检查包含配置程序的输入校验和演示程序的模式切换，不会安装 Hook 或请求真实位置；它不替代 Windows 桌面上的完整定位功能验收。

## 本地编译

安装 Visual Studio 2026 的“使用 C++ 的桌面开发”工作负载、MSVC v145 和 Windows SDK，在开发者命令提示符中运行：

```bat
msbuild lfhookcfg.sln /m /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145
```

生成的程序位于 `x64\Release\`。CI 使用 `windows-2025-vs2026` runner 和相同的工具链。

三个项目的 Debug / Release 配置均显式启用 `/utf-8`，不依赖本机代码页或用户属性表。两个应用内嵌 16–256 像素的多尺寸图标，并设置大小窗口图标。图标源代码位于 `resources/generate_icons.py`；仅重新生成图标时需要 Python 和 Pillow。

在 Windows x64 的管理员 PowerShell 中可运行 `./scripts/test-windows-ui.ps1`，复现 CI 的界面检查并保存截图。
