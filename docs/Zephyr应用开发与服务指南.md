# Zephyr 应用开发与服务指南

本文面向**在 Zephyr 上编写应用代码**的开发者，归纳常用内核能力、设备与配置工作流，以及编写**系统级服务**（后台线程、定时、同步、资源管理）时的注意点。细节以当前 Zephyr 版本官方文档为准；本仓库特有封装见 **[系统服务使用说明.md](系统服务使用说明.md)**、**[事件系统详细使用说明.md](事件系统详细使用说明.md)**。

**前置**：[环境搭建与配置指南.md](环境搭建与配置指南.md) · [独立应用构建说明.md](独立应用构建说明.md) · [设备树与内存配置手册.md](设备树与内存配置手册.md)（按需）

---

## 1. 分层与职责

| 层次 | 典型内容 | 应用开发者关注点 |
|------|----------|------------------|
| **应用 / 业务** | `main()`、`app_main`、模块、状态机 | 少阻塞、少在中断里做重活 |
| **本模板服务** | `sys_*`、事件总线、模块管理器 | 与 `CONFIG_*` 对齐，见 **[项目配置项说明.md](项目配置项说明.md)** |
| **Zephyr 内核** | 线程、调度、同步、定时器、堆 | 栈大小、优先级、超时 |
| **驱动 / 子系统** | GPIO、UART、I2C、Flash… | `device` 句柄、**`DT`** 节点、错误码 |
| **硬件** | SoC、外设 | 时钟、引脚、DMA、MPU（与 overlay 相关） |

---

## 2. 线程与调度

- **线程**：`k_thread_create()` / `K_THREAD_DEFINE`；每个线程需**独立栈**，大小在 `prj.conf` 中常通过 **`CONFIG_MAIN_STACK_SIZE`**、**`CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE`** 或自定义线程属性配置。  
- **优先级**：数值**越小优先级越高**（与部分 RTOS 习惯一致，以 Zephyr 文档为准）；避免大量同优先级线程饥饿，谨慎使用**忙等**。  
- **协作式 vs 抢占式**：取决于 `CONFIG_NUM_COOP_PRIORITIES` / `CONFIG_NUM_PREEMPT_PRIORITIES` 等；不了解时不要混用假设。  
- **系统工作队列**：`k_work` 提交到 **`system workqueue`**，适合**非实时**、可延迟执行的短任务；别在 work 里长时间持锁或阻塞。

**实践**：长耗时任务单独线程；与硬件紧耦合的用高优先级线程或中断+队列。

---

## 3. 同步与通信（常用）

| 机制 | 典型用途 |
|------|----------|
| **`k_mutex`** | 保护共享可变状态（同优先级线程间）；持锁时间要短。 |
| **`k_sem`** | 计数、信号、线程间唤醒；注意初始 count 与上限。 |
| **`k_msgq` / `k_fifo` / `k_pipe`** | 结构化数据流、生产者–消费者。 |
| **`k_poll`** | 多事件等待（socket、fd、内核对象等场景）。 |

**注意**：**ISR** 中只能使用 **ISR 安全** API（如 `k_sem_give` 部分场景）；复杂逻辑放到线程里处理。

---

## 4. 时间与定时器

- **`k_sleep()` / `k_msleep()`**：线程让出 CPU；精度受 tick 配置影响（**`CONFIG_SYS_CLOCK_TICKS_PER_SEC`**、tickless 等）。  
- **`k_timer`**：基于内核定时器的周期性/一次性回调；回调上下文规则见文档。  
- **超时**：大量阻塞 API 接受 **`k_timeout_t`**（如 `K_MSEC(n)`、`K_FOREVER`），避免死锁要设计超时路径。

本模板 **`sys_timer`** 在 Zephyr 定时能力之上做封装，见 **[系统服务使用说明.md](系统服务使用说明.md)**。

---

## 5. 内存

- **`k_malloc()` / `k_free()`**：使用内核堆，大小由 **`CONFIG_HEAP_MEM_POOL_SIZE`** 等决定；与 **SRAM** 总预算相关，见 **[设备树与内存配置手册.md](设备树与内存配置手册.md)**。  
- **`k_heap`**：可在静态缓冲或指定内存区上建独立堆（多段 RAM 场景）。  
- **栈溢出**：开启 **`CONFIG_THREAD_STACK_INFO`**、`CONFIG_THREAD_NAME` 等便于调试；合理设置各线程栈。

本模板 **`sys_memory`** 提供多池与统计，见 **[系统服务使用说明.md](系统服务使用说明.md)**。

---

## 6. 日志（Zephyr `LOG`）

- 在模块内 **`LOG_MODULE_REGISTER(name)`**，使用 **`LOG_INF` / `LOG_ERR`** 等；级别由 **`CONFIG_LOG`** 及模块 **`LOG_LEVEL`** 控制。  
- 默认输出依赖 **UART/console** 与 **`prj.conf`** 中的 **`CONFIG_LOG`**、**`CONFIG_CONSOLE`** 等。  
- 高吞吐场景注意**栈**与**缓冲区**；量产可关闭调试级别以减 Flash/RAM。

与模板 **`sys_log`** 的关系：可在业务层统一封装，便于内存环缓与模块名字段，二者可并存，注意**不要重复刷屏**。

---

## 7. Shell（可选）

启用 **`CONFIG_SHELL`** 及相关子命令后，可在串口使用交互命令做调试。会占用 **Flash/RAM** 与**一个会话上下文**；发布镜像常关闭或裁剪命令集。

---

## 8. 设备与驱动（应用侧用法）

1. **Devicetree** 中节点带 **`status = "okay"`** 且驱动已启用时，在 C 代码中用 **`DEVICE_DT_GET()`** / **`DEVICE_DT_GET_ONE()`** 等获取 **`const struct device *`**。  
2. 使用前 **`device_is_ready(dev)`**。  
3. 调用 **`gpio_pin_configure`**、**`i2c_transfer`** 等 API 时传入 **`dev`** 与 **DT 宏**指定的引脚/通道（**`GPIO_DT_SPEC_GET`** 等）。  
4. 错误码多为 **负 errno**（`-EIO`、`-ETIMEDOUT` 等），需向上传递或记录日志。

**设备树**：应用级修改优先 **`app.overlay`**，见 **[设备树与内存配置手册.md](设备树与内存配置手册.md)**。

---

## 9. Kconfig 与 `prj.conf` 工作流

- **`prj.conf`**：写 **`CONFIG_*=y`** 或赋值；合并进最终 **`.config**。  
- **`Kconfig`**（应用根目录等）：定义本应用可见的菜单项；通过 **`rsource`** 拆分到子目录。  
- 修改后若行为异常：**`west build -p always`** 或 **`pristine`**；复杂项用 **`menuconfig` / `guiconfig`**。

本应用扩展项释义见 **[项目配置项说明.md](项目配置项说明.md)**。

---

## 10. 中断与 ISR

- ISR 要**极短**：清标志、拷贝数据到缓冲区、**释放信号量/提交 work** 等。  
- 若 SoC 支持 **优先级嵌套**，注意与临界区、**`irq_lock`** 的配合。  
- **Never** 在 ISR 里调用可能阻塞或分配堆的 API（除非文档明确 ISR-safe）。

---

## 11. 编写「服务」时的通用模式

「服务」在此指长期运行、对外提供 API 的后台能力（类似本模板的 **`sys_*`**、IPC 工作线程）：

1. **明确初始化阶段**：`SYS_INIT` 或 `app_main` 中单次初始化，返回错误要可恢复或记录。  
2. **资源归属**：堆、静态缓冲、句柄的生命周期清晰；停止路径要**释放/注销**（若适用）。  
3. **线程模型**：单线程串行处理 vs 多线程；若多线程共享状态，**统一用 mutex 或消息队列**，避免裸全局无锁。  
4. **与事件系统协作**：慢业务走 **发布–订阅**，不要在回调里阻塞 IPC/网络。  
5. **可配置性**：通过 **`Kconfig`** 暴露栈大小、队列深度、超时，便于不同板型裁剪。

---

## 12. 与本仓库文档的衔接

| 主题 | 文档 |
|------|------|
| 本模板 `sys_*` | [系统服务使用说明.md](系统服务使用说明.md) |
| 事件与模块 | [事件系统详细使用说明.md](事件系统详细使用说明.md) · [模块系统详细使用说明.md](模块系统详细使用说明.md) |
| Thread IPC | [Thread_IPC服务使用说明.md](Thread_IPC服务使用说明.md) |
| 内存布局 / overlay | [设备树与内存配置手册.md](设备树与内存配置手册.md) |
| OTA / 存储 / PM | [OTA与存储扩展指南.md](OTA与存储扩展指南.md) |

---

## 13. 推荐阅读（官方）

- [Kernel Services](https://docs.zephyrproject.org/latest/kernel/services/index.html)  
- [Device Driver Model](https://docs.zephyrproject.org/latest/kernel/drivers/index.html)  
- [Build and Configuration Systems](https://docs.zephyrproject.org/latest/build/index.html)  
- [Application Development](https://docs.zephyrproject.org/latest/application/index.html)  

---

*本文随 Zephyr 大版本演进；若 API 与官方不一致，以官方为准。*
