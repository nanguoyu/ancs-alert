# 各应用的人声提示音(英文、中文各一段)由 tools/gen_wx_voice.py 在本地生成且不入库;
# 缺了哪个,那个应用在那种语言下只播合成的“叮咚”。
set(WX_VOICE_APPS wechat messages telegram qq whatsapp signal line)
set(WX_VOICE_LANGS en zh)
set(WX_EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../assets/web/setup.html")
set(WX_VOICE_DEFS "")
foreach(lang ${WX_VOICE_LANGS})
    foreach(app ${WX_VOICE_APPS})
        set(pcm "${CMAKE_CURRENT_LIST_DIR}/../assets/music/wx_voice_${app}_${lang}_16k.pcm")
        if(EXISTS "${pcm}")
            list(APPEND WX_EMBED_FILES "${pcm}")
            string(TOUPPER "${app}_${lang}" KEY)
            list(APPEND WX_VOICE_DEFS "WX_HAVE_VOICE_${KEY}=1")
        endif()
    endforeach()
endforeach()

idf_component_register(
    SRCS "main.c"
         "wx_alert.c"
         "wx_ancs_proto.c"
         "wx_apps.c"
         "wx_ble.c"
         "wx_chime.c"
         "wx_dns.c"
         "wx_net.c"
         "wx_phone.c"
         "wx_portal.c"
         "wx_settings.c"
         "wx_sound.c"
         "wx_tg.c"
         "wx_tg_text.c"
         "wx_ui.c"
         "wx_wifi_list.c"
    INCLUDE_DIRS "."
    EMBED_FILES ${WX_EMBED_FILES}
    REQUIRES bsp bt esp_event esp_http_client esp_http_server esp_hw_support esp_netif esp_timer
             esp_wifi json lwip mbedtls nvs_flash
)

# 中文位图字库(tools/gen_wx_fonts.py 生成,来源与许可见 README.md)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/wx_font_cjk_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/wx_font_cjk_24.c"
)

if(WX_VOICE_DEFS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${WX_VOICE_DEFS})
endif()
