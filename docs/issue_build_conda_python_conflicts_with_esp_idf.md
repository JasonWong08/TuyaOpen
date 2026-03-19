# 问题：构建失败（Conda Python 版本与 ESP-IDF 虚拟环境不匹配）

## 现象

执行 `tos.py build` 时，外层 TuyaOpen cmake 构建成功，但在触发内层 `idf.py build` 时报错：

```
Checking "python3" ...
Python 3.12.9
"python3" has been detected
ERROR: ESP-IDF Python virtual environment not found. Please run the install script to set it up before proceeding.
sh: 1: idf.py: not found
Error: Build failed.
```

以及 `idf.py` 找不到：

```
FAILED: build_example_exe ...
ninja: build stopped: subcommand failed.
[ERROR]: Build error.
```

## 根因

### 环境污染链

```
.bashrc 启动
  → conda init → conda base 的 Python 3.12 置入 PATH 最前端
  → tyopen / tos.py → export.sh → .venv 激活（Python 3.10）
     .venv/bin 优先，掩盖了 conda Python

util.py: execute_idf_commands()
  → 移除 .venv/bin（已有修复）
  → 但 conda bin 目录仍在 PATH 中！
  → PATH 中最靠前的 python3 变成 conda 的 Python 3.12

ESP-IDF export.sh → detect_python.sh
  → 检测到 Python 3.12
  → 寻找 idf5.4_py3.12_env
  → 实际安装的是 idf5.4_py3.10_env → 找不到 → 报错
```

### 两个叠加问题

1. **Python 版本冲突**：Conda base 环境的 Python 3.12 在移除 `.venv/bin` 后成为 PATH 中最优先的 Python，而 ESP-IDF 只安装了 `idf5.4_py3.10_env`。

2. **`.bashrc _TYOPEN_ROOT` 指向错误仓库**：`.bashrc` 中 `_TYOPEN_ROOT="/home/jasonw/Projects/TuyaOpen"`（旧仓库），而所有修复都在 `/home/jasonw/Projects/TyOpen_Jason/TuyaOpen`（新仓库）。两个仓库是完全独立的 git 历史：

   ```
   TuyaOpen (旧):        47b4c202 Correct printing memory usage information
   TyOpen_Jason/TuyaOpen: 2240c01e Delete debug log (#523)
   ```

## 修复

### 1. `platform/ESP32/tools/util.py` — 在调用 ESP-IDF 前同时清除 Conda

在 `execute_idf_commands()` 中，除了移除 TuyaOpen `.venv`，还移除 conda 的 PATH 目录和环境变量：

```python
# 移除 TuyaOpen virtualenv
virtual_env = os.environ.pop("VIRTUAL_ENV", None)

# 移除 conda/mamba/miniforge 环境目录
conda_prefix = os.environ.pop("CONDA_PREFIX", None)
os.environ.pop("CONDA_DEFAULT_ENV", None)
os.environ.pop("CONDA_SHLVL", None)
for key in [k for k in list(os.environ.keys()) if k.startswith("CONDA_PREFIX_")]:
    os.environ.pop(key, None)

# 重建 PATH，排除所有 venv/conda 目录
_drop_keywords = (".venv", "venv/bin", "conda", "miniforge", "mambaforge", "mamba")
current_path = os.environ.get("PATH", "")
cleaned_path = os.pathsep.join(
    p for p in current_path.split(os.pathsep)
    if not any(kw in p for kw in _drop_keywords)
)
os.environ["PATH"] = cleaned_path
```

清理后，`detect_python.sh` 找到系统 Python 3.10.12，匹配 `idf5.4_py3.10_env`，构建正常。

### 2. `~/.bashrc` — `tyopen()` 在激活 TuyaOpen 前退出 Conda

`.bashrc` 中 `tyopen()` 函数已添加 conda 退出逻辑：

```bash
tyopen() {
    # 退出所有 conda 环境层（包括 base），避免 Python 版本冲突
    if command -v conda &>/dev/null; then
        while [ -n "${CONDA_PREFIX}" ]; do
            conda deactivate 2>/dev/null || break
        done
    fi
    # 若旧 .venv 由 conda Python 创建（版本不是系统版），先删除让其重建
    local venv_dir="${_TYOPEN_ROOT}/.venv"
    if [ -d "${venv_dir}" ]; then
        local venv_py sys_py
        venv_py=$("${venv_dir}/bin/python3" --version 2>/dev/null | awk '{print $2}')
        sys_py=$(python3 --version 2>/dev/null | awk '{print $2}')
        if [ -n "${venv_py}" ] && [ "${venv_py}" != "${sys_py}" ]; then
            echo "[tyopen] .venv Python (${venv_py}) != system Python (${sys_py}), recreating..."
            rm -rf "${venv_dir}"
        fi
    fi
    source "${_TYOPEN_ROOT}/export.sh"
}
```

### 3. `~/.bashrc` — `_TYOPEN_ROOT` 指向正确仓库

```bash
# 修改前（旧仓库）
_TYOPEN_ROOT="/home/jasonw/Projects/TuyaOpen"

# 修改后（包含所有 petoi_esp32c3_cube 修复的新仓库）
_TYOPEN_ROOT="/home/jasonw/Projects/TyOpen_Jason/TuyaOpen"
```

使修改立即生效：

```bash
source ~/.bashrc
```

## 验证

修复后构建输出：

```
Checking "python3" ...
Python 3.10.12          ← 正确版本
"python3" has been detected
Activating ESP-IDF 5.4
* Checking python version ... 3.10.12
* Checking python dependencies ... OK
...
====================[ BUILD SUCCESS ]===================
```

## 与已有问题的区别

本问题与 `issue_build_idf_python_env_esp_idf_monitor_missing.md` 相关，但根因不同：

| 问题 | 根因 | 症状 |
|------|------|------|
| esp_idf_monitor 缺失 | TuyaOpen `.venv` Python 被 ESP-IDF 选中，该 Python 无 `esp_idf_monitor` 模块 | `No module named 'esp_idf_monitor'` |
| 本问题 | Conda Python 3.12 在移除 `.venv` 后成为首选 Python，与 `idf5.4_py3.10_env` 版本不匹配 | `ESP-IDF Python virtual environment not found` |

两者的修复都落在 `util.py` 的 `execute_idf_commands()`，但需要同时清除 venv 和 conda。

## 注意事项

- `util.py` 的修复是双重保险：即使用户忘记运行 `tyopen`（未退出 conda），构建时也能自动清除干扰
- 若系统新增其他 Python 环境管理工具（如 pyenv、mamba），需相应更新 `_drop_keywords` 列表
- 每次在 Cursor 中构建时，Cursor 的终端继承完整 `.bashrc` 环境（含 conda），`util.py` 的清理机制对此场景同样生效
