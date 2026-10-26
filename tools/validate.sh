#!/usr/bin/env bash
# Checks for this project: host unit tests, firmware build and merged image.
#   ./tools/validate.sh --host       host tests only (no ESP-IDF needed)
#   ./tools/validate.sh --firmware   firmware only (source ESP-IDF v5.5.3 export.sh first)
#   ./tools/validate.sh              both
set -euo pipefail
cd "$(dirname "$0")/.."
mode="${1:---all}"

host_tests() {
    local tmp
    tmp="$(mktemp -d)"
    # 每项:模块名,以及它依赖的其他纯逻辑模块。
    local tests=(
        "ancs_proto"
        "alert apps"
        "apps"
        "chime"
        "dns"
        "phone"
        "tg_text ancs_proto"
        "wifi_list"
    )
    for entry in "${tests[@]}"; do
        read -r t deps <<< "${entry}"
        local srcs=("main/wx_${t}.c")
        for d in ${deps}; do srcs+=("main/wx_${d}.c"); done
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Imain \
            "tests/test_wx_${t}.c" "${srcs[@]}" -o "${tmp}/test_wx_${t}"
        "${tmp}/test_wx_${t}"
    done
    rm -rf "${tmp}"
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_wx_font_symbols.py
    echo "Host tests: PASS"
}

firmware() {
    if ! idf.py --version 2>/dev/null | grep -q 'v5\.5\.3'; then
        echo "Source ESP-IDF v5.5.3 first: . <path-to>/esp-idf-v5.5.3/export.sh" >&2
        exit 1
    fi
    # 每次都从 sdkconfig.defaults 重新生成配置,避免旧配置残留。
    rm -f build/sdkconfig
    idf.py -B build -D SDKCONFIG=build/sdkconfig build
    idf.py -B build -D SDKCONFIG=build/sdkconfig merge-bin -o ancs-alert-full.bin
    echo "Firmware: PASS"
    echo "  App image (write at 0x10000, keeps pairings and settings): build/ancs-alert.bin"
    echo "  Full image (write at 0x0, first install or partition change): build/ancs-alert-full.bin"
}

case "${mode}" in
    --host) host_tests ;;
    --firmware) firmware ;;
    --all) host_tests; firmware ;;
    *) echo "用法: $0 [--host|--firmware]" >&2; exit 2 ;;
esac
