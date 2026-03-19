#!/usr/bin/env python3
"""Upload firmware via le proxy WebSocket (OTA à distance).

Usage PlatformIO :  pio run -e esp32dev-proxy-ota --target upload
Usage manuel     :  python3 ota_upload.py .pio/build/esp32dev-proxy-ota/firmware.bin
"""

import sys, os, json, getpass, requests

# ── Charger le .env du projet (à côté de firmware/) ──
def load_dotenv(path):
    if not os.path.isfile(path):
        return
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#') or '=' not in line:
                continue
            k, v = line.split('=', 1)
            v = v.strip().strip('"').strip("'")
            if k not in os.environ:
                os.environ[k] = v

load_dotenv(os.path.join(os.path.dirname(__file__), '..', '.env'))

# ── Config ──
_domain    = os.environ.get("PROXY_DOMAIN", "www.trotilou.ca")
PROXY_URL  = os.environ.get("OTA_PROXY_URL", f"https://{_domain}")
USERNAME   = os.environ.get("AUTH_USERNAME", "admin")
PASSWORD   = os.environ.get("AUTH_PASSWORD", "")

def main():
    # Trouver le firmware.bin
    if len(sys.argv) >= 2:
        fw_path = sys.argv[1]
    else:
        # PlatformIO passe le chemin en $SOURCE
        fw_path = os.environ.get("SOURCE", "")
    if not fw_path or not os.path.isfile(fw_path):
        print(f"[ota] firmware introuvable : {fw_path}")
        sys.exit(1)

    fw_size = os.path.getsize(fw_path)
    print(f"[ota] firmware : {fw_path} ({fw_size} bytes)")

    # Login pour obtenir un JWT
    password = PASSWORD or getpass.getpass("[ota] Mot de passe proxy : ")

    print(f"[ota] login sur {PROXY_URL}...")
    r = requests.post(f"{PROXY_URL}/login", json={"username": USERNAME, "password": password})
    if r.status_code != 200:
        print(f"[ota] login échoué : {r.status_code} {r.text}")
        sys.exit(1)
    token = r.json()["token"]

    # Upload du firmware
    print(f"[ota] upload vers {PROXY_URL}/ota...")
    with open(fw_path, "rb") as f:
        r = requests.post(
            f"{PROXY_URL}/ota",
            data=f,
            headers={
                "Content-Type": "application/octet-stream",
                "Authorization": f"Bearer {token}",
            },
            timeout=120,
        )
    if r.status_code == 200:
        print(f"[ota] succès : {r.json()}")
    else:
        print(f"[ota] échec : {r.status_code} {r.text}")
        sys.exit(1)

if __name__ == "__main__":
    main()
