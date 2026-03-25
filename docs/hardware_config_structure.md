# your_chat_bot 硬件配置文件结构与调用关系

## 一、硬件配置相关文件总览

### 1. 应用层配置文件（`apps/tuya.ai/your_chat_bot/`）

| 文件 | 作用 |
|---|---|
| `app_default.config` | **当前激活配置**，指定当前使用哪块板，构建时被直接读取 |
| `config/DNESP32S3_BOX.config` | DNESP32S3 BOX 板的预置配置（对应 `app_default.config` 的内容） |
| `config/*.config`（共 16 个） | 各硬件平台的预置配置，切换板子时将对应文件内容复制到 `app_default.config` |
| `include/tuya_config.h` | 涂鸦云认证信息（PID / UUID / AuthKey） |
| `Kconfig` | 应用层配置菜单定义（GUI 类型、字体、按键等选项） |

### 2. 板级硬件配置文件（`boards/ESP32/DNESP32S3_BOX/`）

| 文件 | 作用 |
|---|---|
| `Kconfig` | 声明芯片型号、板名、自动开启的外设能力（audio / display / flash size） |
| `board_config.h` | **GPIO 引脚常量定义**（I2C / I2S / LCD 引脚、分辨率、音频参数） |
| `board_com_api.h` | 对外接口声明：`board_register_hardware()` |
| `DNESP32S3-BOX.c` | **板级初始化实现**（IO 扩展器 / 音频 Codec / 按键 / LCD 初始化） |

### 3. 平台级能力配置（`boards/ESP32/`）

| 文件 | 作用 |
|---|---|
| `Kconfig` | 枚举所有支持的子板，通过 `choice` 机制单选激活某块板 |
| `TKL_Kconfig` | 声明 ESP32 平台默认外设能力开关（WiFi / I2S / SPI / AUDIO / DISPLAY 等） |

---

## 二、配置文件之间的调用链

```
app_default.config
  ↓ CONFIG_BOARD_CHOICE_DNESP32S3_BOX=y
  ↓ CONFIG_BOARD_CHOICE_ESP32=y

boards/Kconfig  →  rsource "ESP32/Kconfig"
                       ↓
boards/ESP32/Kconfig（choice 激活 DNESP32S3_BOX 分支）
   └─ rsource "DNESP32S3_BOX/Kconfig"
         ↓ select ENABLE_AUDIO
         ↓ select ENABLE_ESP_DISPLAY
         ↓ select PLATFORM_FLASHSIZE_16M

boards/ESP32/TKL_Kconfig（外设能力基线，可被 board select 覆盖）

构建系统生成 sdkconfig / tuya_kconfig.h
   ↓
boards/ESP32/DNESP32S3_BOX/board_config.h（引脚常量头文件）
boards/ESP32/DNESP32S3_BOX/DNESP32S3-BOX.c（硬件初始化实现）

src/tuya_main.c
   └─ 调用 board_register_hardware()
         ├── __io_expander_init()      ← 初始化 XL9555 IO 扩展芯片
         ├── __board_register_button() ← 注册 "ai_chat_button"
         ├── __board_register_audio()  ← 检测并注册 ES8311 或 NS4168
         └── board_display_init()      ← 初始化 ST7789 LCD + 背光
   └─ 调用 app_chat_bot_init()         ← 启动 AI 聊天机器人业务
```

---

## 三、DNESP32S3_BOX 板级引脚一览（`board_config.h`）

| 总线 | 引脚 | 说明 |
|---|---|---|
| I2C SCL | GPIO 45 | 音频 Codec（ES8311）控制总线 |
| I2C SDA | GPIO 48 | 音频 Codec（ES8311）控制总线 |
| I2S BCK | GPIO 21 | 音频数据时钟 |
| I2S WS | GPIO 13 | 音频字选择 |
| I2S DO | GPIO 14 | 音频输出（扬声器） |
| I2S DI | GPIO 47 | 音频输入（麦克风） |
| LCD（8080 并口） | CS=1, DC=2, RD=41, WR=42 | ST7789，分辨率 320×240 |
| LCD 数据线 D0~D7 | GPIO 40/39/38/12/11/10/9/46 | 8 位并口数据 |
| IO 扩展器 XL9555 | I2C 地址 0x20 | 管理背光 / 按键 / 扬声器使能 |

---

## 四、支持的全部硬件平台（`config/` 目录）

| 配置文件 | 平台 / 芯片 | 显示 | 音频后端 |
|---|---|---|---|
| `DNESP32S3_BOX.config` | ESP32-S3 | LCD 320×240（8080） | ES8311 / NS4168 |
| `DNESP32S3_BOX2_WIFI.config` | ESP32-S3 | LCD | ES8311 |
| `DNESP32S3.config` | ESP32-S3 | 无 | 默认 |
| `ESP32S3_BREAD_COMPACT_WIFI.config` | ESP32-S3 | OLED 128×32 | — |
| `XINGZHI_ESP32S3_Cube_0_96OLED_WIFI.config` | ESP32-S3 | OLED 128×32 | — |
| `WAVESHARE_ESP32S3_TOUCH_AMOLED_1_8.config` | ESP32-S3 | AMOLED 1.8" 触屏 | — |
| `RaspberryPi.config` | Linux / ARM | 无 | ALSA |
| `DshanPi_A1.config` | Linux / ARM | 无 | ALSA |
| `T5AI_MINI_LCD_1.54.config` | Tuya T5AI | 1.54" LCD | 板载 |
| `T5AI_MOJI_1.28.config` | Tuya T5AI | 1.28" 圆屏 | 板载 |
| `TUYA_T5AI_BOARD_LCD_3.5.config` | Tuya T5AI | 3.5" LCD 触屏 | 板载 |
| `TUYA_T5AI_BOARD_LCD_3.5_CAMERA.config` | Tuya T5AI | 3.5" LCD 触屏 | Opus |
| `TUYA_T5AI_BOARD_LCD_3.5_v101.config` | Tuya T5AI v1.0.1 | 3.5" LCD 触屏 | — |
| `TUYA_T5AI_CORE.config` | Tuya T5AI 核心板 | 无 | — |
| `TUYA_T5AI_EVB.config` | Tuya T5AI EVB | 聊天 GUI | — |
| `WAVESHARE_T5AI_TOUCH_AMOLED_1_75.config` | Tuya T5AI | 1.75" AMOLED 触屏 | — |

---

## 五、切换硬件板的方法

将 `config/` 下对应板的配置文件内容复制到 `app_default.config` 即可：

```bash
# 示例：切换到树莓派
cp config/RaspberryPi.config app_default.config

# 示例：切换到星知 ESP32-S3 Cube
cp config/XINGZHI_ESP32S3_Cube_0_96OLED_WIFI.config app_default.config
```

`app_default.config` 中的 `CONFIG_BOARD_CHOICE_*` 字段会驱动整个 Kconfig 树，决定编译哪个 `boards/` 子目录下的驱动代码。

---

## 六、整体架构设计原则

> 本项目采用 **Kconfig 分层选板 + 运行时硬件注册** 模式。

- **Kconfig** 在编译期决定包含哪些驱动代码（静态裁剪）
- **`board_register_hardware()`** 在运行时完成外设的初始化与挂载（动态注册）
- **`app_default.config`** 是用户唯一需要修改的入口，改一个文件即可切换整块板
