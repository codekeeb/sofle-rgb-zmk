# ZMK Sofle + widget de uso de Claude

Config de ZMK para un Sofle (nice!nano v2) cuya OLED **derecha** muestra en
tiempo real el uso de Claude: porcentaje de la **sesión de 5 h**, porcentaje
del **límite semanal**, y cuenta atrás hasta cada reinicio.

```
OLED derecha (128x32):
┌────────────────────────────┐
│ 5H [████░░░░░]   37%  1h23 │
│ 7D [██████░░░]   62%  3d04 │
└────────────────────────────┘
```

## Arquitectura

```
PC (Windows)                      Sofle izquierda (central)      Sofle derecha (periférico)
─────────────                     ─────────────────────────      ──────────────────────────
claude_usage_daemon.py  ──BLE──▶  servicio GATT propio   ──split──▶  behavior claude_usage
  (lee % de uso del               (claude_usage_gatt.c)   (invoke    └▶ widget LVGL en OLED
   endpoint OAuth de                                       behavior)    (claude_usage_screen.c)
   Claude Code cada 60 s)
```

- El repo entero es además un **módulo ZMK** (`zephyr/module.yml`): el workflow
  oficial `build-user-config` lo detecta y compila el código de `src/` junto
  con ZMK main, sin forks.
- El canal central→periférico reutiliza el comando *invoke behavior* del
  transporte split: el estado viaja empaquetado en los dos `uint32` de
  parámetros del behavior `cldusage` (definido en `config/default.overlay`;
  ZMK no carga overlays llamados `sofle.overlay` desde el config, solo
  `<shield_completo>.overlay`, `<board>.overlay` o `default.overlay`).
  El nombre del nodo debe tener **como máximo 8 caracteres**: el payload BLE
  del split lo trunca a 8+nulo y el periférico no encontraría el behavior.
- Paquete BLE (característica `c1a0de01-2b5e-4f0c-9c8a-3f2a1d4e5b6c`, 6 bytes
  little-endian): `u8 pct_sesion, u8 pct_semana, u16 min_reset_sesion,
  u16 min_reset_semana` (0xFF/0xFFFF = desconocido).
- Si el teclado deja de recibir datos durante 3 min
  (`ZMK_CLAUDE_USAGE_STALE_TIMEOUT_S`), los textos vuelven a `--` (las barras
  conservan el último valor).

## Compilar y flashear

1. Haz push: GitHub Actions compila `sofle_left` y `sofle_right`
   (nice_nano_v2) y publica los `.uf2` como artefacto.
2. Flashea **ambas** mitades (doble pulsación de reset → unidad USB → copiar
   `zmk.uf2`).
3. Tras cambiar de firmware, si Windows no descubre el servicio GATT nuevo,
   desempareja y vuelve a emparejar el teclado (Windows cachea los servicios
   BLE de dispositivos emparejados).

## Daemon en el PC

```powershell
cd host
py -3 -m pip install -r requirements.txt
py -3 .\claude_usage_daemon.py --once --verbose   # prueba
py -3 .\claude_usage_daemon.py                    # en marcha (60 s)
```

- **Token**: el daemon usa el token OAuth local de Claude Code, en este orden:
  variable `CLAUDE_OAUTH_TOKEN` → `~/.claude/.credentials.json` → Administrador
  de credenciales de Windows. Hace falta haber iniciado sesión en el CLI
  (`claude`) al menos una vez en esta máquina. Claude Code refresca el token
  con el uso normal; si caduca, la pantalla mostrará `--` hasta que vuelvas a
  abrir Claude Code.
- **Descubrimiento BLE**: busca "Sofle" anunciándose y, si ya está conectado a
  Windows, saca la MAC de los dispositivos emparejados. Puedes fijarla con
  `--address AA:BB:CC:DD:EE:FF`.
- Para arrancarlo con Windows: Programador de tareas → al iniciar sesión →
  `pyw -3 C:\...\host\claude_usage_daemon.py`.

## Limitaciones conocidas

- El endpoint `api.anthropic.com/api/oauth/usage` es el que usa `/usage` de
  Claude Code pero **no está documentado**: puede cambiar sin aviso. El daemon
  degrada a `--` ante cualquier error.
- La OLED se apaga con la inactividad del teclado (comportamiento estándar de
  ZMK con SSD1306); el widget se ve al volver a teclear.
- El refresco BLE periódico consume algo más de batería en ambas mitades.

## Archivos

| Ruta | Qué es |
|---|---|
| `config/west.yml` | Manifest west (ZMK `v0.3` + módulo `zmk-nice-oled` rama `v0.3/dev`) |
| `config/sofle.conf` | Config común: encoders, RGB underglow, Studio, widget |
| `config/sofle_left.conf` | Solo izquierda: pantalla nice_oled (widget Claude off) |
| `config/sofle_right.conf` | Solo derecha: display + pantalla custom del widget |
| `config/sofle.keymap` | Keymap ADEU (4 capas, unicode €/ñ vía zmk-helpers + WinCompose, encoders RGB/volumen) |
| `config/default.overlay` | Behavior `claude_usage` |
| `config/boards/nice_nano_v2.overlay` | Cadena WS2812 del RGB underglow (SPI) |
| `zephyr/module.yml`, `CMakeLists.txt`, `Kconfig` | El repo como módulo ZMK |
| `src/claude_usage_gatt.c` | Servicio GATT (central) + relay al periférico |
| `src/behavior_claude_usage.c` | Behavior-canal de datos (ambas mitades) |
| `src/claude_usage_screen.c` | Widget LVGL vertical (bicho + baterías) (derecha) |
| `dts/bindings/behaviors/qolera,claude-usage.yaml` | Binding del behavior |
| `host/claude_usage_daemon.py` | Daemon de Windows |
