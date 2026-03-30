# Zephyr 与 SDK 版本对齐

为减少「本地能编、CI 失败」或反之的差异，建议将 **本地环境** 与 **持续集成** 对齐到同一主线。

## CI 使用的 Zephyr 版本

GitHub Actions（`.github/workflows/ci.yml`）中：

- 容器镜像：`gcr.io/zephyr-project/zephyr-build:v<ZEPHYR_VERSION>`
- 环境变量 `ZEPHYR_VERSION`（例如 `3.6.0`）应与镜像标签一致。

合并或发版前，若升级 CI 中的 Zephyr 版本，请同步：

1. 本地 `ZEPHYR_BASE` 指向的 Zephyr 仓库检出 **兼容 tag 或分支**。
2. [Zephyr SDK](https://github.com/zephyrproject-rtos/sdk-ng/releases) 与 [Zephyr 文档](https://docs.zephyrproject.org/) 中该版本要求的工具链版本。
3. 本仓库 `README.md` 中关于前提条件 / CI 的说明（若有硬编码版本号）。

## West 工作区（可选）

若使用根目录 `west.yml` 管理 Zephyr：

- 将 `projects.zephyr.revision` 固定为 **tag**（如 `v3.6.0`）而非浮动的 `main`，便于复现构建。
- 首次 `west update` 后执行 `west zephyr-export`，与 `zephyr_config.env` 中的 `ZEPHYR_BASE` 一致。

## 应用版本（固件版本号）

固件展示用版本号与 **仓库 `APP_VERSION` 文件**、CMake、`Doxyfile`、README 的同步方式见 [VERSION_MANAGEMENT.md](VERSION_MANAGEMENT.md) 与脚本 `scripts/bump_version.py`。
