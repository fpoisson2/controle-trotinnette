#!/usr/bin/env python3
"""
provision.py — Provisionnement en batch des trottinettes ESP32

Compile le firmware une fois avec PlatformIO, detecte automatiquement les
ports USB (CP2102/CH340/FTDI), flashe tous les ESP32 en parallele, surveille
le boot serie, extrait l'adresse MAC, genere des QR codes et un rapport CSV.

Usage:
    python tools/provision.py [--port PORT] [--skip-compile] [--proxy-url URL]

Exemples:
    python tools/provision.py                           # auto-detect + compile + flash
    python tools/provision.py --skip-compile            # flash seulement
    python tools/provision.py --port COM3               # un seul port
    python tools/provision.py --proxy-url https://my.proxy.com
"""

import argparse
import csv
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime

# ─── Dependances tierces ────────────────────────────────────────────────────
try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("\033[91m[ERREUR] pyserial requis : pip install pyserial\033[0m")
    sys.exit(1)

try:
    import qrcode
    QR_DISPONIBLE = True
except ImportError:
    QR_DISPONIBLE = False

# ─── Constantes ─────────────────────────────────────────────────────────────
CHIPS_USB = ["CP210", "CH340", "FTDI", "USB"]
BAUD_FLASH = 921600
BAUD_MONITOR = 115200
TIMEOUT_BOOT_SEC = 60
FLASH_OFFSET = "0x10000"

# Patterns serie pour detecter le boot
RE_WIFI = re.compile(r"\[wifi\] connect", re.IGNORECASE)
RE_PROXY = re.compile(r"\[ws-proxy\] connect", re.IGNORECASE)
RE_MAC = re.compile(r"mac.*?([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})", re.IGNORECASE)

# Couleurs ANSI
class Couleur:
    VERT = "\033[92m"
    JAUNE = "\033[93m"
    ROUGE = "\033[91m"
    CYAN = "\033[96m"
    GRIS = "\033[90m"
    GRAS = "\033[1m"
    FIN = "\033[0m"


def log_info(msg: str) -> None:
    """Affiche un message d'information en cyan."""
    print(f"{Couleur.CYAN}[INFO]{Couleur.FIN} {msg}")


def log_ok(msg: str) -> None:
    """Affiche un message de succes en vert."""
    print(f"{Couleur.VERT}[OK]{Couleur.FIN}   {msg}")


def log_warn(msg: str) -> None:
    """Affiche un avertissement en jaune."""
    print(f"{Couleur.JAUNE}[WARN]{Couleur.FIN} {msg}")


def log_err(msg: str) -> None:
    """Affiche une erreur en rouge."""
    print(f"{Couleur.ROUGE}[ERR]{Couleur.FIN}  {msg}")


# ─── Detection des ports USB ────────────────────────────────────────────────

def detecter_ports() -> list[str]:
    """Detecte les ports serie correspondant aux puces USB courantes (CP2102, CH340, FTDI)."""
    ports_trouves = []
    for port_info in serial.tools.list_ports.comports():
        description = (port_info.description or "").upper()
        vid_pid = f"{port_info.vid:04X}:{port_info.pid:04X}" if port_info.vid else ""
        for chip in CHIPS_USB:
            if chip.upper() in description or chip.upper() in vid_pid:
                ports_trouves.append(port_info.device)
                break
    ports_trouves.sort()
    return ports_trouves


# ─── Compilation PlatformIO ─────────────────────────────────────────────────

def compiler_firmware(firmware_dir: str) -> str:
    """
    Lance `pio run -e esp32dev` dans le repertoire firmware.

    Retourne le chemin du .bin compile.
    Leve RuntimeError si la compilation echoue.
    """
    log_info(f"Compilation du firmware dans {firmware_dir} ...")
    resultat = subprocess.run(
        ["pio", "run", "-e", "esp32dev"],
        cwd=firmware_dir,
        capture_output=True,
        text=True,
    )
    if resultat.returncode != 0:
        log_err("Echec de compilation PlatformIO :")
        print(resultat.stderr)
        raise RuntimeError("Compilation echouee")

    # Chemin par defaut du binaire PlatformIO
    bin_path = os.path.join(firmware_dir, ".pio", "build", "esp32dev", "firmware.bin")
    if not os.path.isfile(bin_path):
        raise FileNotFoundError(f"Binaire introuvable : {bin_path}")

    taille_ko = os.path.getsize(bin_path) / 1024
    log_ok(f"Firmware compile : {bin_path} ({taille_ko:.0f} Ko)")
    return bin_path


# ─── Flash et surveillance d'un port ────────────────────────────────────────

def flasher_et_surveiller(port: str, bin_path: str, proxy_url: str) -> dict:
    """
    Flashe le firmware sur un port, puis surveille la sortie serie
    pour confirmer le boot WiFi + proxy et extraire l'adresse MAC.

    Retourne un dict avec les resultats du provisionnement.
    """
    resultat = {
        "port": port,
        "mac": "",
        "wifi": False,
        "proxy": False,
        "status": "FAIL",
        "timestamp": datetime.now().isoformat(timespec="seconds"),
        "erreur": "",
    }

    # ── Etape 1 : Flash via esptool ─────────────────────────────────────
    log_info(f"[{port}] Flash en cours ...")
    try:
        proc = subprocess.run(
            [
                "esptool.py",
                "--chip", "esp32",
                "--port", port,
                "--baud", str(BAUD_FLASH),
                "write_flash", FLASH_OFFSET, bin_path,
            ],
            capture_output=True,
            text=True,
            timeout=120,
        )
        if proc.returncode != 0:
            resultat["erreur"] = f"esptool erreur: {proc.stderr.strip()[-200:]}"
            log_err(f"[{port}] Echec flash : {resultat['erreur']}")
            return resultat
    except FileNotFoundError:
        # esptool.py pas dans le PATH, essayer en tant que module Python
        try:
            proc = subprocess.run(
                [
                    sys.executable, "-m", "esptool",
                    "--chip", "esp32",
                    "--port", port,
                    "--baud", str(BAUD_FLASH),
                    "write_flash", FLASH_OFFSET, bin_path,
                ],
                capture_output=True,
                text=True,
                timeout=120,
            )
            if proc.returncode != 0:
                resultat["erreur"] = f"esptool erreur: {proc.stderr.strip()[-200:]}"
                log_err(f"[{port}] Echec flash : {resultat['erreur']}")
                return resultat
        except Exception as e:
            resultat["erreur"] = f"esptool introuvable : {e}"
            log_err(f"[{port}] {resultat['erreur']}")
            return resultat
    except subprocess.TimeoutExpired:
        resultat["erreur"] = "Flash timeout (120s)"
        log_err(f"[{port}] {resultat['erreur']}")
        return resultat

    log_ok(f"[{port}] Flash termine")

    # ── Etape 2 : Surveillance serie ────────────────────────────────────
    log_info(f"[{port}] Surveillance serie (max {TIMEOUT_BOOT_SEC}s) ...")
    try:
        # Petit delai pour laisser l'ESP32 rebooter apres le flash
        time.sleep(2)
        ser = serial.Serial(port, BAUD_MONITOR, timeout=1)
    except serial.SerialException as e:
        resultat["erreur"] = f"Impossible d'ouvrir le port serie : {e}"
        log_err(f"[{port}] {resultat['erreur']}")
        return resultat

    debut = time.time()
    try:
        while time.time() - debut < TIMEOUT_BOOT_SEC:
            try:
                ligne = ser.readline().decode("utf-8", errors="replace").strip()
            except serial.SerialException:
                break

            if not ligne:
                continue

            # Afficher la sortie serie en gris
            print(f"{Couleur.GRIS}  [{port}] {ligne}{Couleur.FIN}")

            # Verifier connexion WiFi
            if RE_WIFI.search(ligne):
                resultat["wifi"] = True
                log_ok(f"[{port}] WiFi connecte")

            # Verifier connexion proxy WebSocket
            if RE_PROXY.search(ligne):
                resultat["proxy"] = True
                log_ok(f"[{port}] Proxy connecte")

            # Extraire adresse MAC
            match_mac = RE_MAC.search(ligne)
            if match_mac and not resultat["mac"]:
                resultat["mac"] = match_mac.group(1).upper()
                log_ok(f"[{port}] MAC detectee : {resultat['mac']}")

            # Si tout est bon, on peut arreter
            if resultat["wifi"] and resultat["proxy"] and resultat["mac"]:
                break
    finally:
        ser.close()

    # ── Determination du statut ─────────────────────────────────────────
    if resultat["wifi"] and resultat["proxy"] and resultat["mac"]:
        resultat["status"] = "PASS"
    elif resultat["wifi"] and resultat["mac"]:
        resultat["status"] = "PARTIAL"
        resultat["erreur"] = "WiFi OK mais proxy non connecte"
    elif resultat["wifi"]:
        resultat["status"] = "PARTIAL"
        resultat["erreur"] = "WiFi OK mais MAC non detectee"
    else:
        resultat["erreur"] = resultat["erreur"] or "Boot timeout — WiFi non detecte"

    couleur = Couleur.VERT if resultat["status"] == "PASS" else Couleur.JAUNE if resultat["status"] == "PARTIAL" else Couleur.ROUGE
    print(f"{couleur}{Couleur.GRAS}[{port}] Statut : {resultat['status']}{Couleur.FIN}")

    return resultat


# ─── Generation QR codes ────────────────────────────────────────────────────

def generer_qr(mac: str, proxy_url: str, dossier_qr: str) -> str | None:
    """
    Genere un QR code PNG pour une trottinette identifiee par sa MAC.

    Retourne le chemin du fichier PNG ou None si qrcode n'est pas installe.
    """
    if not QR_DISPONIBLE:
        return None

    mac_clean = mac.replace(":", "").upper()
    url = f"{proxy_url}/scooter/{mac_clean}"

    os.makedirs(dossier_qr, exist_ok=True)
    fichier = os.path.join(dossier_qr, f"qr_{mac_clean}.png")

    qr = qrcode.QRCode(
        version=1,
        error_correction=qrcode.constants.ERROR_CORRECT_M,
        box_size=10,
        border=4,
    )
    qr.add_data(url)
    qr.make(fit=True)

    img = qr.make_image(fill_color="black", back_color="white")
    img.save(fichier)
    return fichier


# ─── Rapport CSV ─────────────────────────────────────────────────────────────

def ecrire_rapport(resultats: list[dict], chemin_csv: str) -> None:
    """Ecrit le rapport de provisionnement au format CSV."""
    champs = ["port", "mac", "wifi", "proxy", "status", "timestamp", "erreur"]
    with open(chemin_csv, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=champs)
        writer.writeheader()
        for r in resultats:
            writer.writerow({k: r.get(k, "") for k in champs})
    log_ok(f"Rapport CSV : {chemin_csv}")


# ─── Tableau recapitulatif ──────────────────────────────────────────────────

def afficher_resume(resultats: list[dict]) -> None:
    """Affiche un tableau recapitulatif colore dans le terminal."""
    print()
    print(f"{Couleur.GRAS}{'='*72}")
    print(f"  RAPPORT DE PROVISIONNEMENT — {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"{'='*72}{Couleur.FIN}")
    print(f"  {'Port':<10} {'MAC':<20} {'WiFi':<6} {'Proxy':<7} {'Statut':<10}")
    print(f"  {'-'*10} {'-'*20} {'-'*6} {'-'*7} {'-'*10}")

    nb_pass = 0
    nb_fail = 0
    for r in resultats:
        couleur = Couleur.VERT if r["status"] == "PASS" else Couleur.JAUNE if r["status"] == "PARTIAL" else Couleur.ROUGE
        wifi = "OK" if r["wifi"] else "--"
        proxy = "OK" if r["proxy"] else "--"
        mac = r["mac"] or "N/A"
        print(f"  {r['port']:<10} {mac:<20} {wifi:<6} {proxy:<7} {couleur}{r['status']:<10}{Couleur.FIN}")
        if r["status"] == "PASS":
            nb_pass += 1
        else:
            nb_fail += 1

    print(f"\n  {Couleur.VERT}{nb_pass} PASS{Couleur.FIN}  /  {Couleur.ROUGE}{nb_fail} FAIL{Couleur.FIN}  /  {len(resultats)} total")
    print()


# ─── Point d'entree ─────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Provisionnement en batch des trottinettes ESP32",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--port",
        help="Port serie specifique (ex: COM3, /dev/ttyUSB0). Par defaut : auto-detection.",
    )
    parser.add_argument(
        "--skip-compile",
        action="store_true",
        help="Ne pas recompiler le firmware (utiliser le .bin existant).",
    )
    parser.add_argument(
        "--proxy-url",
        default="https://trottinette.cegeplimoilou.ca",
        help="URL du proxy pour les QR codes (defaut: %(default)s).",
    )
    args = parser.parse_args()

    # ── Repertoires ─────────────────────────────────────────────────────
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_dir = os.path.dirname(script_dir)
    firmware_dir = os.path.join(project_dir, "firmware")
    dossier_qr = os.path.join(script_dir, "qr")
    chemin_csv = os.path.join(script_dir, "provision_report.csv")

    if not os.path.isdir(firmware_dir):
        log_err(f"Repertoire firmware introuvable : {firmware_dir}")
        sys.exit(1)

    # ── Compilation ─────────────────────────────────────────────────────
    bin_path = os.path.join(firmware_dir, ".pio", "build", "esp32dev", "firmware.bin")

    if not args.skip_compile:
        bin_path = compiler_firmware(firmware_dir)
    else:
        if not os.path.isfile(bin_path):
            log_err(f"Binaire introuvable (--skip-compile) : {bin_path}")
            log_info("Lancez d'abord sans --skip-compile pour compiler.")
            sys.exit(1)
        log_info(f"Compilation ignoree, binaire existant : {bin_path}")

    # ── Detection ports ─────────────────────────────────────────────────
    if args.port:
        ports = [args.port]
        log_info(f"Port specifie : {args.port}")
    else:
        ports = detecter_ports()
        if not ports:
            log_err("Aucun port USB ESP32 detecte (CP2102/CH340/FTDI)")
            log_info("Verifiez les cables USB et les pilotes.")
            sys.exit(1)
        log_ok(f"{len(ports)} port(s) detecte(s) : {', '.join(ports)}")

    # ── Flash et surveillance en parallele ──────────────────────────────
    print(f"\n{Couleur.GRAS}Debut du provisionnement de {len(ports)} unite(s)...{Couleur.FIN}\n")
    resultats = []

    with ThreadPoolExecutor(max_workers=len(ports)) as pool:
        futures = {
            pool.submit(flasher_et_surveiller, port, bin_path, args.proxy_url): port
            for port in ports
        }
        for future in as_completed(futures):
            port = futures[future]
            try:
                res = future.result()
                resultats.append(res)
            except Exception as e:
                log_err(f"[{port}] Exception inattendue : {e}")
                resultats.append({
                    "port": port,
                    "mac": "",
                    "wifi": False,
                    "proxy": False,
                    "status": "FAIL",
                    "timestamp": datetime.now().isoformat(timespec="seconds"),
                    "erreur": str(e),
                })

    # Trier par port pour un affichage coherent
    resultats.sort(key=lambda r: r["port"])

    # ── Generation des QR codes ─────────────────────────────────────────
    if QR_DISPONIBLE:
        nb_qr = 0
        for r in resultats:
            if r["mac"]:
                fichier_qr = generer_qr(r["mac"], args.proxy_url, dossier_qr)
                if fichier_qr:
                    nb_qr += 1
                    log_ok(f"QR genere : {fichier_qr}")
        if nb_qr:
            log_ok(f"{nb_qr} QR code(s) genere(s) dans {dossier_qr}/")
    else:
        log_warn("Module qrcode non installe — QR codes ignores (pip install qrcode[pil])")

    # ── Rapport CSV ─────────────────────────────────────────────────────
    ecrire_rapport(resultats, chemin_csv)

    # ── Resume ──────────────────────────────────────────────────────────
    afficher_resume(resultats)


if __name__ == "__main__":
    main()
