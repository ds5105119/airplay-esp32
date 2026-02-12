#!/usr/bin/env python3
import argparse
import json
import os
import random
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import urlparse


def clamp(n: int, lo: int, hi: int) -> int:
    return max(lo, min(hi, n))


class ControlPanelState:
    def __init__(self) -> None:
        self.device_name = "O1"
        self.firmware_version = "0.1.1"
        self.wifi_connected = False
        self.ip = "192.168.4.1"
        self.mac = "AC:A7:04:26:3C:80"
        # Simulate NVS-saved WiFi credentials (multiple for UI testing).
        # Firmware stores a single set, but this helps exercise the UI list.
        self.saved_networks = [
            {"ssid": "IIH-1", "password": "saved-password"},
            {"ssid": "iptime-5G", "password": "saved-password"},
            {"ssid": "CoffeeWiFi", "password": ""},
        ]
        # "Connected" credentials (used when wifi_connected=True).
        self.ssid = self.saved_networks[0]["ssid"]
        self.password = self.saved_networks[0]["password"]
        self.scan_delay_ms = 1600
        self.info_delay_ms = 350
        self.settings_ap_enabled = True

    def system_info(self) -> dict:
        # Mimic changing free heap a little, like a real device.
        free_heap = 380_000 + random.randint(-8_000, 8_000)
        free_heap = clamp(free_heap, 120_000, 480_000)
        rssi = -55 + random.randint(-10, 10)
        return {
            "wifi_connected": self.wifi_connected,
            "ip": self.ip if self.wifi_connected else "192.168.4.1",
            "mac": self.mac,
            "device_name": self.device_name,
            "firmware_version": self.firmware_version or "Unknown",
            "free_heap": free_heap,
            "ssid": self.ssid if self.wifi_connected else "",
            "rssi": rssi if self.wifi_connected else None,
            "secure": bool(self.password) if self.wifi_connected else False,
        }


def make_handler(state: ControlPanelState, html_path: Path):
    wifi_networks = [
        {"ssid": "IIH-1", "rssi": -58, "channel": 1, "secure": True},
        {"ssid": "O1", "rssi": -35, "channel": 6, "secure": True},
        {"ssid": "CoffeeWiFi", "rssi": -80, "channel": 11, "secure": False},
        {"ssid": "ABCD", "rssi": -85, "channel": 9, "secure": False},
        {"ssid": "KT_GiGA_2G", "rssi": -52, "channel": 3, "secure": True},
        {"ssid": "KT_GiGA_5G", "rssi": -61, "channel": 36, "secure": True},
        {"ssid": "U+Net_2G", "rssi": -72, "channel": 11, "secure": True},
        {"ssid": "U+Net_5G", "rssi": -69, "channel": 149, "secure": True},
        {"ssid": "iptime-2G", "rssi": -66, "channel": 6, "secure": True},
        {"ssid": "iptime-5G", "rssi": -63, "channel": 44, "secure": True},
        {"ssid": "Guest", "rssi": -78, "channel": 1, "secure": False},
        {"ssid": "Public_WiFi", "rssi": -82, "channel": 4, "secure": False},
    ]

    class Handler(BaseHTTPRequestHandler):
        server_version = "MockControlPanel/1.0"

        def _send(self, status: int, content_type: str, body: bytes) -> None:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _send_json(self, obj: dict, status: int = 200) -> None:
            self._send(status, "application/json; charset=utf-8",
                       json.dumps(obj).encode("utf-8"))

        def _read_body(self) -> bytes:
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                length = 0
            if length <= 0:
                return b""
            return self.rfile.read(length)

        def log_message(self, format: str, *args) -> None:
            # Quieter logs, but keep useful info.
            msg = format % args
            print(f"{self.address_string()} - {self.command} {self.path} - {msg}")

        def do_GET(self) -> None:
            path = urlparse(self.path).path

            if path in ("/", "/index.html"):
                body = html_path.read_bytes()
                self._send(200, "text/html; charset=utf-8", body)
                return

            if path == "/api/system/info":
                if state.info_delay_ms > 0:
                    time.sleep(state.info_delay_ms / 1000.0)
                self._send_json({"success": True, "info": state.system_info()})
                return

            if path == "/api/wifi/scan":
                if state.scan_delay_ms > 0:
                    time.sleep(state.scan_delay_ms / 1000.0)
                self._send_json({"success": True, "networks": wifi_networks})
                return

            if path == "/api/wifi/saved":
                networks = []
                for n in state.saved_networks:
                    ssid = str(n.get("ssid", "")).strip()
                    if not ssid:
                        continue
                    networks.append({"ssid": ssid, "secure": bool(n.get("password", ""))})
                self._send_json({"success": True, "networks": networks})
                return

            self._send_json({"success": False, "error": "Not found"}, status=404)

        def do_POST(self) -> None:
            path = urlparse(self.path).path
            body = self._read_body()

            if path == "/api/wifi/config":
                try:
                    req = json.loads(body.decode("utf-8") or "{}")
                except Exception:
                    self._send_json({"success": False, "error": "Invalid JSON"}, status=400)
                    return

                ssid = str(req.get("ssid", "")).strip()
                password = str(req.get("password", ""))
                if not ssid:
                    self._send_json({"success": False, "error": "Missing SSID"}, status=400)
                    return

                state.ssid = ssid
                state.password = password
                state.wifi_connected = True
                state.ip = "192.168.219.102"
                # Save/update in "NVS".
                updated = False
                for n in state.saved_networks:
                    if n.get("ssid") == ssid:
                        n["password"] = password
                        updated = True
                        break
                if not updated:
                    state.saved_networks.insert(0, {"ssid": ssid, "password": password})
                self._send_json({"success": True})
                return

            if path == "/api/wifi/disconnect":
                # Simulate disconnecting from the current AP.
                state.wifi_connected = False
                state.ip = "192.168.4.1"
                self._send_json({"success": True})
                return

            if path == "/api/wifi/forget":
                # Forget saved credentials (NVS erase equivalent).
                state.wifi_connected = False
                state.ip = "192.168.4.1"
                state.ssid = ""
                state.password = ""
                state.saved_networks = []
                self._send_json({"success": True})
                return

            if path == "/api/settings/close":
                state.settings_ap_enabled = False
                self._send_json({"success": True})
                return

            if path == "/api/device/name":
                try:
                    req = json.loads(body.decode("utf-8") or "{}")
                except Exception:
                    self._send_json({"success": False, "error": "Invalid JSON"}, status=400)
                    return

                name = str(req.get("name", "")).strip()
                if not name:
                    self._send_json({"success": False, "error": "Missing name"}, status=400)
                    return

                state.device_name = name
                self._send_json({"success": True})
                return

            if path == "/api/ota/update":
                # Accept any payload, respond like the real device (plain text),
                # and simulate a reboot by dropping WiFi state.
                self._send(200, "text/plain; charset=utf-8", b"Firmware update complete, rebooting now!\n")
                state.wifi_connected = False
                state.ip = "192.168.4.1"
                return

            if path == "/api/system/restart":
                # Simulate a restart: drop WiFi briefly and reset IP.
                self._send_json({"success": True})
                state.wifi_connected = False
                state.ip = "192.168.4.1"
                state.ssid = ""
                state.password = ""
                time.sleep(0.2)
                return

            self._send_json({"success": False, "error": "Not found"}, status=404)

    return Handler


def main() -> int:
    parser = argparse.ArgumentParser(description="Mock server for AirPlay Receiver control panel HTML.")
    parser.add_argument(
        "--html",
        default=str(Path("main/network/main.html")),
        help="Path to main.html (default: main/network/main.html)",
    )
    parser.add_argument("--host", default="127.0.0.1", help="Bind host (use 0.0.0.0 for LAN access)")
    parser.add_argument("--port", type=int, default=8000, help="Bind port")
    parser.add_argument("--scan-delay-ms", type=int, default=1600, help="Delay for /api/wifi/scan (ms)")
    parser.add_argument("--info-delay-ms", type=int, default=350, help="Delay for /api/system/info (ms)")
    args = parser.parse_args()

    html_path = Path(args.html)
    if not html_path.exists():
        raise SystemExit(f"HTML not found: {html_path}")

    state = ControlPanelState()
    state.scan_delay_ms = max(0, int(args.scan_delay_ms))
    state.info_delay_ms = max(0, int(args.info_delay_ms))
    handler = make_handler(state, html_path)
    httpd = HTTPServer((args.host, args.port), handler)

    print(f"Serving {html_path} on http://{args.host}:{args.port}/")
    print(
        "Mock APIs: /api/system/info, /api/wifi/scan, /api/wifi/saved, /api/wifi/config, /api/wifi/disconnect, /api/wifi/forget, "
        "/api/device/name, /api/ota/update, /api/system/restart"
    )
    print("Tip (iPhone): run with --host 0.0.0.0 and open http://<your-mac-ip>:<port>/")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
