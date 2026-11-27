set(main_requires
    read_pico
    read_pico_transfer
    read_pico_usb
    read_pico_search
    read_pico_totp
    read_pico_wifi_vault
    read_pico_weread
    tomato
    pinyin_ime
    espressif__qrcode
    continuous_du
    e0470_epaper_waveform
    read_pico_pmu
    fatfs
    wear_levelling
    bootloader_support
    spi_flash
    heap
    nvs_flash
    esp_hw_support
    esp_rom
    esp_driver_gpio
    esp_driver_usb_serial_jtag
    esp_app_format
    app_update
    mbedtls
    esp_wifi
)
idf_component_register(
    SRCS
        "app_main.c"
        "display.c"
        "heap_diag.c"
        "mem_reclaim.c"
        "sleep.c"
        "settings.c"
        "app/app_loop.c"
        "app/app_registry.c"
        "ui/ui_kit.c"
        "ui/ui_gesture.c"
        "ui/ui_wifi_qr.c"
        "ui/datamatrix.c"
        "ui/ui_menu.c"
        "ui/tr_ui.c"
        "ui/tr_kbd.c"
        "app/tr_nav.c"
        "app/tr_time.c"
        "app/tr_auth.c"
        "app/tr_wifi.c"
        "app/auto_turn.c"
        "apps/app_tr_tomato.c"
        "apps/app_tr_settings.c"
        "apps/app_tr_wall.c"
        "apps/app_tr_card.c"
        "apps/app_tr_store.c"
        "apps/app_tr_weread.c"
        "apps/app_tr_update.c"
        "apps/app_tr_hupu.c"
        "apps/app_tr_xhs.c"
        "apps/app_tr_weibo.c"
        "apps/app_tr_x.c"
        "apps/app_tr_all.c"
        "apps/app_tr_auth.c"
        "apps/app_tr_wt.c"
        "apps/tr_pin.c"
        "apps/tr_wallsave.c"
        "apps/tr_lock.c"
        "apps/tr_tarot.c"
        "apps/app_home.c"
        "apps/app_book.c"
        "apps/app_transfer.c"
        "apps/app_usb.c"
        "apps/app_comic.c"
        "book/book_store.c"
        "book/comic_store.c"
        "book/book_progress.c"
        "book/book_source.c"
        "book/book_txt.c"
        "book/book_epub.c"
        "book/book_image.c"
        "book/book_cover.c"
        "book/hupu_img.c"
        "book/book_jpegdec.c"
        "book/vendor/tjpgd.c"
        "book/vendor/jpegdec/jpegdec.c"
        "book/zip_reader.c"
        "book/html_text.c"
        "book/book_layout.c"
        "book/gbk.c"
        "apps/app_font_pick.c"
        "apps/app_sleep.c"
        "apps/app_selftest.c"
        "font/ttf_font.c"
        "factory/pmu_selftest.c"
        "factory/vcom_setup.c"
    REQUIRES ${main_requires}
    INCLUDE_DIRS "." "app" "apps" "font" "ui" "factory" "book"
    EMBED_FILES
        "assets/lock_4bpp.bin"
        "assets/loading_4bpp.bin"
        "assets/builtin.ttf"
    EMBED_TXTFILES
        "apps/server_form.html"
        "apps/auth_form.html"
        "apps/wall_upload.html"
)

# 图片解码器碰到损坏的图会让有符号整数溢出，用 -fwrapv 把它定义为回绕（与原来生成的结果一致）。
# 上游 JPEGDEC 不按本仓库的告警标准写，只对它关掉告警。
# Image decoders overflow signed integers on corrupt images; -fwrapv defines that as wraparound (the same result as
# before). Upstream JPEGDEC is not written to this repo's warning bar, so its warnings are silenced.
set_source_files_properties("book/book_image.c" "book/vendor/tjpgd.c" PROPERTIES COMPILE_OPTIONS "-fwrapv")
set_source_files_properties("book/vendor/jpegdec/jpegdec.c" PROPERTIES COMPILE_OPTIONS "-w;-fwrapv")

# 静态分析构建：idf.py -B build-analyze -DTREADER_ANALYZE=1 build
# Static-analysis build: idf.py -B build-analyze -DTREADER_ANALYZE=1 build
if(TREADER_ANALYZE)
    target_compile_options(${COMPONENT_LIB} PRIVATE -fanalyzer -fstack-usage -Wno-error)
endif()

# PSRAM 占用诊断固件：idf.py -B build-diag -DTREADER_HEAP_DIAG=1 build，每分钟在串口打一次 PSRAM 清单（heap_diag.c）。
# PSRAM usage diagnostic build: idf.py -B build-diag -DTREADER_HEAP_DIAG=1 build prints a PSRAM report on the serial
# port every minute (heap_diag.c).
if(TREADER_HEAP_DIAG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TREADER_HEAP_DIAG=1)
endif()
