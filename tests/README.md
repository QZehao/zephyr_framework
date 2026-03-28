# 单元测试指南

本目录包含 Zephyr 事件驱动项目模板的单元测试。

## 目录结构

```
tests/
├── CMakeLists.txt          # 测试构建配置
├── prj.conf                # 测试项目配置
├── test_event_system.c     # 事件系统测试
├── test_event_queue.c      # 事件队列测试
├── test_module_manager.c   # 模块管理器测试（待添加）
└── test_sys_memory.c       # 内存系统测试（待添加）
```

## 运行测试

### 使用 West

```bash
# 构建并运行测试（native_posix）
west build -b native_posix tests/
west test

# 或一步完成
west test -b native_posix tests/
```

### 使用 CMake

```bash
# 创建构建目录
mkdir build_tests && cd build_tests

# 配置
cmake -DBOARD=native_posix ../tests

# 构建
cmake --build .

# 运行测试
ctest
```

## 编写新测试

### 测试模板

```c
#include <zephyr/ztest.h>
#include <被测试模块.h>

/**
 * @brief 测试用例
 */
ZTEST(test_module_name, test_feature)
{
    // 准备测试数据
    
    // 执行测试
    
    // 断言结果
    zassert_equal(expected, actual, "错误消息");
    zassert_not_null(pointer, "指针不应为空");
    zassert_true(condition, "条件应为真");
}

/**
 * @brief 测试套件
 */
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
| `zassert_isa(instance, class, msg)` | 断言类型 |

## 测试覆盖率

### 使用 gcov

```bash
# 构建时启用覆盖率
west build -b native_posix tests/ -- -DCMAKE_C_FLAGS="--coverage"

# 运行测试
west test

# 生成报告
gcovr -r .. --html --html-details coverage.html
```

### 查看报告

打开 `coverage.html` 查看详细的覆盖率报告。

## 持续集成

测试会自动在 GitHub Actions 中运行。查看 `.github/workflows/ci.yml` 了解配置。

## 最佳实践

1. **测试隔离**：每个测试应独立运行，不依赖其他测试的状态
2. **清理资源**：测试后清理分配的资源
3. **有意义的名称**：测试函数名应描述测试内容
4. **边界条件**：测试边界值和异常情况
5. **可重复性**：测试应始终产生相同的结果

## 故障排除

### 测试失败

```bash
# 详细输出
west test -v

# 只运行特定测试
west test -T test_event_system
```

### 内存泄漏

```bash
# 启用内存跟踪
CONFIG_HEAP_MEM_POOL_SIZE=32768
CONFIG_STACK_SENTINEL=y
```

## 参考文档

- [Zephyr 测试框架](https://docs.zephyrproject.org/latest/develop/test.html)
- [ztest API 参考](https://docs.zephyrproject.org/latest/develop/test/test_api_reference.html)
