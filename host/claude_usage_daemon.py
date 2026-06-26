#!/usr/bin/env python3
"""Envia el uso de Claude (sesion 5 h y semanal) a la OLED del Sofle por BLE.

Lee el porcentaje de uso y la hora de reinicio del endpoint OAuth que usa
el comando /usage de Claude Code, y escribe un paquete de 6 bytes en la
caracteristica GATT que expone el firmware (modulo zmk claude-usage):

    uint8  pct sesion 5 h   (0-100, 0xFF = desconocido)
    uint8  pct semanal      (0-100, 0xFF = desconocido)
    uint16 minutos hasta reset de sesion (LE, 0xFFFF = desconocido)
    uint16 minutos hasta reset semanal   (LE, 0xFFFF = desconocido)

Uso:
    python claude_usage_daemon.py [--address AA:BB:CC:DD:EE:FF] [--name Sofle]
                                  [--interval 60] [--once]

Requiere: pip install bleak  (y estar logueado en Claude Code CLI para que
exista el token OAuth local).
"""

import argparse
import asyncio
import ctypes
import ctypes.wintypes
import json
import logging
import logging.handlers
import os
import struct
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from bleak import BleakClient, BleakScanner

LOG = logging.getLogger("claude-usage")

USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
OAUTH_BETA_HEADER = "oauth-2025-04-20"
# Cloudflare de console.anthropic.com bloquea el UA por defecto de urllib
# (403 "error code: 1010"); imitar a la CLI de Claude Code.
USER_AGENT = "claude-cli/2.0.0 (external, cli)"
CREDENTIALS_FILE = Path.home() / ".claude" / ".credentials.json"
CRED_MANAGER_TARGET = "Claude Code-credentials"

CHAR_UUID = "c1a0de01-2b5e-4f0c-9c8a-3f2a1d4e5b6c"

PCT_UNKNOWN = 0xFF
MIN_UNKNOWN = 0xFFFF
UNKNOWN_PAYLOAD = struct.pack("<BBHH", PCT_UNKNOWN, PCT_UNKNOWN, MIN_UNKNOWN, MIN_UNKNOWN)


# ---------------------------------------------------------------------------
# Token OAuth de Claude Code
# ---------------------------------------------------------------------------

def _token_from_credential_manager() -> str | None:
    """Lee el credencial generico de Claude Code del Administrador de
    credenciales de Windows, si existe."""
    if sys.platform != "win32":
        return None

    class CREDENTIAL(ctypes.Structure):
        _fields_ = [
            ("Flags", ctypes.wintypes.DWORD),
            ("Type", ctypes.wintypes.DWORD),
            ("TargetName", ctypes.wintypes.LPWSTR),
            ("Comment", ctypes.wintypes.LPWSTR),
            ("LastWritten", ctypes.wintypes.FILETIME),
            ("CredentialBlobSize", ctypes.wintypes.DWORD),
            ("CredentialBlob", ctypes.POINTER(ctypes.c_byte)),
            ("Persist", ctypes.wintypes.DWORD),
            ("AttributeCount", ctypes.wintypes.DWORD),
            ("Attributes", ctypes.c_void_p),
            ("TargetAlias", ctypes.wintypes.LPWSTR),
            ("UserName", ctypes.wintypes.LPWSTR),
        ]

    advapi32 = ctypes.windll.advapi32
    cred_ptr = ctypes.POINTER(CREDENTIAL)()
    # CRED_TYPE_GENERIC = 1
    if not advapi32.CredReadW(CRED_MANAGER_TARGET, 1, 0, ctypes.byref(cred_ptr)):
        return None
    try:
        blob = ctypes.string_at(cred_ptr.contents.CredentialBlob,
                                cred_ptr.contents.CredentialBlobSize)
        data = json.loads(blob.decode("utf-8"))
        return data.get("claudeAiOauth", {}).get("accessToken")
    except (ValueError, KeyError):
        return None
    finally:
        advapi32.CredFree(cred_ptr)


# El endpoint de refresco se movió de console.anthropic.com a api.anthropic.com
# (~jun 2026); console.* ahora devuelve 404 not_found_error.
TOKEN_URL = "https://api.anthropic.com/v1/oauth/token"
# client_id público de la CLI de Claude Code (mismo flujo OAuth que usa /login)
OAUTH_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
# Refrescar si quedan menos de 2 min de vida al token
TOKEN_REFRESH_MARGIN_MS = 120 * 1000


def _refresh_oauth_token(data: dict) -> str | None:
    """Refresca el access token con el refresh token y persiste el resultado
    en .credentials.json (escritura atómica, conservando el resto de campos).
    Claude Code refresca su token en memoria pero no actualiza el archivo, así
    que de madrugada el archivo siempre está caducado."""
    oauth = data.get("claudeAiOauth", {})
    refresh_token = oauth.get("refreshToken")
    if not refresh_token:
        return None

    body = json.dumps({
        "grant_type": "refresh_token",
        "refresh_token": refresh_token,
        "client_id": OAUTH_CLIENT_ID,
    }).encode("utf-8")
    request = Request(TOKEN_URL, data=body,
                      headers={"Content-Type": "application/json",
                               "anthropic-beta": OAUTH_BETA_HEADER,
                               "User-Agent": USER_AGENT})
    try:
        with urlopen(request, timeout=20) as response:
            result = json.loads(response.read())
    except HTTPError as err:
        try:
            detail = err.read().decode("utf-8", "replace")[:300]
        except OSError:
            detail = ""
        LOG.error("No se pudo refrescar el token OAuth: HTTP %s %s", err.code, detail)
        return None
    except (URLError, TimeoutError, ValueError) as err:
        LOG.error("No se pudo refrescar el token OAuth: %s", err)
        return None

    access_token = result.get("access_token")
    if not access_token:
        LOG.error("Respuesta de refresco sin access_token: claves %s",
                  sorted(result.keys()))
        return None

    oauth["accessToken"] = access_token
    if result.get("refresh_token"):
        oauth["refreshToken"] = result["refresh_token"]
    if result.get("expires_in"):
        oauth["expiresAt"] = int((datetime.now(timezone.utc).timestamp() +
                                  float(result["expires_in"])) * 1000)
    data["claudeAiOauth"] = oauth

    try:
        tmp = CREDENTIALS_FILE.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(data), encoding="utf-8")
        tmp.replace(CREDENTIALS_FILE)
        LOG.info("Token OAuth refrescado y guardado (caduca %s)",
                 datetime.fromtimestamp(oauth.get("expiresAt", 0) / 1000))
    except OSError as err:
        LOG.warning("Token refrescado pero no se pudo guardar: %s", err)

    return access_token


def read_oauth_token(force_refresh: bool = False) -> str | None:
    token = os.environ.get("CLAUDE_OAUTH_TOKEN")
    if token:
        return token

    if CREDENTIALS_FILE.exists():
        try:
            data = json.loads(CREDENTIALS_FILE.read_text(encoding="utf-8"))
        except (ValueError, OSError) as err:
            LOG.warning("No se pudo leer %s: %s", CREDENTIALS_FILE, err)
        else:
            oauth = data.get("claudeAiOauth", {})
            token = oauth.get("accessToken")
            expires_at = oauth.get("expiresAt") or 0
            now_ms = datetime.now(timezone.utc).timestamp() * 1000
            expired = now_ms > (expires_at - TOKEN_REFRESH_MARGIN_MS)
            if token and not expired and not force_refresh:
                return token
            refreshed = _refresh_oauth_token(data)
            if refreshed:
                return refreshed
            if token:
                LOG.warning("Usando access token posiblemente caducado "
                            "(refresco fallido)")
                return token

    return _token_from_credential_manager()


# ---------------------------------------------------------------------------
# Endpoint de uso
# ---------------------------------------------------------------------------

def _minutes_until(iso_ts: str | None) -> int:
    if not iso_ts:
        return MIN_UNKNOWN
    try:
        when = datetime.fromisoformat(str(iso_ts).replace("Z", "+00:00"))
    except ValueError:
        return MIN_UNKNOWN
    if when.tzinfo is None:
        when = when.replace(tzinfo=timezone.utc)
    minutes = int((when - datetime.now(timezone.utc)).total_seconds() // 60)
    return max(0, min(minutes, MIN_UNKNOWN - 1))


def _pct(value) -> int:
    try:
        return max(0, min(100, int(round(float(value)))))
    except (TypeError, ValueError):
        return PCT_UNKNOWN


def _find_section(usage: dict, names: list[str]) -> dict:
    for name in names:
        section = usage.get(name)
        if isinstance(section, dict):
            return section
    return {}


# Último paquete válido, para reusar ante fallos transitorios (HTTP 429,
# cortes de red): así la pantalla no parpadea a "--" por un fallo puntual.
_last_good_payload: bytes | None = None
_last_good_at: float = 0.0
LAST_GOOD_MAX_AGE_S = 600

# Enfriamiento tras 429/401 persistente: no insistir cada ciclo, que el
# límite de tasa del endpoint se alimenta de nuestros propios reintentos.
_cooldown_until: float = 0.0
COOLDOWN_429_S = 300
COOLDOWN_401_S = 600


def _fallback_payload(reason: str) -> bytes:
    if _last_good_payload and (time.monotonic() - _last_good_at) < LAST_GOOD_MAX_AGE_S:
        LOG.warning("%s; reenvio el ultimo valor conocido", reason)
        return _last_good_payload
    LOG.error("%s (sin valor reciente que reenviar)", reason)
    return UNKNOWN_PAYLOAD


def _request_usage(token: str) -> dict:
    request = Request(USAGE_URL, headers={
        "Authorization": f"Bearer {token}",
        "anthropic-beta": OAUTH_BETA_HEADER,
        "Content-Type": "application/json",
        "User-Agent": USER_AGENT,
    })
    with urlopen(request, timeout=20) as response:
        return json.loads(response.read())


def fetch_usage_payload() -> bytes:
    """Consulta el endpoint y devuelve el paquete de 6 bytes para el teclado.
    Ante fallos transitorios reenvía el último valor válido (max 10 min)."""
    global _last_good_payload, _last_good_at, _cooldown_until

    if time.monotonic() < _cooldown_until:
        return _fallback_payload("En enfriamiento por errores previos del endpoint")

    token = read_oauth_token()
    if not token:
        return _fallback_payload(
            "Sin token OAuth: inicia sesion en Claude Code CLI ('claude') "
            "o define CLAUDE_OAUTH_TOKEN")

    try:
        try:
            usage = _request_usage(token)
        except HTTPError as err:
            if err.code != 401:
                raise
            # 401 con token "vigente": forzar un refresco y reintentar una vez
            LOG.info("HTTP 401: forzando refresco del token y reintentando...")
            token = read_oauth_token(force_refresh=True)
            if not token:
                raise
            usage = _request_usage(token)
    except HTTPError as err:
        if err.code == 429:
            _cooldown_until = time.monotonic() + COOLDOWN_429_S
            return _fallback_payload(
                f"HTTP 429 (limite de tasa); pauso las consultas {COOLDOWN_429_S} s")
        if err.code == 401:
            _cooldown_until = time.monotonic() + COOLDOWN_401_S
            return _fallback_payload(
                f"HTTP 401 persistente (refresco fallido); pauso {COOLDOWN_401_S} s")
        return _fallback_payload(f"Endpoint de uso devolvio HTTP {err.code}")
    except (URLError, TimeoutError, ValueError) as err:
        return _fallback_payload(f"No se pudo consultar el uso: {err}")

    session = _find_section(usage, ["five_hour", "5h", "session"])
    weekly = _find_section(usage, ["seven_day", "7d", "weekly", "seven_day_overall"])

    session_pct = _pct(session.get("utilization"))
    weekly_pct = _pct(weekly.get("utilization"))
    session_min = _minutes_until(session.get("resets_at"))
    weekly_min = _minutes_until(weekly.get("resets_at"))

    if session_pct == PCT_UNKNOWN and weekly_pct == PCT_UNKNOWN:
        LOG.warning("Respuesta del endpoint sin datos reconocibles: claves %s",
                    sorted(usage.keys()))

    LOG.info("Sesion 5h: %s%% (reset en %s min) | Semana: %s%% (reset en %s min)",
             session_pct, session_min, weekly_pct, weekly_min)
    payload = struct.pack("<BBHH", session_pct, weekly_pct, session_min, weekly_min)
    if session_pct != PCT_UNKNOWN or weekly_pct != PCT_UNKNOWN:
        _last_good_payload = payload
        _last_good_at = time.monotonic()
    return payload


# ---------------------------------------------------------------------------
# BLE
# ---------------------------------------------------------------------------

def _address_from_windows_pnp(name: str) -> str | None:
    """Busca un dispositivo BLE emparejado por nombre y extrae su MAC del
    InstanceId de Windows (BTHLE\\DEV_AABBCCDDEEFF\\...)."""
    if sys.platform != "win32":
        return None
    try:
        out = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             f"Get-PnpDevice -Class Bluetooth | Where-Object {{ $_.FriendlyName -like '{name}*' }} "
             "| Select-Object -ExpandProperty InstanceId"],
            capture_output=True, text=True, timeout=30, check=False,
            creationflags=subprocess.CREATE_NO_WINDOW)
    except (OSError, subprocess.TimeoutExpired):
        return None
    for line in out.stdout.splitlines():
        line = line.strip().upper()
        marker = "DEV_"
        idx = line.find(marker)
        if idx >= 0 and len(line) >= idx + len(marker) + 12:
            raw = line[idx + len(marker):idx + len(marker) + 12]
            if all(c in "0123456789ABCDEF" for c in raw):
                return ":".join(raw[i:i + 2] for i in range(0, 12, 2))
    return None


async def resolve_address(args) -> str:
    if args.address:
        return args.address

    LOG.info("Buscando '%s' por anuncio BLE...", args.name)
    device = await BleakScanner.find_device_by_name(args.name, timeout=10.0)
    if device:
        LOG.info("Encontrado anunciandose: %s", device.address)
        return device.address

    LOG.info("No se anuncia (normal si ya esta conectado); buscando en "
             "dispositivos emparejados de Windows...")
    address = _address_from_windows_pnp(args.name)
    if address:
        LOG.info("Encontrado emparejado: %s", address)
        return address

    raise RuntimeError(
        f"No se encontro '{args.name}'. Empareja el teclado con Windows o "
        "pasa --address AA:BB:CC:DD:EE:FF")


async def run(args) -> None:
    loop = asyncio.get_running_loop()
    address = None

    while True:
        try:
            # Resolver (o re-resolver) la dirección dentro del bucle: al
            # iniciar sesión el teclado puede no estar disponible aún, y la
            # identidad BLE de ZMK puede cambiar entre perfiles/reflasheos.
            if address is None:
                address = await resolve_address(args)
            LOG.info("Conectando a %s ...", address)
            # use_cached_services=False: descubrir servicios de verdad en cada
            # conexión. La caché GATT de Windows queda obsoleta al reflashear
            # firmware con otros servicios y da "Characteristic not found".
            # address_type="random": ZMK usa direcciones random estáticas.
            client = BleakClient(address,
                                 winrt={"use_cached_services": False,
                                        "address_type": "random"})
            # bleak (WinRT) escanea buscando el anuncio antes de conectar,
            # pero un dispositivo ya conectado a Windows no se anuncia.
            # Inyectar la dirección como _device_info salta ese escaneo y
            # conecta directamente sobre el enlace existente.
            try:
                client._backend._device_info = int(address.replace(":", ""), 16)
            except (AttributeError, ValueError):
                pass  # otro backend u otra versión de bleak: dejar el flujo normal
            async with client:
                LOG.info("Conectado")
                while True:
                    payload = await loop.run_in_executor(None, fetch_usage_payload)
                    await client.write_gatt_char(CHAR_UUID, payload, response=True)
                    LOG.debug("Paquete enviado: %s", payload.hex())
                    if args.once:
                        return
                    await asyncio.sleep(args.interval)
        except Exception as err:  # bleak lanza tipos variados segun backend
            if args.once:
                raise
            LOG.warning("BLE: %s: %s; reintento en 15 s", type(err).__name__, err)
            address = None  # re-resolver por si cambió la identidad BLE
            await asyncio.sleep(15)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--address", help="MAC BLE del teclado (omite el descubrimiento)")
    parser.add_argument("--name", default="Sofle", help="Nombre BLE del teclado (def: Sofle)")
    parser.add_argument("--interval", type=int, default=60,
                        help="Segundos entre actualizaciones (def: 60)")
    parser.add_argument("--once", action="store_true",
                        help="Envia una actualizacion y termina (para probar)")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    # Log a archivo además de consola: bajo pythonw/tarea programada no hay
    # consola, y sin esto los fallos son invisibles.
    log_path = Path(os.environ.get("LOCALAPPDATA", Path.home())) / "claude_usage_daemon.log"
    file_handler = logging.handlers.RotatingFileHandler(
        log_path, maxBytes=256 * 1024, backupCount=1, encoding="utf-8")
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s",
                        datefmt="%H:%M:%S",
                        handlers=[logging.StreamHandler(), file_handler])
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
