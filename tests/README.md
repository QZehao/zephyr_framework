# 单元测试指南

本目录包含 Zephyr 事件驱动项目模板的单元测试（ztest），与主应用共享 `../src/` 下的实现；**不**编译 `app_main`、示例业务模块及 `ipc_service/` 源文件（测试用 `tests/prj.conf` 关闭 `CONFIG_THREAD_IPC_SERVICE`）。

## 目录结构

```
tests/
├── CMakeLists.txt          # 测试构建：链接 core / services / module_manager + 测试用例
├── Kconfig                 # rsource 复用仓库根目录 Kconfig
├── prj.conf                # 测试专用 Zephyr 配置
├── test_event_system.c
├── test_event_queue.c
├── test_module_manager.c
├── test_sys_memory.c
└── test_sys_timer.c
```

## 运行测试

### 前提

- 已设置 `ZEPHYR_BASE`，或已在仓库根目录配置 `zephyr_config.env`（与主工程相同）。

### West

```bash
# 构建（native_posix）
west build -b native_posix tests/ --build-dir build_tests

# 运行（退出码 0 表示通过）
west build -t run --build-dir build_tests
```

在 Linux/macOS 上也可直接执行 `build_tests/zephyr/zephyr`。

### CMake（可选）

```bash
mkdir build_tests && cd build_tests
cmake -DBOARD=native_posix -GNinja ..
ninja
./zephyr/zephyr
```

## 编写新测试

### 测试模板

```c
#include <zephyr/ztest.h>
#include <被测试模块.h>

ZTEST(test_module_name, test_feature)
{
    zassert_equal(expected, actual, "错误消息");
}

ZTEST_SUITE(test_module_name, NULL, NULL, NULL, NULL, NULL);
```

### 常用断言宏

| 宏 | 描述 |
|----|------|
| `zassert_equal(expected, actual, msg)` | 断言相等 |
| `zassert_not_equal(expected, actual, msg)` | 断言不相等 |
| `zassert_true(condition, msg)` | 断言条件为真 |
| `zassert_false(condition, msg)` | 断言条件为假 |
| `zassert_null(pointer, msg)` | 断言指针为空 |
| `zassert_not_null(pointer, msg)` | 断言指针非空 |

## 测试覆盖率

### 使用 gcov

```bash
west build -b native_posix tests/ --build-dir build_tests -- -DCMAKE_C_FLAGS="--coverage"
west build -t run --build-dir build_tests
gcovr -r .. --html --html-details coverage.html
```

## 持续集成

`native_posix` 测试在 GitHub Actions 的 `build-tests` 任务中执行；见 `.github/workflows/ci.yml`。

## 参考文档

- [Zephyr 测试框架](https://docs.zephyrproject.org/latest/develop/test.html)
- [ztest API 参考](https://docs.zephyrproject.org/latest/develop/test/test_api_reference.html)
