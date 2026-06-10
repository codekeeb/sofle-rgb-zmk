if(CONFIG_ZMK_CLAUDE_USAGE)
  zephyr_library_named(zmk_claude_usage)
  # Headers de ZMK (app/include): son PRIVATE del target app, hay que añadirlos
  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
  zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
  zephyr_library_sources(src/behavior_claude_usage.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ROLE_CENTRAL src/claude_usage_gatt.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_CLAUDE_USAGE_WIDGET src/claude_usage_screen.c)
endif()
