#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="${ROOT}/apps/tuya.ai/your_chat_bot"
PETOI_TUYA_FW_DIR="/Volumes/PetoiSpace/Quaddle/Upload_Firmwares/Firmwares/Tuya"

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

find_firmware_bin() {
    local src=""
    if [ -d "${APP_DIR}/.build/bin" ]; then
        src="$(find "${APP_DIR}/.build/bin" -maxdepth 1 -type f -name '*_QIO_*.bin' | head -n 1 || true)"
    fi
    if [ -z "${src}" ] && [ -d "${APP_DIR}/dist" ]; then
        src="$(find "${APP_DIR}/dist" -type f -name '*_QIO_*.bin' | head -n 1 || true)"
    fi
    printf '%s' "${src}"
}

firmware_bin_digest() {
    local path="$1"
    if [ -z "${path}" ] || [ ! -f "${path}" ]; then
        return 0
    fi
    shasum -a 256 "${path}" | awk '{print $1}'
}

copy_firmware_bin() {
    local stamp src base dest digest
    stamp="$(date +%Y%m%d_%H%M)"

    src="$(find_firmware_bin)"
    if [ -z "${src}" ]; then
        echo "No *_QIO_*.bin found under ${APP_DIR}/.build/bin or dist" >&2
        return 1
    fi

    base="$(basename "${src}")"
    dest="$(printf '%s\n' "${base}" | sed -E "s/_[0-9]+(\.[0-9]+)+\.bin$/_${stamp}.bin/")"
    if [ "${dest}" = "${base}" ]; then
        dest="${base%.bin}_${stamp}.bin"
    fi

    cp -f "${src}" "${ROOT}/${dest}"
    echo "Firmware copy: ${ROOT}/${dest}"
}

publish_firmware_to_petoispace() {
    local src="$1"
    local base dest hist_dir stamp

    if [ -z "${src}" ] || [ ! -f "${src}" ]; then
        return 0
    fi
    if [ ! -d "/Volumes/PetoiSpace" ] || [ ! -d "${PETOI_TUYA_FW_DIR}" ]; then
        echo "PetoiSpace Tuya firmware dir not mounted, skip publish: ${PETOI_TUYA_FW_DIR}" >&2
        return 0
    fi

    base="$(basename "${src}")"
    case "${base}" in
        your_chat_bot_QIO_*.bin) ;;
        *)
            echo "Skip PetoiSpace publish (unexpected bin name): ${base}" >&2
            return 0
            ;;
    esac

    dest="${PETOI_TUYA_FW_DIR}/${base}"
    hist_dir="${PETOI_TUYA_FW_DIR}/history"
    mkdir -p "${hist_dir}"

    if [ -f "${dest}" ]; then
        if [ "$(firmware_bin_digest "${dest}")" = "$(firmware_bin_digest "${src}")" ]; then
            echo "PetoiSpace firmware already up to date: ${dest}"
            return 0
        fi
        stamp="$(date +%Y%m%d_%H%M)"
        mv -f "${dest}" "${hist_dir}/${base%.bin}_${stamp}.bin"
        echo "Moved previous firmware to ${hist_dir}/"
    fi

    cp -f "${src}" "${dest}"
    echo "Published firmware: ${dest}"
}

echo "Build in ${APP_DIR}"
BIN_BEFORE="$(find_firmware_bin)"
DIGEST_BEFORE="$(firmware_bin_digest "${BIN_BEFORE}")"

"${TOS[@]}" build

BIN_AFTER="$(find_firmware_bin)"
DIGEST_AFTER="$(firmware_bin_digest "${BIN_AFTER}")"

if [ -n "${DIGEST_BEFORE}" ] && [ "${DIGEST_BEFORE}" = "${DIGEST_AFTER}" ]; then
    echo "Firmware unchanged, skip copying a new bin to ${ROOT}"
else
    copy_firmware_bin
    publish_firmware_to_petoispace "${BIN_AFTER}"
fi

echo "Flash ${PORT}"
"${TOS[@]}" flash -p "${PORT}"

echo "Flash done. Press Reset (do not hold Boot) to run the app."
