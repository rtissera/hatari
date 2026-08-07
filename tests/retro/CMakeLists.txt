
add_executable(test-retro test-retro.c)
target_include_directories(test-retro PRIVATE ${LIBRETRO_INCLUDE_DIR})

add_test(NAME retro COMMAND test-retro $<TARGET_FILE:retro-hatari>)

add_executable(test-retro-disk test-retro-disk.c)
target_include_directories(test-retro-disk PRIVATE ${LIBRETRO_INCLUDE_DIR})

add_test(NAME retro-disk COMMAND test-retro-disk $<TARGET_FILE:retro-hatari>)

add_executable(test-retro-harddisk test-retro-harddisk.c)
target_include_directories(test-retro-harddisk PRIVATE ${LIBRETRO_INCLUDE_DIR})

add_test(NAME retro-harddisk COMMAND test-retro-harddisk $<TARGET_FILE:retro-hatari>)

add_executable(test-retro-statusbar test-retro-statusbar.c)
target_include_directories(test-retro-statusbar PRIVATE ${LIBRETRO_INCLUDE_DIR})

add_test(NAME retro-statusbar COMMAND test-retro-statusbar $<TARGET_FILE:retro-hatari>)

add_executable(test-retro-joystick test-retro-joystick.c)
target_include_directories(test-retro-joystick PRIVATE ${LIBRETRO_INCLUDE_DIR})

add_test(NAME retro-joystick COMMAND test-retro-joystick $<TARGET_FILE:retro-hatari>)

add_executable(test-retro-gemdos test-retro-gemdos.c)
target_include_directories(test-retro-gemdos PRIVATE ${LIBRETRO_INCLUDE_DIR})

add_test(NAME retro-gemdos COMMAND test-retro-gemdos $<TARGET_FILE:retro-hatari>)
