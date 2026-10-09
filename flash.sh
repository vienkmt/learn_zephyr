#!/bin/zsh
# Bật môi trường Zephyr, biên dịch app này, rồi nạp lên kit STM32F411E-DISCO.
# Chạy: ./flash.sh
# Script tự làm hết. Không cần gõ zephyr-env trước.

# Dừng ngay nếu một lệnh bị lỗi, tránh nạp firmware build hỏng.
set -e

# Thư mục chứa script này, kể cả khi gọi từ chỗ khác.
root="$(cd "$(dirname "$0")" && pwd)"

# Bật venv Python 3.12, trỏ ZEPHYR_BASE và SDK nằm trên /Volumes/1tb.
# shellcheck disable=SC1091
source /Volumes/1tb/zephyrproject/activate.sh

# Biên dịch cho board stm32f411e_disco. Kết quả nằm trong thư mục build/.
west build -b stm32f411e_disco -d "${root}/build" "${root}"

# Nạp file .hex qua OpenOCD. OpenOCD dùng ST-Link có sẵn trên kit.
west flash -d "${root}/build" --runner openocd
