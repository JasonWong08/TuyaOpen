# 问题：ESP-IDF 子构建失败（`No module named 'esp_idf_monitor'`）

## 现象

在 `tos.py build` 触发 ESP-IDF 子构建（`idf.py set-target`/`idf.py build`）时出现：

- `No module named 'esp_idf_monitor'`
- 并伴随提示“idf.py 未在 ESP-IDF shell 环境中运行/虚拟环境损坏”等

## 根因

ESP-IDF 的 `export.sh` 会先运行 `tools/detect_python.sh`，从 `PATH` 中挑选 `python3/python/...` 作为 `ESP_PYTHON`，再用它执行 `tools/activate.py`：

- 若外层 TuyaOpen 的 `.venv` 已激活，`PATH` 前缀里优先命中的是 TuyaOpen 的 Python
- ESP-IDF 环境导出过程中（shell completion/monitor 相关）会依赖 `esp_idf_monitor`
- TuyaOpen 的 Python 环境通常不包含该模块，于是报 `No module named 'esp_idf_monitor'`

> 关键点：这里并不是 ESP-IDF 的 python_env 缺失，而是 **ESP-IDF 激活链路选错了 Python 解释器**。

## 修复

在调用 ESP-IDF `export.sh` 之前，先从环境变量中移除外层项目的 Python venv 痕迹：

- `unset VIRTUAL_ENV`
- 从 `PATH` 中剔除 `${VIRTUAL_ENV}/bin`

代码落点：

- `platform/ESP32/tools/util.py`
- 函数 `execute_idf_commands(...)`

通过在该函数里对 `os.environ` 做清理，确保后续 `os.system(". export.sh && idf.py ...")` 的子 shell 不会优先命中外层 venv 的 python，从而让 ESP-IDF 正常启用其自身的 python_env。

## 验证

- `tos.py build` 能顺利进入 `idf.py build`
- 构建日志不再出现 `No module named 'esp_idf_monitor'`

