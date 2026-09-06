# SPDX-License-Identifier: MIT

if(CONFIG_CODEKEEB_BATTERY_NRF_VDDH_CURVE)
  zephyr_library()
  zephyr_library_sources(src/battery_nrf_vddh_curve.c)
endif()

if(CONFIG_ZMK_RGB_FX)
  target_include_directories(app PRIVATE include)
  target_sources(app PRIVATE
    src/color.c
    src/rgb_fx.c
    src/behaviors/behavior_rgb_fx.c
    src/fx/control_group.c
    src/fx/ripple.c
    src/fx/sparkle.c
    src/fx/linear_gradient.c
    src/fx/solid.c
    src/fx/heatmap.c
    src/fx/layer_color.c)
endif()

if(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO)
  target_include_directories(app PRIVATE include)
  target_sources(app PRIVATE src/behaviors/behavior_dynamic_macro.c)
endif()

if(CONFIG_ZMK_BEHAVIOR_MOUSE_WHEEL)
  target_sources(app PRIVATE src/behaviors/behavior_mouse_wheel.c)
endif()

if(CONFIG_ZMK_SENSOR_ROTATE_DYNAMIC)
  target_include_directories(app PRIVATE include)
  target_sources(app PRIVATE src/behaviors/behavior_sensor_rotate_dynamic.c)
endif()

# The Studio RPC handlers only make sense where Studio itself runs;
# ZMK_SENSOR_ROTATE_DYNAMIC is already central-only (see Kconfig), and
# Studio in this repo only ever builds on the central too.
if(CONFIG_ZMK_SENSOR_ROTATE_DYNAMIC AND CONFIG_ZMK_STUDIO)
  target_sources(app PRIVATE src/studio/sensor_binding_subsystem.c)
endif()

if(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO AND CONFIG_ZMK_STUDIO)
  target_sources(app PRIVATE src/studio/macro_subsystem.c)
endif()

# Lighting over RPC (RGB + OLED animation). Studio only runs on the
# central, and the handlers compile out on their own when the board has
# no RGB or no OLED module, so CONFIG_ZMK_STUDIO is the only gate needed.
if(CONFIG_ZMK_STUDIO)
  target_sources(app PRIVATE src/studio/lighting_subsystem.c)
endif()
