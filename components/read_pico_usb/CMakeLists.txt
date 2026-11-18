# SPDX-License-Identifier: Apache-2.0
# USB 传书：TF 卡当 U 盘。/ USB transfer: the TF card as a USB drive.
idf_component_register(
    SRCS "read_pico_usb.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer sdmmc esp_driver_sdmmc fatfs
)

# 数读写量：包住 sdmmc 读写。/ Count bytes by wrapping sdmmc reads and writes.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=sdmmc_read_sectors"
    "-Wl,--wrap=sdmmc_write_sectors")
