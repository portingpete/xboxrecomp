# Shared FMV player: Media Foundation decode onto a D3D8 texture.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    fb_present.c
    pc_video.c
    pc_video_menu.c
    pc_menu.c
    pc_menu_assets.c
    pc_movie_skip.c
    pc_photo.c
    pc_speech_subtitle.c
    pc_crosshair.c
    pc_debug.c
    pc_split.c
)
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(xbox_video PUBLIC xbox_d3d8 xbox_input)
if(WIN32)
    target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32 shell32 gdi32 user32)
endif()
