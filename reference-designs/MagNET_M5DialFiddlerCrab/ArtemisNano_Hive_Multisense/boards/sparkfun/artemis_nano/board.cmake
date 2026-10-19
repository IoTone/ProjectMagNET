# SPDX-License-Identifier: Apache-2.0
#
# J-Link over the unpopulated 2x5 1.27 mm SWD footprint (J11). See docs/wiring.html.
# The image is linked at 0x10000 (CONFIG_FLASH_LOAD_OFFSET), so `west flash` writes
# only the application area and leaves the SVL at 0xC000 intact.
# USB flashing through the SVL is scripts/flash.py, not a west runner.

board_runner_args(jlink "--device=AMA3B1KK-KBR" "--iface=swd" "--speed=1000")

include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
