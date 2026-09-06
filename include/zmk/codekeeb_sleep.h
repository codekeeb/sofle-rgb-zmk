/*
 * SPDX-License-Identifier: MIT
 *
 * CODEKEEB PATCH: deep sleep con tiempo editable en caliente.
 *
 * ZMK duerme por inactividad con CONFIG_ZMK_IDLE_SLEEP_TIMEOUT, que es
 * un int de Kconfig: vive en el binario, asi que cambiar el tiempo
 * obligaba a recompilar y desde la web no habia forma de tocarlo.
 * Aqui el tiempo (y el si/no) viven en flash y se pueden cambiar desde
 * ZMK Studio, igual que el RGB o la animacion del OLED.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Limites de lo que se acepta al escribir, en minutos. El maximo son
 * 12 h: mas alla el temporizador deja de tener sentido practico. */
#define CODEKEEB_SLEEP_MIN_MINUTES 1
#define CODEKEEB_SLEEP_MAX_MINUTES 720

bool zmk_codekeeb_sleep_enabled(void);

/* Minutos de inactividad antes de dormir. */
uint16_t zmk_codekeeb_sleep_minutes(void);

/* Fija ambos, persiste en flash y reinicia la cuenta.
 * Devuelve 0, o -EINVAL si los minutos se salen del rango. */
int zmk_codekeeb_sleep_set(bool enabled, uint16_t minutes);
