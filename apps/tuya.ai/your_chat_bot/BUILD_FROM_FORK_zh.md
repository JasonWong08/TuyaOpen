# Quaddle AI 完整源码下载与编译指南

本文说明如何从 JasonWong08 的 Fork 获取 `your_chat_bot` 的完整源码，包括主仓库代码和独立的 ESP32 平台代码，并编译出可烧录固件。

## 代码组成

完整功能来自两个 GitHub 仓库：

| 内容 | 仓库 | 分支/提交 |
| --- | --- | --- |
| TuyaOpen 主工程及应用代码 | `https://github.com/JasonWong08/TuyaOpen.git` | `cursor/add-ai-board` |
| ESP32 平台、唤醒词模型及底层驱动 | `https://github.com/JasonWong08/TuyaOpen-esp32.git` | `72c3414138af1e73265783df88b72805657beb38` |

主工程的 `platform/platform_config.yaml` 已固定 ESP32 平台仓库、分支和提交。正常执行构建或平台更新时，工具会自动下载正确的平台代码。

> 当前修改位于功能分支和 Draft PR 中，尚未合入仓库默认分支。请务必指定 `cursor/add-ai-board` 分支。直接下载默认分支 ZIP 不会包含全部修改。

## 1. 安装环境

建议使用 Ubuntu 22.04/24.04，Windows 用户可使用 WSL2。安装基础依赖：

```bash
sudo apt update
sudo apt install -y build-essential git python3 python3-pip python3-venv \
    libsystemd-dev libc6-i386 libusb-1.0-0 libusb-1.0-0-dev clang-format
```

## 2. 首次下载完整源码

```bash
git clone --branch cursor/add-ai-board --single-branch \
    https://github.com/JasonWong08/TuyaOpen.git
cd TuyaOpen
git submodule update --init --recursive
git branch --show-current
```

最后一条命令应输出：

```text
cursor/add-ai-board
```

不建议使用 GitHub 的 Download ZIP，因为 Git 更适合同步子模块、平台仓库和后续更新。

## 3. 初始化 TuyaOpen

在 TuyaOpen 仓库根目录执行：

```bash
. ./export.sh
tos.py check
```

`export.sh` 会创建或复用 Python 虚拟环境，并设置 TuyaOpen 所需环境变量。

## 4. 编译 your_chat_bot

```bash
cd apps/tuya.ai/your_chat_bot
tos.py build
```

首次构建会根据 `platform/platform_config.yaml` 下载 ESP32 平台。日志中应看到 ESP32 平台切换到以下提交：

```text
72c3414138af1e73265783df88b72805657beb38
```

构建成功后，完整固件位于：

```text
apps/tuya.ai/your_chat_bot/dist/your_chat_bot_1.0.1/your_chat_bot_QIO_1.0.1.bin
```

首次使用这套源码或更新唤醒词模型后，必须烧录 `QIO` 完整固件。只烧录 `UA`/`UG` 增量固件可能保留旧分区或旧语音模型。

## 5. 验证 ESP32 平台版本

回到 TuyaOpen 根目录后执行：

```bash
git -C platform/ESP32 remote -v
git -C platform/ESP32 rev-parse HEAD
grep -R "MULTINET5" platform/ESP32 --include='*.h' --include='*.c' -n | head
grep -R "hai kua dou" platform/ESP32 --include='*.h' --include='*.c' -n | head
```

`rev-parse HEAD` 必须输出：

```text
72c3414138af1e73265783df88b72805657beb38
```

如果该提交不一致，请先执行：

```bash
. ./export.sh
tos.py update
```

然后重新构建。

## 6. 更新已有的本地仓库

如果之前已经克隆过 TuyaOpen，在仓库根目录执行：

```bash
git fetch origin
git switch cursor/add-ai-board
git pull --ff-only origin cursor/add-ai-board
git submodule sync --recursive
git submodule update --init --recursive
. ./export.sh
tos.py update
cd apps/tuya.ai/your_chat_bot
tos.py build
```

如果 `platform/ESP32` 中存在个人修改，平台更新可能拒绝切换提交。请先提交或备份这些修改。确认不需要该目录中的本地修改后，也可以将旧目录改名，再重新下载：

```bash
cd "$(git rev-parse --show-toplevel)"
mv platform/ESP32 platform/ESP32.backup
. ./export.sh
cd apps/tuya.ai/your_chat_bot
tos.py build
```

## 7. 烧录后检查

完整烧录并启动后，可在串口日志中检查以下关键信息：

```text
multinet model in flash: mn5q8_cn
Custom wake ready: '嗨 Quaddle' (hai kua dou)
quaddle ble hid: pre-network gamepad discovery enabled
```

建议按以下顺序验证：

1. 配网前短按手柄 Home 键，确认手柄可以连接并控制设备。
2. 短按主板 Boot 键完成手机配网。
3. 联网后确认手柄可继续连接。
4. 使用“嗨 Quaddle”（发音“嗨夸豆”）唤醒并进行多轮对话。
5. 结束对话进入 IDLE 后，再验证手柄扫描与重连。

## 常见问题

### 为什么只克隆主仓库还会下载另一个仓库？

`platform/ESP32` 是独立平台仓库，不直接提交在 TuyaOpen 主仓库中。主仓库通过 `platform/platform_config.yaml` 固定它的下载地址和提交版本，这是 TuyaOpen 的正常平台管理方式。

### 为什么仍然是旧唤醒词？

通常是主工程分支不正确、ESP32 平台提交不正确，或只烧录了增量固件。依次检查当前分支、平台提交，并重新烧录 `QIO` 完整固件。

### 云端对话还需要什么？

设备需要有效的 Tuya UUID/AuthKey 和网络配置。不要把个人 UUID、AuthKey、Wi-Fi 密码或其他密钥提交到公开仓库。
