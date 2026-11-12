"""
3DS Screen Link - application PC (Windows et macOS)

Envoie l'écran du PC sur l'écran du haut de la 3DS et reçoit en retour
les boutons (manette Xbox virtuelle via ViGEm, ou clavier) et le clavier tactile.

Dépendances : pip install -r requirements.txt
Manette virtuelle : installer le pilote ViGEmBus (https://github.com/nefarius/ViGEmBus/releases)
"""

import array
import json
import shlex
import os
import queue
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import tkinter as tk
import urllib.request
from io import BytesIO
from tkinter import messagebox, scrolledtext, ttk

import mss
from PIL import Image
from pynput.keyboard import Controller as KeyboardController
from pynput.keyboard import Key, KeyCode

APP_NAME = "3DS Screen Link"
COPYRIGHT = "\u00a9 2026 heivoli-network.fr"
VERSION = "1.5.2"
# Dépôt GitHub où sont publiées les releases (format "pseudo/nom-du-depot")
GITHUB_REPO = "Heivoli-dev/3ds-screen-link"
UPDATE_ASSET_EXE = "ScreenLinkPC.exe"
UPDATE_ASSET_PY = "screenlink_pc.py"
UPDATE_ASSET_MAC = "ScreenLinkPC-macOS.zip"

IS_WINDOWS = sys.platform == "win32"
IS_MAC = sys.platform == "darwin"

FRAME_W, FRAME_H = 400, 240
AUDIO_RATE = 32000          # doit correspondre à AUDIO_RATE dans l'appli 3DS
AUDIO_CHUNK = 640           # échantillons par canal et par paquet (20 ms)
MAX_IN_FLIGHT = 2
DEFAULT_PORT = 8334

# Paquets PC -> 3DS
PKT_PC_HELLO, PKT_PC_FRAME, PKT_PC_PING, PKT_PC_BYE, PKT_PC_AUDIO = 0x10, 0x11, 0x12, 0x13, 0x14
# Paquets 3DS -> PC
PKT_HELLO, PKT_INPUT, PKT_KEY, PKT_ACK = 0x01, 0x02, 0x03, 0x04

# Bits des boutons 3DS (libctru)
BTN = {
    "A": 1 << 0, "B": 1 << 1, "SELECT": 1 << 2, "START": 1 << 3,
    "DRIGHT": 1 << 4, "DLEFT": 1 << 5, "DUP": 1 << 6, "DDOWN": 1 << 7,
    "R": 1 << 8, "L": 1 << 9, "X": 1 << 10, "Y": 1 << 11,
    "ZL": 1 << 14, "ZR": 1 << 15,
}

MOD_SHIFT, MOD_CTRL, MOD_ALT = 0x01, 0x02, 0x04


def app_dir():
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))


def config_dir():
    # Sur Mac, une appli ne doit pas écrire dans son propre paquet .app
    if IS_MAC:
        d = os.path.expanduser("~/Library/Application Support/3DS Screen Link")
        os.makedirs(d, exist_ok=True)
        return d
    return app_dir()


CONFIG_PATH = os.path.join(config_dir(), "screenlink_config.json")


def resource(name):
    """Fichier fourni avec l'appli (dans le .exe / le .app, ou à côté du script)."""
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, name)

DEFAULT_CONFIG = {
    "ip": "192.168.1.50",
    "port": DEFAULT_PORT,
    "fps": 20,
    "quality": 75,
    "monitor": 1,
    "stretch": False,
    "full_color": True,
    "audio": True,         # envoie le son du PC à la 3DS
    "check_updates": True, # interroge GitHub au démarrage pour proposer les mises à jour
    "desktop_shortcut_done": False,  # raccourci Bureau déjà créé une fois (Windows)
    "last_version": "",    # dernière version lancée (pour rafraîchir les icônes de Windows)    # JPEG 4:4:4 : couleurs et texte plus nets, images un peu plus lourdes
    "controller": "xbox",  # "xbox" (ViGEm) ou "keyboard"
    "cpad": "both",        # Circle Pad en mode Xbox : "stick", "keys" (ZQSD) ou "both"
    # Mapping utilisé en mode "keyboard" (boutons 3DS -> touches PC)
    "keymap": {
        "A": "enter", "B": "esc", "X": "e", "Y": "space",
        "L": "shift", "R": "ctrl", "ZL": "q", "ZR": "f",
        "START": "enter", "SELECT": "tab",
        "DUP": "up", "DDOWN": "down", "DLEFT": "left", "DRIGHT": "right",
        "CPAD_UP": "z", "CPAD_DOWN": "s", "CPAD_LEFT": "q", "CPAD_RIGHT": "d",
    },
}


def load_config():
    cfg = json.loads(json.dumps(DEFAULT_CONFIG))
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            user = json.load(f)
        keymap = user.pop("keymap", None)
        cfg.update(user)
        if isinstance(keymap, dict):
            cfg["keymap"].update(keymap)
    except FileNotFoundError:
        pass
    except Exception:
        pass
    return cfg


def save_config(cfg):
    try:
        with open(CONFIG_PATH, "w", encoding="utf-8") as f:
            json.dump(cfg, f, indent=2, ensure_ascii=False)
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Clavier
# ---------------------------------------------------------------------------

SPECIAL_KEYS = {
    "esc": Key.esc, "backspace": Key.backspace, "tab": Key.tab, "enter": Key.enter,
    "space": Key.space, "delete": Key.delete, "shift": Key.shift, "ctrl": Key.ctrl,
    "alt": Key.alt, "left": Key.left, "right": Key.right, "up": Key.up, "down": Key.down,
    "home": Key.home, "end": Key.end, "page_up": Key.page_up, "page_down": Key.page_down,
}
for _i in range(1, 13):
    SPECIAL_KEYS[f"f{_i}"] = getattr(Key, f"f{_i}")


def to_pynput_key(name):
    name = name.lower()
    if name in SPECIAL_KEYS:
        return SPECIAL_KEYS[name]
    if len(name) == 1:
        return KeyCode.from_char(name)
    return None


class KeyboardInjector:
    """Gère les touches du clavier tactile et garde la trace de ce qui est enfoncé."""

    def __init__(self):
        self.kb = KeyboardController()
        self.pressed = set()
        self.lock = threading.Lock()

    def _mods(self, mods):
        out = []
        if mods & MOD_SHIFT:
            out.append(Key.shift)
        if mods & MOD_CTRL:
            out.append(Key.ctrl)
        if mods & MOD_ALT:
            out.append(Key.alt)
        return out

    def key_event(self, down, mods, name):
        key = to_pynput_key(name)
        if key is None:
            return
        with self.lock:
            if down:
                for m in self._mods(mods):
                    self.kb.press(m)
                self.kb.press(key)
                self.pressed.add(key)
            else:
                self.kb.release(key)
                self.pressed.discard(key)
                for m in reversed(self._mods(mods)):
                    self.kb.release(m)

    def press(self, key):
        with self.lock:
            if key not in self.pressed:
                self.kb.press(key)
                self.pressed.add(key)

    def release(self, key):
        with self.lock:
            if key in self.pressed:
                self.kb.release(key)
                self.pressed.discard(key)

    def release_all(self):
        with self.lock:
            for k in list(self.pressed):
                try:
                    self.kb.release(k)
                except Exception:
                    pass
            self.pressed.clear()
            for m in (Key.shift, Key.ctrl, Key.alt):
                try:
                    self.kb.release(m)
                except Exception:
                    pass


# ---------------------------------------------------------------------------
# Manette
# ---------------------------------------------------------------------------

CPAD_MAX = 156.0
CSTICK_MAX = 146.0


def clamp(v):
    return max(-1.0, min(1.0, v))


class XboxBackend:
    """Manette Xbox 360 virtuelle (nécessite le pilote ViGEmBus)."""

    def __init__(self, use_left_stick=True):
        import vgamepad as vg  # import ici : échoue si ViGEmBus est absent
        self.use_left_stick = use_left_stick
        self.vg = vg
        self.pad = vg.VX360Gamepad()
        B = vg.XUSB_BUTTON
        self.map = {
            "A": B.XUSB_GAMEPAD_A, "B": B.XUSB_GAMEPAD_B,
            "X": B.XUSB_GAMEPAD_X, "Y": B.XUSB_GAMEPAD_Y,
            "L": B.XUSB_GAMEPAD_LEFT_SHOULDER, "R": B.XUSB_GAMEPAD_RIGHT_SHOULDER,
            "START": B.XUSB_GAMEPAD_START, "SELECT": B.XUSB_GAMEPAD_BACK,
            "DUP": B.XUSB_GAMEPAD_DPAD_UP, "DDOWN": B.XUSB_GAMEPAD_DPAD_DOWN,
            "DLEFT": B.XUSB_GAMEPAD_DPAD_LEFT, "DRIGHT": B.XUSB_GAMEPAD_DPAD_RIGHT,
        }

    def update(self, held, cx, cy, sx, sy):
        for name, xb in self.map.items():
            if held & BTN[name]:
                self.pad.press_button(button=xb)
            else:
                self.pad.release_button(button=xb)
        self.pad.left_trigger_float(1.0 if held & BTN["ZL"] else 0.0)
        self.pad.right_trigger_float(1.0 if held & BTN["ZR"] else 0.0)
        if self.use_left_stick:
            self.pad.left_joystick_float(clamp(cx / CPAD_MAX), clamp(cy / CPAD_MAX))
        else:
            self.pad.left_joystick_float(0.0, 0.0)
        self.pad.right_joystick_float(clamp(sx / CSTICK_MAX), clamp(sy / CSTICK_MAX))
        self.pad.update()

    def reset(self):
        try:
            self.pad.reset()
            self.pad.update()
        except Exception:
            pass


class KeyboardBackend:
    """Boutons 3DS convertis en touches clavier (mapping dans screenlink_config.json)."""

    THRESHOLD = 50

    def __init__(self, injector, keymap):
        self.inj = injector
        self.keymap = {k: to_pynput_key(v) for k, v in keymap.items() if to_pynput_key(v)}

    def update(self, held, cx, cy, sx, sy):
        wanted = {}
        for name in BTN:
            if name in self.keymap:
                wanted[self.keymap[name]] = wanted.get(self.keymap[name], False) or bool(held & BTN[name])
        stick = {
            "CPAD_UP": cy > self.THRESHOLD, "CPAD_DOWN": cy < -self.THRESHOLD,
            "CPAD_LEFT": cx < -self.THRESHOLD, "CPAD_RIGHT": cx > self.THRESHOLD,
        }
        for name, active in stick.items():
            if name in self.keymap:
                wanted[self.keymap[name]] = wanted.get(self.keymap[name], False) or active
        # Une même touche peut être liée à plusieurs boutons : on fusionne avant d'appliquer
        for key, active in wanted.items():
            if active:
                self.inj.press(key)
            else:
                self.inj.release(key)

    def reset(self):
        self.inj.release_all()


class MultiBackend:
    """Envoie les entrées à plusieurs sorties (ex. manette Xbox + touches ZQSD)."""

    def __init__(self, *backends):
        self.backends = backends

    def update(self, held, cx, cy, sx, sy):
        for b in self.backends:
            b.update(held, cx, cy, sx, sy)

    def reset(self):
        for b in self.backends:
            b.reset()


# ---------------------------------------------------------------------------
# Connexion avec la 3DS
# ---------------------------------------------------------------------------

def make_packet(ptype, payload=b""):
    return struct.pack(">BI", ptype, len(payload)) + payload


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connexion fermée par la console")
        buf.extend(chunk)
    return bytes(buf)


class AudioStreamer:
    """Capture le son de Windows (boucle WASAPI) et l'envoie en IMA ADPCM 32 kHz stéréo."""

    def __init__(self, send, log, stop_event):
        self.send = send
        self.log = log
        self.stop_event = stop_event
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()

    def _run(self):
        # Toute erreur, même inattendue, doit apparaître dans le journal
        try:
            self._capture()
        except Exception as e:
            self.log(f"ERREUR son : {type(e).__name__}: {e}")

    def _capture(self):
        self.log("Son : démarrage de la capture...")
        try:
            import audioop
        except Exception as e:
            self.log(f"Son désactivé : module audioop indisponible ({e}).")
            return
        try:
            if IS_WINDOWS:
                import pyaudiowpatch as pyaudio
            else:
                import pyaudio
        except Exception as e:
            mod = "PyAudioWPatch" if IS_WINDOWS else "PyAudio"
            self.log(f"Son désactivé : module {mod} indisponible ({e}).")
            return

        pa = pyaudio.PyAudio()
        stream = None
        try:
            dev = find_windows_loopback(pa, pyaudio) if IS_WINDOWS else find_mac_blackhole(pa)
            if dev is None:
                self.log("Son désactivé : sur Mac, installe BlackHole 2ch "
                         "(https://existential.audio/blackhole/) et choisis-le comme sortie "
                         "(voir le README pour entendre le son sur le Mac en même temps).")
                return

            channels = max(1, int(dev["maxInputChannels"]))
            rate = int(dev["defaultSampleRate"])
            chunks = queue.Queue(maxsize=100)

            def callback(in_data, frame_count, time_info, status):
                try:
                    chunks.put_nowait(in_data)
                except queue.Full:
                    pass
                return (None, pyaudio.paContinue)

            stream = pa.open(format=pyaudio.paInt16, channels=channels, rate=rate, input=True,
                             input_device_index=dev["index"], frames_per_buffer=rate // 100,
                             stream_callback=callback)
            stream.start_stream()
            self.log(f"Son capturé depuis « {dev['name']} » ({rate} Hz) → envoyé à la 3DS.")

            rs_state = None
            adpcm_state = [None, None]
            pcm = bytearray()
            chunk_bytes = AUDIO_CHUNK * 4      # stéréo 16 bits
            t_start = time.time()
            got_data = warned_silence = reported = False
            sent, peak = 0, 0

            while not self.stop_event.is_set():
                try:
                    data = chunks.get(timeout=0.2)
                except queue.Empty:
                    if not got_data and not warned_silence and time.time() - t_start > 5:
                        warned_silence = True
                        self.log("Son : Windows n'envoie rien depuis 5 s. Lance un son sur le PC ; "
                                 f"s'il ne passe toujours pas, il ne sort pas par « {dev['name']} ».")
                    continue
                got_data = True
                if not reported:
                    peak = max(peak, audioop.max(data, 2))
                    if time.time() - t_start > 3 and sent:
                        reported = True
                        self.log(f"Son OK : {sent} paquets envoyés à la 3DS, niveau max "
                                 f"{peak * 100 // 32768} %" + (" (silence : rien ne joue ?)" if peak < 100 else ""))
                data = to_stereo(data, channels, audioop)
                data, rs_state = audioop.ratecv(data, 2, 2, rate, AUDIO_RATE, rs_state)
                pcm.extend(data)
                if len(pcm) > AUDIO_RATE * 4 // 4:       # plus de 250 ms de retard : on rattrape
                    del pcm[:len(pcm) - chunk_bytes]
                while len(pcm) >= chunk_bytes:
                    block = bytes(pcm[:chunk_bytes])
                    del pcm[:chunk_bytes]
                    self.send(make_packet(PKT_PC_AUDIO, encode_audio(block, adpcm_state, audioop)))
                    sent += 1
        except Exception as e:
            if not self.stop_event.is_set():
                self.log(f"ERREUR son : {type(e).__name__}: {e}")
        finally:
            try:
                if stream:
                    stream.stop_stream()
                    stream.close()
            except Exception:
                pass
            pa.terminate()


def find_windows_loopback(pa, pyaudio):
    """Entrée « boucle » WASAPI correspondant à la sortie audio par défaut de Windows."""
    wasapi = pa.get_host_api_info_by_type(pyaudio.paWASAPI)
    dev = pa.get_device_info_by_index(wasapi["defaultOutputDevice"])
    if dev.get("isLoopbackDevice"):
        return dev
    for lb in pa.get_loopback_device_info_generator():
        if dev["name"] in lb["name"]:
            return lb
    names = [lb["name"] for lb in pa.get_loopback_device_info_generator()]
    raise RuntimeError(f"pas de capture possible pour « {dev['name']} » "
                       f"(disponibles : {', '.join(names) or 'aucune'})")


def find_mac_blackhole(pa):
    """macOS ne capture pas le son du système : on lit la carte son virtuelle BlackHole."""
    for i in range(pa.get_device_count()):
        dev = pa.get_device_info_by_index(i)
        if "blackhole" in dev["name"].lower() and int(dev["maxInputChannels"]) > 0:
            return dev
    return None


def to_stereo(data, channels, audioop):
    """Ramène n'importe quel nombre de canaux à de la stéréo 16 bits."""
    if channels == 2:
        return data
    if channels == 1:
        return audioop.tostereo(data, 2, 1, 1)
    src = array.array("h", data)
    left, right = src[0::channels], src[1::channels]
    out = array.array("h", bytes(len(left) * 4))
    out[0::2] = left
    out[1::2] = right
    return out.tobytes()


def encode_audio(block, states, audioop):
    """Paquet AUDIO : [2][valpred, index] x2 puis l'ADPCM du canal gauche puis du droit."""
    header = bytearray([2])
    payload = []
    for c, mono in enumerate((audioop.tomono(block, 2, 1, 0), audioop.tomono(block, 2, 0, 1))):
        valpred, index = states[c] or (0, 0)
        header += struct.pack(">hB", valpred, index)
        enc, states[c] = audioop.lin2adpcm(mono, 2, states[c])
        payload.append(enc)
    return bytes(header) + b"".join(payload)


class StreamSession:
    """Boucle de connexion : se reconnecte tant que le bouton est sur ON."""

    def __init__(self, cfg, log, set_status):
        self.cfg = cfg
        self.log = log
        self.set_status = set_status
        self.stop_event = threading.Event()
        self.sock = None
        self.send_lock = threading.Lock()
        self.in_flight = None   # nombre d'images envoyées mais pas encore affichées
        self.injector = KeyboardInjector()
        self.backend = None
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        # fermeture dans un thread pour ne jamais bloquer l'interface
        threading.Thread(target=self._close, daemon=True).start()

    # -- interne -------------------------------------------------------------

    def _close(self):
        s, self.sock = self.sock, None
        if s:
            try:
                with self.send_lock:
                    s.sendall(make_packet(PKT_PC_BYE))
            except Exception:
                pass
            try:
                s.shutdown(socket.SHUT_RDWR)
            except Exception:
                pass
            try:
                s.close()
            except Exception:
                pass

    def _send(self, data):
        with self.send_lock:
            self.sock.sendall(data)

    def _release_slot(self):
        try:
            self.in_flight.release()
        except ValueError:
            pass

    def _make_backend(self):
        keymap = self.cfg.get("keymap", {})
        if self.cfg.get("controller") == "xbox":
            cpad = self.cfg.get("cpad", "both")
            try:
                xbox = XboxBackend(use_left_stick=cpad in ("stick", "both"))
                self.log("Manette Xbox virtuelle créée (ViGEm).")
                if cpad in ("keys", "both"):
                    cpad_keys = {k: v for k, v in keymap.items() if k.startswith("CPAD_")}
                    keys = ", ".join(cpad_keys.get(k, "?").upper() for k in
                                     ("CPAD_UP", "CPAD_LEFT", "CPAD_DOWN", "CPAD_RIGHT"))
                    self.log(f"Circle Pad → stick gauche + touches {keys}" if cpad == "both"
                             else f"Circle Pad → touches {keys}")
                    return MultiBackend(xbox, KeyboardBackend(self.injector, cpad_keys))
                self.log("Circle Pad → stick gauche de la manette.")
                return xbox
            except Exception as e:
                self.log(f"ERREUR manette virtuelle : {e}")
                self.log("→ Installe le pilote ViGEmBus. Passage en mode clavier.")
        else:
            self.log("Mode manette : clavier.")
        return KeyboardBackend(self.injector, keymap)

    def _run(self):
        ip, port = self.cfg["ip"], int(self.cfg["port"])
        self.backend = self._make_backend()
        while not self.stop_event.is_set():
            self.set_status("connecting", f"Connexion à {ip}:{port}...")
            self.log(f"Connexion à la console {ip}:{port}...")
            try:
                s = socket.create_connection((ip, port), timeout=5)
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                s.settimeout(6)
                self.sock = s
                self._send(make_packet(PKT_PC_HELLO, VERSION.encode()))
                self._session()
            except socket.timeout:
                if not self.stop_event.is_set():
                    self.log("ERREUR : la console ne répond pas (délai dépassé). "
                             "Vérifie l'IP, le port et que l'appli 3DS est lancée.")
            except ConnectionRefusedError:
                self.log("ERREUR : connexion refusée. L'appli 3DS est-elle ouverte ?")
            except OSError as e:
                if not self.stop_event.is_set():
                    self.log(f"ERREUR réseau : {e}")
            except Exception as e:
                if not self.stop_event.is_set():
                    self.log(f"ERREUR : {e}")
            finally:
                self._close()
                if self.backend:
                    self.backend.reset()
                self.injector.release_all()

            if not self.stop_event.is_set():
                self.set_status("error", "Console non connectée — nouvel essai dans 3 s")
                self.stop_event.wait(3)

        self.set_status("off", "Arrêté")
        self.log("Diffusion arrêtée.")

    def _session(self):
        connected = threading.Event()
        reader_error = []
        cpad_seen = [False]

        def reader():
            try:
                while not self.stop_event.is_set():
                    ptype, length = struct.unpack(">BI", recv_exact(self.sock, 5))
                    payload = recv_exact(self.sock, length) if length else b""
                    if ptype == PKT_HELLO:
                        self.log(f"Console connectée ! (appli 3DS v{payload.decode(errors='replace')})")
                        self.set_status("on", f"Connectée à {self.cfg['ip']}")
                        connected.set()
                    elif ptype == PKT_ACK:
                        self._release_slot()
                    elif ptype == PKT_INPUT and len(payload) >= 12:
                        held, cx, cy, sx, sy = struct.unpack(">Ihhhh", payload[:12])
                        if not cpad_seen[0] and (abs(cx) > 40 or abs(cy) > 40):
                            cpad_seen[0] = True
                            self.log(f"Circle Pad reçu de la console (x={cx}, y={cy}).")
                        self.backend.update(held, cx, cy, sx, sy)
                    elif ptype == PKT_KEY and len(payload) >= 3:
                        self.injector.key_event(payload[0] == 1, payload[1],
                                                payload[2:].decode(errors="ignore"))
            except Exception as e:
                reader_error.append(e)
            finally:
                connected.set()
                for _ in range(MAX_IN_FLIGHT):
                    self._release_slot()

        self.in_flight = threading.BoundedSemaphore(MAX_IN_FLIGHT)
        rt = threading.Thread(target=reader, daemon=True)
        rt.start()

        if not connected.wait(6) or reader_error:
            raise reader_error[0] if reader_error else socket.timeout()

        self._stream(rt, reader_error)

    def _stream(self, reader_thread, reader_error):
        cfg = self.cfg
        frame_interval = 1.0 / max(1, int(cfg["fps"]))
        quality = max(10, min(95, int(cfg["quality"])))
        stretch = bool(cfg.get("stretch"))
        full_color = bool(cfg.get("full_color", True))
        frames, bytes_sent, t_stat = 0, 0, time.time()

        audio_stop = threading.Event()
        if cfg.get("audio", True):
            AudioStreamer(self._send_quiet, self.log, audio_stop).start()
        try:
            self._stream_video(cfg, frame_interval, quality, stretch, full_color, reader_thread,
                               reader_error, frames, bytes_sent, t_stat)
        finally:
            audio_stop.set()

    def _send_quiet(self, data):
        """Envoi depuis le thread son : une erreur réseau est gérée par la boucle vidéo."""
        try:
            sock = self.sock
            if sock:
                with self.send_lock:
                    sock.sendall(data)
        except Exception:
            pass

    def _stream_video(self, cfg, frame_interval, quality, stretch, full_color, reader_thread,
                      reader_error, frames, bytes_sent, t_stat):

        with mss.mss() as sct:
            mon_index = int(cfg.get("monitor", 1))
            if mon_index < 1 or mon_index >= len(sct.monitors):
                self.log(f"Écran {mon_index} introuvable, utilisation de l'écran 1.")
                mon_index = 1
            monitor = sct.monitors[mon_index]

            while not self.stop_event.is_set() and reader_thread.is_alive():
                t0 = time.time()

                # Au plus MAX_IN_FLIGHT images en route : la 3DS décode la suivante
                # pendant qu'elle affiche celle-ci, sans accumuler de retard.
                if not self.in_flight.acquire(timeout=5):
                    raise TimeoutError("la console ne répond plus")
                if reader_error:
                    raise reader_error[0]

                shot = sct.grab(monitor)
                img = Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")
                jpeg = encode_frame(img, quality, stretch, full_color)
                self._send(make_packet(PKT_PC_FRAME, jpeg))

                frames += 1
                bytes_sent += len(jpeg)
                now = time.time()
                if now - t_stat >= 5:
                    fps = frames / (now - t_stat)
                    kbps = bytes_sent / 1024 / (now - t_stat)
                    self.set_status("on", f"Connectée à {cfg['ip']} — {fps:.1f} i/s, {kbps:.0f} Ko/s")
                    frames, bytes_sent, t_stat = 0, 0, now

                dt = time.time() - t0
                if dt < frame_interval:
                    self.stop_event.wait(frame_interval - dt)

        if reader_error and not self.stop_event.is_set():
            raise reader_error[0]


def encode_frame(img, quality, stretch, full_color=True):
    # Lanczos : réduction nette (texte lisible) ; reducing_gap garde le calcul rapide
    if stretch:
        out = img.resize((FRAME_W, FRAME_H), Image.LANCZOS, reducing_gap=3.0)
    else:
        img.thumbnail((FRAME_W, FRAME_H), Image.LANCZOS, reducing_gap=3.0)
        out = Image.new("RGB", (FRAME_W, FRAME_H))
        out.paste(img, ((FRAME_W - img.width) // 2, (FRAME_H - img.height) // 2))
    buf = BytesIO()
    out.save(buf, "JPEG", quality=quality, subsampling=0 if full_color else 2)
    return buf.getvalue()


# ---------------------------------------------------------------------------
# Mises à jour (GitHub Releases)
# ---------------------------------------------------------------------------

def parse_version(v):
    v = v.strip().lstrip("vV")
    parts = []
    for p in v.split("."):
        num = "".join(ch for ch in p if ch.isdigit())
        parts.append(int(num) if num else 0)
    return tuple(parts)


def check_for_update():
    """Renvoie (version, url_du_fichier, notes) si une version plus récente existe, sinon None."""
    url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
    req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json",
                                               "User-Agent": "ScreenLinkPC"})
    with urllib.request.urlopen(req, timeout=10) as r:
        data = json.load(r)
    latest = data.get("tag_name", "")
    if parse_version(latest) <= parse_version(VERSION):
        return None
    if not getattr(sys, "frozen", False):
        wanted = UPDATE_ASSET_PY
    elif IS_MAC:
        wanted = UPDATE_ASSET_MAC
    else:
        wanted = UPDATE_ASSET_EXE
    for asset in data.get("assets", []):
        if asset.get("name") == wanted:
            return latest.lstrip("vV"), asset["browser_download_url"], data.get("body") or ""
    return None


def download(url, dest, progress=None):
    req = urllib.request.Request(url, headers={"User-Agent": "ScreenLinkPC"})
    with urllib.request.urlopen(req, timeout=30) as r, open(dest, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        done = 0
        while True:
            chunk = r.read(64 * 1024)
            if not chunk:
                break
            f.write(chunk)
            done += len(chunk)
            if progress and total:
                progress(done * 100 // total)


def install_update(url, log):
    """Télécharge et remplace l'appli, puis la relance."""
    if getattr(sys, "frozen", False) and IS_MAC:
        install_update_mac(url, log)
    elif getattr(sys, "frozen", False):
        exe = sys.executable
        new = exe + ".new"
        download(url, new, lambda p: log(f"Téléchargement : {p}%"))
        old = exe + ".old"
        if os.path.exists(old):
            os.remove(old)
        os.replace(exe, old)      # Windows autorise à renommer un .exe en cours d'exécution
        os.replace(new, exe)
        subprocess.Popen([exe], close_fds=True)
    else:
        script = os.path.abspath(__file__)
        tmp = os.path.join(tempfile.gettempdir(), "screenlink_pc_update.py")
        download(url, tmp, lambda p: log(f"Téléchargement : {p}%"))
        with open(tmp, "rb") as f:
            content = f.read()
        compile(content, script, "exec")   # vérifie que le fichier est valide avant de remplacer
        with open(script, "wb") as f:
            f.write(content)
        subprocess.Popen([sys.executable, script], close_fds=True)


def install_update_mac(url, log):
    """Remplace le paquet .app : un script attend la fermeture de l'appli, échange et relance."""
    app = os.path.abspath(os.path.join(os.path.dirname(sys.executable), "..", ".."))
    if not app.endswith(".app"):
        raise RuntimeError(f"emplacement de l'appli inattendu : {app}")
    if not os.access(os.path.dirname(app), os.W_OK) or "/AppTranslocation/" in app:
        raise RuntimeError("l'appli ne peut pas se remplacer à cet endroit : "
                           "déplace-la dans le dossier Applications puis réessaie")
    work = tempfile.mkdtemp(prefix="screenlink_update_")
    zip_path = os.path.join(work, "update.zip")
    download(url, zip_path, lambda p: log(f"Téléchargement : {p}%"))
    out = os.path.join(work, "app")
    subprocess.run(["ditto", "-x", "-k", zip_path, out], check=True)
    apps = [d for d in os.listdir(out) if d.endswith(".app")]
    if not apps:
        raise RuntimeError("archive de mise à jour invalide")
    new_app = os.path.join(out, apps[0])
    q = shlex.quote
    script = (f"while kill -0 {os.getpid()} 2>/dev/null; do sleep 0.3; done; "
              f"rm -rf {q(app)} && mv {q(new_app)} {q(app)} && "
              f"xattr -dr com.apple.quarantine {q(app)}; open {q(app)}; rm -rf {q(work)}")
    subprocess.Popen(["/bin/sh", "-c", script], start_new_session=True, close_fds=True)


# ---------------------------------------------------------------------------
# Intégration Windows : raccourci Bureau et icônes
# ---------------------------------------------------------------------------

def create_desktop_shortcut():
    """Crée « 3DS Screen Link » sur le Bureau, avec le logo. Renvoie le chemin du raccourci."""
    frozen = getattr(sys, "frozen", False)
    if frozen:
        exe, args, work, icon = sys.executable, "", os.path.dirname(sys.executable), sys.executable
    else:                                        # depuis les sources : lance le script avec pythonw
        pyw = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
        exe = pyw if os.path.exists(pyw) else sys.executable
        args, work, icon = '"' + os.path.abspath(__file__) + '"', app_dir(), resource("icon.ico")

    def ps(v):                                   # chaîne PowerShell entre apostrophes
        return "'" + v.replace("'", "''") + "'"

    script = (
        "$d=[Environment]::GetFolderPath('Desktop');"
        "$p=Join-Path $d '3DS Screen Link.lnk';"
        "$s=(New-Object -ComObject WScript.Shell).CreateShortcut($p);"
        f"$s.TargetPath={ps(exe)};"
        f"$s.Arguments={ps(args)};"
        f"$s.WorkingDirectory={ps(work)};"
        f"$s.IconLocation={ps(icon + ',0')};"
        "$s.Description='3DS Screen Link - heivoli-network.fr';"
        "$s.Save();$p"
    )
    out = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                         capture_output=True, text=True, timeout=20,
                         creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if out.returncode != 0:
        raise RuntimeError((out.stderr or out.stdout).strip()[:200] or "PowerShell a échoué")
    return out.stdout.strip()


def refresh_windows_icons():
    """Demande à l'Explorateur de recharger les icônes (après une mise à jour du .exe)."""
    try:
        import ctypes
        SHCNE_ASSOCCHANGED, SHCNF_IDLIST = 0x08000000, 0x0000
        ctypes.windll.shell32.SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, None, None)
    except Exception:
        pass


def cleanup_old_exe():
    if getattr(sys, "frozen", False):
        old = sys.executable + ".old"
        for _ in range(10):
            try:
                if os.path.exists(old):
                    os.remove(old)
                return
            except OSError:
                time.sleep(0.5)


# ---------------------------------------------------------------------------
# Interface
# ---------------------------------------------------------------------------

CPAD_LABELS = {
    "both": "Stick gauche + touches ZQSD",
    "stick": "Stick gauche (manette)",
    "keys": "Touches ZQSD (clavier)",
}

STATUS_COLORS = {"off": "#7a7f8c", "connecting": "#e0a01e", "on": "#2fb86a", "error": "#e05252"}


class App:
    def __init__(self, root):
        self.root = root
        self.cfg = load_config()
        self.session = None
        self.update_info = None
        self.events = queue.Queue()

        root.title(f"{APP_NAME} v{VERSION}")
        root.geometry("580x740")
        root.minsize(480, 420)
        root.protocol("WM_DELETE_WINDOW", self.on_close)

        # Icône de la fenêtre et logo heivoli-network
        self.images = {}
        try:
            if IS_WINDOWS:
                # .ico multi-tailles : net dans la barre des tâches et dans Alt+Tab
                root.iconbitmap(default=resource("icon.ico"))
            else:
                self.images["icon"] = tk.PhotoImage(file=resource("icon.png")).subsample(8, 8)
                root.iconphoto(True, self.images["icon"])
        except Exception:
            pass

        footer = tk.Label(root, text=f"{COPYRIGHT}  ·  v{VERSION}", fg="#8a8f99", font=("Segoe UI", 8))
        footer.pack(side="bottom", pady=(0, 6))

        try:
            self.images["logo"] = tk.PhotoImage(file=resource("logo.png"))
            header = tk.Frame(root, bg="white")
            header.pack(fill="x")
            tk.Label(header, image=self.images["logo"], bg="white").pack(side="left", padx=12, pady=8)
            tk.Label(header, text=APP_NAME, bg="white", fg="#222222",
                     font=("Segoe UI", 14, "bold")).pack(side="left", padx=4)
        except Exception:
            pass

        main = ttk.Frame(root, padding=12)
        main.pack(fill="both", expand=True)

        # --- Connexion ---
        conn = ttk.LabelFrame(main, text="Console", padding=10)
        conn.pack(fill="x")

        ttk.Label(conn, text="IP de la 3DS :").grid(row=0, column=0, sticky="w")
        self.ip_var = tk.StringVar(value=self.cfg["ip"])
        self.ip_entry = ttk.Entry(conn, textvariable=self.ip_var, width=18)
        self.ip_entry.grid(row=0, column=1, sticky="w", padx=(4, 16))

        ttk.Label(conn, text="Port :").grid(row=0, column=2, sticky="w")
        self.port_var = tk.StringVar(value=str(self.cfg["port"]))
        self.port_entry = ttk.Entry(conn, textvariable=self.port_var, width=7)
        self.port_entry.grid(row=0, column=3, sticky="w", padx=4)

        self.toggle_btn = tk.Button(conn, text="OFF", width=8, font=("Segoe UI", 11, "bold"),
                                    bg=STATUS_COLORS["off"], fg="white", relief="flat",
                                    activebackground="#555", command=self.toggle)
        self.toggle_btn.grid(row=0, column=4, padx=(16, 0))

        # --- Options ---
        opts = ttk.LabelFrame(main, text="Options", padding=10)
        opts.pack(fill="x", pady=(10, 0))

        ttk.Label(opts, text="Images/s :").grid(row=0, column=0, sticky="w")
        self.fps_var = tk.IntVar(value=self.cfg["fps"])
        ttk.Spinbox(opts, from_=1, to=30, textvariable=self.fps_var, width=5).grid(row=0, column=1, padx=4)

        ttk.Label(opts, text="Qualité :").grid(row=0, column=2, sticky="w", padx=(12, 0))
        self.quality_var = tk.IntVar(value=self.cfg["quality"])
        ttk.Spinbox(opts, from_=10, to=95, increment=5, textvariable=self.quality_var, width=5).grid(row=0, column=3, padx=4)

        ttk.Label(opts, text="Écran n° :").grid(row=0, column=4, sticky="w", padx=(12, 0))
        self.monitor_var = tk.IntVar(value=self.cfg["monitor"])
        ttk.Spinbox(opts, from_=1, to=8, textvariable=self.monitor_var, width=4).grid(row=0, column=5, padx=4)

        ttk.Label(opts, text="Manette :").grid(row=1, column=0, sticky="w", pady=(8, 0))
        # Pas de manette virtuelle sur Mac : les boutons deviennent des touches
        ctrl_values = ["Xbox (ViGEm)", "Clavier"] if IS_WINDOWS else ["Clavier"]
        self.ctrl_var = tk.StringVar(value="Xbox (ViGEm)" if IS_WINDOWS and self.cfg["controller"] == "xbox"
                                     else "Clavier")
        ttk.Combobox(opts, textvariable=self.ctrl_var, values=ctrl_values,
                     state="readonly", width=13).grid(row=1, column=1, columnspan=2, sticky="w", padx=4, pady=(8, 0))

        self.stretch_var = tk.BooleanVar(value=self.cfg["stretch"])
        ttk.Checkbutton(opts, text="Étirer l'image (sans bandes noires)",
                        variable=self.stretch_var).grid(row=1, column=3, columnspan=3, sticky="w", pady=(8, 0))

        ttk.Label(opts, text="Circle Pad :").grid(row=2, column=0, sticky="w", pady=(8, 0))
        self.cpad_var = tk.StringVar(value=CPAD_LABELS.get(self.cfg.get("cpad"), CPAD_LABELS["both"]))
        ttk.Combobox(opts, textvariable=self.cpad_var, values=list(CPAD_LABELS.values()),
                     state="readonly", width=26).grid(row=2, column=1, columnspan=4, sticky="w", padx=4, pady=(8, 0))

        self.full_color_var = tk.BooleanVar(value=self.cfg.get("full_color", True))
        ttk.Checkbutton(opts, text="Couleurs pleine résolution (texte plus net, un peu plus lent)",
                        variable=self.full_color_var).grid(row=3, column=0, columnspan=6, sticky="w", pady=(8, 0))

        self.audio_var = tk.BooleanVar(value=self.cfg.get("audio", True))
        ttk.Checkbutton(opts, text="Envoyer le son du PC à la 3DS" + ("" if IS_WINDOWS else " (nécessite BlackHole)"),
                        variable=self.audio_var).grid(row=4, column=0, columnspan=6, sticky="w", pady=(8, 0))

        self.updates_var = tk.BooleanVar(value=self.cfg.get("check_updates", True))
        ttk.Checkbutton(opts, text="Rechercher les mises à jour au démarrage (contacte GitHub)",
                        variable=self.updates_var, command=self.on_updates_toggle
                        ).grid(row=5, column=0, columnspan=6, sticky="w", pady=(8, 0))

        # --- Statut + mise à jour ---
        bar = ttk.Frame(main)
        bar.pack(fill="x", pady=(10, 4))
        self.dot = tk.Canvas(bar, width=14, height=14, highlightthickness=0)
        self.dot.pack(side="left")
        self.dot_id = self.dot.create_oval(2, 2, 12, 12, fill=STATUS_COLORS["off"], outline="")
        self.status_lbl = ttk.Label(bar, text="Console non connectée")
        self.status_lbl.pack(side="left", padx=6)

        self.update_btn = tk.Button(bar, text="Mise à jour disponible", bg="#4c8bf5", fg="white",
                                    relief="flat", command=self.do_update)
        if IS_WINDOWS:
            ttk.Button(bar, text="Raccourci sur le Bureau", command=self.make_shortcut).pack(side="right", padx=(6, 0))
        # caché tant qu'aucune mise à jour n'est trouvée

        # --- Console ---
        ttk.Label(main, text="Journal :").pack(anchor="w")
        self.console = scrolledtext.ScrolledText(main, height=12, state="disabled",
                                                 bg="#15171c", fg="#e6e6e6",
                                                 insertbackground="white", font=("Consolas", 9))
        self.console.pack(fill="both", expand=True)
        self.console.tag_config("err", foreground="#ff7070")
        self.console.tag_config("ok", foreground="#5fd38d")

        self.log(f"{APP_NAME} v{VERSION} prêt.")
        self.log("Lance l'appli sur la 3DS, entre l'IP et le port affichés, puis clique sur OFF pour passer à ON.")
        if IS_MAC:
            self.log("Mac : autorise l'appli dans Réglages Système → Confidentialité et sécurité → "
                     "« Enregistrement de l'écran » (image) et « Accessibilité » (clavier et boutons).")

        if IS_WINDOWS:
            self.windows_first_run()

        self.root.after(100, self.process_events)
        threading.Thread(target=self.update_loop, daemon=True).start()
        threading.Thread(target=cleanup_old_exe, daemon=True).start()

    # -- thread-safe : les threads poussent des événements, l'UI les traite ----

    def log(self, msg):
        self.events.put(("log", msg))

    def set_status(self, state, text):
        self.events.put(("status", (state, text)))

    def process_events(self):
        try:
            while True:
                kind, data = self.events.get_nowait()
                if kind == "log":
                    self._append_log(data)
                elif kind == "status":
                    state, text = data
                    self.dot.itemconfig(self.dot_id, fill=STATUS_COLORS.get(state, "#777"))
                    self.status_lbl.config(text=text)
                elif kind == "update":
                    self.update_info = data
                    self.update_btn.config(text=f"⬇ Mettre à jour (v{data[0]})")
                    self.update_btn.pack(side="right")
                elif kind == "exit":
                    self.root.destroy()
                    return
        except queue.Empty:
            pass
        self.root.after(100, self.process_events)

    def _append_log(self, msg):
        tag = ()
        if "ERREUR" in msg:
            tag = ("err",)
        elif "connectée !" in msg or "Mise à jour installée" in msg:
            tag = ("ok",)
        self.console.config(state="normal")
        self.console.insert("end", time.strftime("[%H:%M:%S] ") + msg + "\n", tag)
        self.console.see("end")
        self.console.config(state="disabled")

    # -- actions --------------------------------------------------------------

    def read_form(self):
        ip = self.ip_var.get().strip()
        try:
            socket.inet_aton(ip)
            if ip.count(".") != 3:
                raise OSError
        except OSError:
            raise ValueError("Adresse IP invalide (exemple : 192.168.1.50)")
        try:
            port = int(self.port_var.get())
            if not 1 <= port <= 65535:
                raise ValueError
        except ValueError:
            raise ValueError("Port invalide (1 à 65535)")
        self.cfg.update({
            "ip": ip, "port": port,
            "fps": int(self.fps_var.get()), "quality": int(self.quality_var.get()),
            "monitor": int(self.monitor_var.get()), "stretch": bool(self.stretch_var.get()),
            "full_color": bool(self.full_color_var.get()),
            "audio": bool(self.audio_var.get()),
            "check_updates": bool(self.updates_var.get()),
            "controller": "xbox" if self.ctrl_var.get().startswith("Xbox") else "keyboard",
            "cpad": next((k for k, v in CPAD_LABELS.items() if v == self.cpad_var.get()), "both"),
        })
        save_config(self.cfg)

    def toggle(self):
        if self.session is None:
            try:
                self.read_form()
            except ValueError as e:
                self.log(f"ERREUR : {e}")
                return
            self.session = StreamSession(dict(self.cfg), self.log, self.set_status)
            self.session.start()
            self.toggle_btn.config(text="ON", bg=STATUS_COLORS["on"])
            self.ip_entry.config(state="disabled")
            self.port_entry.config(state="disabled")
        else:
            self.session.stop()
            self.session = None
            self.toggle_btn.config(text="OFF", bg=STATUS_COLORS["off"])
            self.ip_entry.config(state="normal")
            self.port_entry.config(state="normal")
            self.set_status("off", "Console non connectée")

    def on_updates_toggle(self):
        self.cfg["check_updates"] = bool(self.updates_var.get())
        save_config(self.cfg)
        if self.cfg["check_updates"]:
            threading.Thread(target=self.update_loop, daemon=True).start()
        else:
            self.log("Recherche des mises à jour désactivée.")

    def windows_first_run(self):
        """Premier lancement : raccourci Bureau. Après une mise à jour : icônes rafraîchies."""
        if self.cfg.get("last_version") != VERSION:
            refresh_windows_icons()
            self.cfg["last_version"] = VERSION
            save_config(self.cfg)
        if getattr(sys, "frozen", False) and not self.cfg.get("desktop_shortcut_done"):
            self.cfg["desktop_shortcut_done"] = True
            save_config(self.cfg)
            self.make_shortcut()

    def make_shortcut(self):
        def work():
            try:
                path = create_desktop_shortcut()
                refresh_windows_icons()
                self.log(f"Raccourci « 3DS Screen Link » créé sur le Bureau ({path}).")
            except Exception as e:
                self.log(f"ERREUR raccourci Bureau : {e}")
        threading.Thread(target=work, daemon=True).start()

    def update_loop(self):
        if "TON_PSEUDO" in GITHUB_REPO:
            self.log("Mises à jour désactivées : GITHUB_REPO n'est pas configuré.")
            return
        if not self.cfg.get("check_updates", True):
            self.log("Recherche des mises à jour désactivée (case à cocher dans les options).")
            return
        while self.cfg.get("check_updates", True):
            try:
                info = check_for_update()
                if info:
                    self.log(f"Nouvelle version disponible : v{info[0]}")
                    self.events.put(("update", info))
                    return
            except Exception as e:
                self.log(f"Vérification des mises à jour impossible : {e}")
            time.sleep(3600)

    def do_update(self):
        if not self.update_info:
            return
        version, url, notes = self.update_info
        text = f"Installer la version {version} ?\nL'application va redémarrer."
        if notes:
            text += "\n\nNouveautés :\n" + notes[:600]
        if not messagebox.askyesno("Mise à jour", text):
            return
        self.update_btn.config(state="disabled", text="Téléchargement...")
        if self.session:
            self.toggle()

        def worker():
            try:
                install_update(url, self.log)
                self.log("Mise à jour installée, redémarrage...")
                time.sleep(0.5)
                self.events.put(("exit", None))
            except Exception as e:
                self.log(f"ERREUR pendant la mise à jour : {e}")

        threading.Thread(target=worker, daemon=True).start()

    def on_close(self):
        if self.session:
            self.session.stop()
        self.root.destroy()


def main():
    root = tk.Tk()
    try:
        ttk.Style().theme_use("vista")
    except tk.TclError:
        pass
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
