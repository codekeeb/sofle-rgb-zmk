/*
 * Estado compartido del uso de Claude y su empaquetado en los dos
 * parámetros uint32 que admite el comando invoke-behavior del split.
 */

#pragma once

#include <stdint.h>

#define ZMK_CLAUDE_USAGE_PCT_UNKNOWN 0xFF
#define ZMK_CLAUDE_USAGE_MIN_UNKNOWN 0xFFFF

struct zmk_claude_usage_state {
    uint8_t session_pct;        /* 0-100, 0xFF = desconocido */
    uint8_t weekly_pct;         /* 0-100, 0xFF = desconocido */
    uint16_t session_reset_min; /* minutos hasta el reset de la sesión de 5 h */
    uint16_t weekly_reset_min;  /* minutos hasta el reset semanal */
};

static inline void zmk_claude_usage_pack(const struct zmk_claude_usage_state *state,
                                         uint32_t *param1, uint32_t *param2) {
    *param1 = (uint32_t)state->session_pct | ((uint32_t)state->weekly_pct << 8);
    *param2 = (uint32_t)state->session_reset_min | ((uint32_t)state->weekly_reset_min << 16);
}

static inline struct zmk_claude_usage_state zmk_claude_usage_unpack(uint32_t param1,
                                                                    uint32_t param2) {
    return (struct zmk_claude_usage_state){
        .session_pct = param1 & 0xFF,
        .weekly_pct = (param1 >> 8) & 0xFF,
        .session_reset_min = param2 & 0xFFFF,
        .weekly_reset_min = (param2 >> 16) & 0xFFFF,
    };
}

#ifdef CONFIG_ZMK_CLAUDE_USAGE_WIDGET
void zmk_claude_usage_widget_update(struct zmk_claude_usage_state state);
/* Alterna la pantalla entre reposo (bicho grande) y datos (claude-usage). */
void zmk_claude_usage_widget_toggle(void);
#endif
