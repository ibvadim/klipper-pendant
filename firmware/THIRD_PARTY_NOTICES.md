THIRD-PARTY NOTICES (DRAFT)

This file lists third-party software or assets that may be used by the examples
in this directory. This is a draft and should be verified and completed before
public release.

1) ESP-IDF
   - License: Apache License 2.0
   - Source: https://github.com/espressif/esp-idf
   - Notes: ESP-IDF includes additional third-party components (e.g. FatFS,
     LWIP, mbedTLS) that carry their own licenses. See the ESP-IDF NOTICE
     files for full details.

2) LVGL
   - License: MIT
   - Source: https://github.com/lvgl/lvgl
   - Used in: touch/LCD UI, image display, music UI, Wi-Fi UI, camera UI,
     microphone UI, battery UI

3) ESP LVGL Port / esp_lvgl_port
   - License: Apache License 2.0 (verify)
   - Source: https://github.com/espressif/esp-bsp (or esp-idf components)
   - Used in: LVGL display/touch porting

4) esp_lv_decoder
   - License: Apache License 2.0 (verify)
   - Source: https://github.com/espressif/esp-iot-solution (or esp-bsp)
   - Used in: SD card image decoding

5) esp-dsp
   - License: Apache License 2.0
   - Source: https://github.com/espressif/esp-dsp
   - Used in: microphone/AFe processing

6) esp_afe_sr_models (speech models)
   - License: Espressif license (verify redistribution terms)
   - Source: https://github.com/espressif/esp-sr
   - Used in: microphone test example

7) Shine MP3 (shine_mp3.c)
   - License: LGPL-2.1 (verify)
   - Source: https://github.com/toots/shine (or upstream used by esp_ting_mp3)
   - Used in: music test example

8) GUI assets and fonts
   - License: TBD (verify)
   - Source: assets under components/gui/assets and fonts under esp_prj_ui_font
   - Used in: music UI and microphone UI

END OF DRAFT



