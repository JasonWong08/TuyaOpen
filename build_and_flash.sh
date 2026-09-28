#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="${ROOT}/apps/tuya.ai/your_chat_bot"

# ./build_and_flash.sh starts a child shell, so this cannot nest into the
# caller's terminal. Skip export.sh if this process already has the project venv.
if [ "${VIRTUAL_ENV:-}" != "${ROOT}/.venv" ]; then
    # export.sh reads unset VIRTUAL_ENV and may fail tos.py completion on Bash 3.2.
    set +eu
    # shellcheck disable=SC1091
    . "${ROOT}/export.sh"
    set -euo pipefail
fi

# export.sh replaces exit() with "leave the venv". Restore real exit so
# a missing serial port cannot deactivate the venv and keep running.
unset -f exit 2>/dev/null || true

TOS=("${OPEN_SDK_PYTHON:-${ROOT}/.venv/bin/python}" "${ROOT}/tos.py")

PORT_NAME="$(ls /dev | grep usb | grep '^cu.usbmodem' | head -n 1 || true)"
if [[ -z "${PORT_NAME}" ]]; then
    echo "No cu.usbmodem* found from: ls /dev | grep usb" >&2
    builtin exit 1
fi
PORT="/dev/${PORT_NAME}"

cd "${APP_DIR}"
export PYTHONUNBUFFERED=1

echo "Build in ${APP_DIR}"
"${TOS[@]}" build

echo "Flash ${PORT}"
"${TOS[@]}" flash -p "${PORT}"

echo "Flash done. Press Reset (do not hold Boot) to run the app."
