#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="${ROOT}/apps/tuya.ai/your_chat_bot"
PETOI_TUYA_FW_DIR="/Volumes/PetoiSpace/Quaddle/Upload_Firmwares/Firmwares/Tuya"
PETOI_TUYA_FW_HIST_DIR="${PETOI_TUYA_FW_DIR}/Histories"

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

dated_qio_bin_name() {
    local base="$1"
    local stamp="$2"
    local dest

    dest="$(printf '%s\n' "${base}" | sed -E "s/_[0-9]+(\.[0-9]+)+\.bin$/_${stamp}.bin/")"
    if [ "${dest}" = "${base}" ]; then
        dest="${base%.bin}_${stamp}.bin"
    fi
    printf '%s' "${dest}"
}

copy_firmware_bin() {
    local stamp src base dest digest
    stamp="${1:-$(date +%Y%m%d_%H%M)}"

    src="$(find_firmware_bin)"
    if [ -z "${src}" ]; then
        echo "No *_QIO_*.bin found under ${APP_DIR}/.build/bin or dist" >&2
        return 1
    fi

    base="$(basename "${src}")"
    dest="$(dated_qio_bin_name "${base}" "${stamp}")"

    cp -f "${src}" "${ROOT}/${dest}"
    echo "Firmware copy: ${ROOT}/${dest}"
}

publish_firmware_to_petoispace() {
    local src="$1"
    local stamp="$2"
    local base dated_base dest hist_dir digest existing

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

    stamp="${stamp:-$(date +%Y%m%d_%H%M)}"
    dated_base="$(dated_qio_bin_name "${base}" "${stamp}")"
    dest="${PETOI_TUYA_FW_DIR}/${dated_base}"
    hist_dir="${PETOI_TUYA_FW_HIST_DIR}"
    mkdir -p "${hist_dir}"
    digest="$(firmware_bin_digest "${src}")"

    for existing in "${PETOI_TUYA_FW_DIR}"/your_chat_bot_QIO_*.bin; do
        if [ ! -f "${existing}" ]; then
            continue
        fi
        if [ "$(firmware_bin_digest "${existing}")" = "${digest}" ]; then
            echo "PetoiSpace firmware already up to date: ${existing}"
            return 0
        fi
    done

    for existing in "${PETOI_TUYA_FW_DIR}"/your_chat_bot_QIO_*.bin; do
        if [ ! -f "${existing}" ]; then
            continue
        fi
        mv -f "${existing}" "${hist_dir}/$(basename "${existing}")"
        echo "Moved previous firmware to ${hist_dir}/$(basename "${existing}")"
    done

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
    FIRMWARE_STAMP="$(date +%Y%m%d_%H%M)"
    copy_firmware_bin "${FIRMWARE_STAMP}"
    publish_firmware_to_petoispace "${BIN_AFTER}" "${FIRMWARE_STAMP}"
fi

echo "Flash ${PORT}"
"${TOS[@]}" flash -p "${PORT}"

echo "Flash done. Press Reset (do not hold Boot) to run the app."
