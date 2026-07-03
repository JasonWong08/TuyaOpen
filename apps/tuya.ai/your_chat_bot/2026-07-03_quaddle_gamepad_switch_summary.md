# 2026-07-03 Quaddle 手柄总开关记录

## 背景

检查 `your_chat_bot` 后发现，项目已有 `CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL`，但它只控制 BLE HID central 的连接链路。手柄事件到机器狗 UART 的 `quaddle_robot_bridge.c` 跟随 `CONFIG_ENABLE_CHAT_BOT_ROBOT_SECOND_UART` 编译，并且会注册 `quaddle_evt` 测试入口，因此原先没有一个同时覆盖“手柄连接 + 手柄操控”的总开关。

## 本次改动

- 新增 Kconfig 总开关：`CONFIG_ENABLE_QUADDLE_GAMEPAD`。
- `CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL` 改为依赖 `CONFIG_ENABLE_QUADDLE_GAMEPAD && CONFIG_ENABLE_BLUETOOTH`。
- CMake 在总开关打开时定义 `ENABLE_QUADDLE_GAMEPAD=1`。
- 总开关关闭时：
  - 不编译 BLE HID central。
  - 不注册 `quaddle_evt` CLI。
  - `quaddle_robot_bridge_handle_line()` 返回 `OPRT_NOT_SUPPORTED`，不接受手柄事件控制。
  - `quaddle_robot_bridge_gamepad_active_remaining_ms()` 固定返回 0，不阻塞 ASR/MCP 机器人控制。
- 保留 `CONFIG_ENABLE_CHAT_BOT_ROBOT_SECOND_UART` 下的 UART、ASR、MCP 机器人控制能力。

## 使用方式

### 宏定义关系

`CONFIG_ENABLE_QUADDLE_GAMEPAD` 是总开关，控制手柄连接与手柄操控功能。

`CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL` 是 BLE HID central 子功能开关，依赖总开关。只有总开关和蓝牙都打开时，BLE 手柄扫描、连接、订阅和 HID 报文解析才会启用。

依赖关系：

```text
CONFIG_ENABLE_CHAT_BOT_ROBOT_SECOND_UART=y
CONFIG_ENABLE_QUADDLE_GAMEPAD=y
CONFIG_ENABLE_BLUETOOTH=y
CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL=y
```

其中 `CONFIG_ENABLE_CHAT_BOT_ROBOT_SECOND_UART` 是机器狗 UART 控制链路基础能力；关闭 `CONFIG_ENABLE_QUADDLE_GAMEPAD` 不会关闭 ASR/MCP/机器人 UART 控制。

### 下载源码后在哪里修改

推荐直接修改 `apps/tuya.ai/your_chat_bot/app_default.config`，这样别人下载源码后在自己电脑本地首次构建时会自动生效。

`apps/tuya.ai/your_chat_bot/config/AI_BOARD.config` 不是普通 `tos.py build` 的直接输入。它是 AI_BOARD 预设模板，只有在重新选择 AI_BOARD 配置，或手动把这个预设复制成 `app_default.config` 时，才会参与并覆盖 `app_default.config`。

普通直接编译链路：

```text
app_default.config -> .build/cache/using.config -> CMake/编译
```

重新选择 AI_BOARD 预设时的链路：

```text
config/AI_BOARD.config -> app_default.config -> .build/cache/using.config -> CMake/编译
```

因此，如果分发方式是让别人下载源码后直接修改 `app_default.config` 并执行 `tos.py build`，只改 `app_default.config` 就可以，`config/AI_BOARD.config` 中的手柄开关不会影响本次构建。

只有在希望别人后续执行配置选择流程、重新选择 AI_BOARD 预设后仍保持同样默认行为时，才建议同步修改：

```text
apps/tuya.ai/your_chat_bot/config/AI_BOARD.config
```

当前项目为了保持 AI_BOARD 原有预设行为，`app_default.config` 和 `config/AI_BOARD.config` 默认都打开了手柄功能。但这不表示两个文件必须永远同步；当前本地直接构建以 `app_default.config` 为准。

### 按官方流程使用 AI_BOARD 预设编译

TuyaOpen 官方项目编译说明的基本流程是：进入项目目录，执行 `tos.py config choice` 选择已验证的硬件配置，再执行 `tos.py build` 编译工程。官方文档地址：

```text
https://tuyaopen.ai/zh/docs/quick-start/project-compilation
```

如果希望通过 AI_BOARD 预设来生成当前工程配置，完整流程如下。

#### 1. 先修改 AI_BOARD 预设文件

编辑：

```text
apps/tuya.ai/your_chat_bot/config/AI_BOARD.config
```

开启手柄连接和操控时，确保包含：

```text
CONFIG_ENABLE_CHAT_BOT_ROBOT_SECOND_UART=y
CONFIG_ENABLE_BLUETOOTH=y
CONFIG_ENABLE_QUADDLE_GAMEPAD=y
CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL=y
```

关闭手柄连接和操控时，确保不再设置手柄总开关和 BLE HID 子开关：

```text
CONFIG_ENABLE_CHAT_BOT_ROBOT_SECOND_UART=y
# CONFIG_ENABLE_QUADDLE_GAMEPAD is not set
# CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL is not set
```

如果文件里已有下面两行，需要删除或注释掉：

```text
CONFIG_ENABLE_QUADDLE_GAMEPAD=y
CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL=y
```

#### 2. 选择 AI_BOARD 预设

从仓库根目录初始化环境：

```bash
. ./export.sh
```

进入 `your_chat_bot` 项目目录：

```bash
cd apps/tuya.ai/your_chat_bot
```

执行官方配置选择命令：

```bash
tos.py config choice
```

在列表中选择：

```text
AI_BOARD.config
```

选择完成后，工具会把：

```text
config/AI_BOARD.config
```

复制为：

```text
app_default.config
```

并重新生成当前工程使用的配置。也就是说，真正参与后续编译的是选择流程生成后的 `app_default.config` / `.build/cache/using.config`。

#### 3. 编译工程

继续在 `apps/tuya.ai/your_chat_bot` 目录执行：

```bash
tos.py build
```

编译成功后，固件输出位于：

```text
apps/tuya.ai/your_chat_bot/dist/
```

#### 4. 检查配置是否真的生效

选择预设并编译后，可以检查当前构建配置：

```bash
grep -n "CONFIG_ENABLE_QUADDLE_GAMEPAD\|CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL" .build/cache/using.config
```

这条命令的含义：

- `grep`：在文件中搜索文本。
- `-n`：显示匹配内容所在的行号，方便定位。
- `CONFIG_ENABLE_QUADDLE_GAMEPAD`：搜索手柄总开关。
- `CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL`：搜索 BLE HID 手柄连接子开关。
- `\|`：表示“或者”，也就是匹配两个配置项中的任意一个。
- `.build/cache/using.config`：TuyaOpen 当前这次构建实际使用的配置缓存文件。

这条命令的用途是确认：修改后的手柄配置是否真的进入了本次构建使用的 `using.config`。

开启手柄时应看到：

```text
CONFIG_ENABLE_QUADDLE_GAMEPAD=y
CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL=y
```

关闭手柄时不应再出现这两项为 `y`。

如果结果不是预期值，通常说明配置选择前没有先改 `config/AI_BOARD.config`，或旧缓存没有刷新。可以重新执行 `tos.py config choice` 选择 `AI_BOARD.config`，然后再执行 `tos.py build`。

`tos.py config choice` 内部会先执行 full clean，再把选中的 `.config` 复制成 `app_default.config`，并强制重新生成 `.build/cache/using.config`。因此，走配置选择流程时一般不需要手动删除 `using.config`。

#### 非交互环境的等价做法

`tos.py config choice` 是交互式命令，不适合 CI、云端或自动化脚本。非交互环境中，可以在确认 `config/AI_BOARD.config` 已经改好后，直接把它复制到 `app_default.config`，再重新构建：

```bash
cd apps/tuya.ai/your_chat_bot
cp config/AI_BOARD.config app_default.config
tos.py clean -f
tos.py build
```

这等价于“选择 AI_BOARD 预设后再编译”。

其中 `tos.py clean -f` 是 TuyaOpen 提供的 full clean 命令，会删除当前项目的 `.build/` 目录，包括旧的 `.build/cache/using.config`。下一次 `tos.py build` 会根据当前 `app_default.config` 重新生成 `using.config`。

### 开启手柄连接和操控

在 `apps/tuya.ai/your_chat_bot/app_default.config` 中保留或添加：

```text
CONFIG_ENABLE_CHAT_BOT_ROBOT_SECOND_UART=y
CONFIG_ENABLE_QUADDLE_GAMEPAD=y
CONFIG_ENABLE_BLUETOOTH=y
CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL=y
```

如果只是直接执行 `tos.py build`，只需要修改 `app_default.config`。如果要通过“重新选择 AI_BOARD 预设”的方式生成配置，需要先把同样配置写入 `config/AI_BOARD.config`，再执行选择 AI_BOARD 预设的流程；选择流程会把 `config/AI_BOARD.config` 复制到 `app_default.config`，之后才会进入编译。

注意：如果已经选择过 AI_BOARD 预设，再去修改 `config/AI_BOARD.config`，不会自动反向更新当前的 `app_default.config`。这种情况下需要重新选择 AI_BOARD 预设，或直接修改 `app_default.config`。

生效后：

- BLE HID central 会编译进固件。
- 设备会扫描并连接支持的 BM769/Q34B/GamepadSpace 手柄。
- 手柄事件会进入 `quaddle_robot_bridge_handle_line()` 并映射为机器狗 UART 指令。
- `quaddle_evt` CLI 测试命令会注册，可用于手动模拟手柄事件。

### 关闭手柄连接和操控

在 `apps/tuya.ai/your_chat_bot/app_default.config` 中改成：

```text
# CONFIG_ENABLE_QUADDLE_GAMEPAD is not set
# CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL is not set
```

如果文件里原来有下面两行，需要删除或注释掉，避免再次打开：

```text
CONFIG_ENABLE_QUADDLE_GAMEPAD=y
CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL=y
```

如果只是直接执行 `tos.py build`，只需要修改 `app_default.config`。如果要通过“重新选择 AI_BOARD 预设”的方式生成关闭手柄的配置，需要先把同样修改写入 `config/AI_BOARD.config`，再执行选择 AI_BOARD 预设的流程；选择流程会把 `config/AI_BOARD.config` 复制到 `app_default.config`，之后才会进入编译。

注意：如果已经选择过 AI_BOARD 预设，再去修改 `config/AI_BOARD.config`，不会自动反向更新当前的 `app_default.config`。这种情况下需要重新选择 AI_BOARD 预设，或直接修改 `app_default.config`。

关闭后：

- BLE HID central 不会编译进固件。
- 不会扫描或连接 BLE 手柄。
- 不会注册 `quaddle_evt` CLI 测试命令。
- `quaddle_robot_bridge_handle_line()` 会返回 `OPRT_NOT_SUPPORTED`，不接受手柄事件控制。
- `quaddle_robot_bridge_gamepad_active_remaining_ms()` 固定返回 0，不会因为手柄优先级阻塞 ASR/MCP 控制。
- 机器人 UART、语音 ASR、MCP `self.robot.send_command` 仍然可用。

### 本地修改后如何构建生效

从仓库根目录初始化环境：

```bash
. ./export.sh
```

进入项目目录构建：

```bash
cd apps/tuya.ai/your_chat_bot
tos.py build
```

如果之前已经构建过，`.build/cache/using.config` 里可能还保留旧配置。普通 `tos.py build` 调用 `init_using_config(force=False)`，当 `using.config` 已经存在时不会强制从新的 `app_default.config` 重新生成。因此，修改 `app_default.config` 后，建议先刷新构建缓存，再编译。

推荐使用 TuyaOpen 的 full clean 命令：

```bash
tos.py clean -f
tos.py build
```

`tos.py clean -f` 会删除当前项目的 `.build/` 目录，包括旧的 `.build/cache/using.config`。随后 `tos.py build` 会按照新的 `app_default.config` 重新生成 `using.config` 并参与编译。

如果只想做最小清理，也可以只删除配置缓存文件：

```bash
rm -f .build/cache/using.config
tos.py build
```

不过从 TuyaOpen CLI 的正规流程看，`tos.py clean -f` 更稳妥，因为它会同步清掉 CMake/Ninja 等构建缓存，避免旧配置残留。

为了确认本地修改已经进入构建，可以检查：

```bash
grep -n "CONFIG_ENABLE_QUADDLE_GAMEPAD\|CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL" .build/cache/using.config
```

这条命令会在当前构建实际使用的 `using.config` 中搜索手柄总开关和 BLE HID 子开关；`-n` 会显示行号，`\|` 表示匹配两个配置名中的任意一个。

开启时应看到：

```text
CONFIG_ENABLE_QUADDLE_GAMEPAD=y
CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL=y
```

关闭时应看到两项未设置，或至少不再出现 `CONFIG_ENABLE_QUADDLE_GAMEPAD=y` 和 `CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL=y`。

如果检查结果仍是旧值，说明本地构建缓存还没刷新。优先执行 `tos.py clean -f` 后重新 `tos.py build`。

### 代码侧生效点

- `Kconfig`：新增 `CONFIG_ENABLE_QUADDLE_GAMEPAD`，并让 `CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL` 依赖它。
- `CMakeLists.txt`：总开关打开时定义 C 宏 `ENABLE_QUADDLE_GAMEPAD=1`。
- `quaddle_robot_bridge.c`：用 `ENABLE_QUADDLE_GAMEPAD` 包住手柄 CLI、手柄事件处理、手柄优先级和宏播放相关轮询。
- `quaddle_ble_hid_central.c`：继续由 `ENABLE_QUADDLE_BLE_HID_CENTRAL` 控制，且该 Kconfig 选项受总开关约束。
- `tuya_main.c`：只有启用 `ENABLE_QUADDLE_BLE_HID_CENTRAL` 时才使用 `NETCONN_CMD_NETCFG_PREPARE`，等待 BOOT 短按显式启动配网；关闭手柄/BLE HID central 时恢复为 `NETCONN_CMD_NETCFG`，上电后直接启动普通 Tuya BLE 配网。

## 2026-07-03 追加：关闭手柄后的配网恢复

关闭手柄后测试发现：

- `using.config` 中 `CONFIG_ENABLE_QUADDLE_GAMEPAD` 已关闭。
- 固件启动日志中没有 `quaddle ble hid:` 相关日志，说明 BLE HID central 和 BOOT 短按配网代码没有编译进来。
- 但 `tuya_main.c` 仍然调用 `NETCONN_CMD_NETCFG_PREPARE`，日志显示：

```text
wifi netcfg prepared, waiting for explicit start
```

这表示 Tuya BLE 配网只完成了 prepare，没有真正执行 `netcfg_start()`。在手柄功能开启时，这一步由 `quaddle_ble_hid_central.c` 的 BOOT 短按流程补上；但手柄关闭后该模块不再编译，BOOT 短按启动配网的入口也不存在。

因此手机 App 仍可能发现设备并建立 BLE 连接，但完整配网流程没有按普通模式启动，随后 App/设备侧会触发 reset/restart，日志中表现为：

```text
Device Reset:0
CLIENT RESTART!
Device Bind Start!
Device Reset!
```

修复方式：

```c
#if defined(ENABLE_QUADDLE_BLE_HID_CENTRAL) && ENABLE_QUADDLE_BLE_HID_CENTRAL
    netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_NETCFG_PREPARE, &(netcfg_args_t){.type = NETCFG_TUYA_BLE});
#else
    netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_NETCFG, &(netcfg_args_t){.type = NETCFG_TUYA_BLE});
#endif
```

修复后：

- 手柄/BLE HID central 开启：保持“手柄优先，BOOT 短按后进入 Tuya BLE 配网”的流程。
- 手柄/BLE HID central 关闭：恢复普通 Tuya BLE 配网流程，上电进入 activation mode 后直接启动 BLE 配网，不再依赖 BOOT 短按。
