#!/usr/bin/env python3
"""
FOTA Fleet Simulator
=====================
Simulates N virtual ESP32 devices talking to your real FOTA backend, so you
can test dashboard behavior (many devices online, staggered updates,
rollback, targeting, scheduled deployments, etc.) without needing N physical
boards.

Each simulated device speaks the EXACT SAME HTTP contract as the real
firmware:
  - POST /api/esp/data           (heartbeat / check-in, same as device_report.cpp)
  - GET  /api/firmware/latest    (metadata poll, same as ota_manager.cpp's fetchMetadata())
  - GET  <download url>          (firmware binary download, same as downloadFlashAndVerify())

It also verifies the SHA-256 hash exactly like the real device does, and
"installs" (updates its in-memory version) only after that check passes -
so a bug in your backend's hash/version logic will show up here too, not
just on real hardware.

INSTALL
    pip install requests

USAGE
    # 20 simulated devices, default intervals, run until Ctrl+C
    python fota_simulator.py --base-url https://fotaproject-production.up.railway.app --count 20

    # 100 devices, faster polling for quicker test cycles, run for 10 minutes
    python fota_simulator.py --base-url https://your-app.up.railway.app 
        --count 100 --poll-interval 5 --report-interval 10 --duration 600

    # Simulate some devices randomly going offline (tests "Offline" status)
    python fota_simulator.py --base-url https://your-app.up.railway.app 
        --count 30 --dropout-chance 0.05
"""

import argparse
import hashlib
import random
import signal
import sys
import threading
import time
from datetime import datetime

import requests

# ----------------------------------------------------------------------------
# One simulated device
# ----------------------------------------------------------------------------

class SimulatedDevice:
    def __init__(self, device_id, base_url, hw_id, poll_interval, report_interval,
                 dropout_chance, flash_delay_range, verbose):
        self.device_id = device_id
        self.base_url = base_url.rstrip("/")
        self.hw_id = hw_id
        self.poll_interval = poll_interval
        self.report_interval = report_interval
        self.dropout_chance = dropout_chance
        self.flash_delay_range = flash_delay_range
        self.verbose = verbose

        self.current_version = "0.0.0"
        self._stop = threading.Event()
        self._offline = False  # simulated "temporarily unreachable" state

    def log(self, msg):
        if self.verbose:
            ts = datetime.now().strftime("%H:%M:%S")
            print(f"[{ts}] [{self.device_id}] {msg}")

    def stop(self):
        self._stop.set()

    def report(self):
        """Mirrors device_report.cpp's sendReport()."""
        try:
            r = requests.post(
                f"{self.base_url}/api/esp/data",
                json={"deviceId": self.device_id, "firmware": self.current_version},
                timeout=10,
            )
            self.log(f"report -> HTTP {r.status_code}")
        except requests.RequestException as e:
            self.log(f"report FAILED: {e}")

    def check_for_update(self):
        """Mirrors ota_manager.cpp's fetchMetadata() + downloadFlashAndVerify()."""
        try:
            r = requests.get(
                f"{self.base_url}/api/firmware/latest",
                params={"hw": self.hw_id, "deviceId": self.device_id},
                timeout=10,
            )
        except requests.RequestException as e:
            self.log(f"metadata fetch FAILED: {e}")
            return

        if r.status_code == 404:
            self.log("metadata fetch HTTP 404 (no firmware for active/target version)")
            return
        if r.status_code != 200:
            self.log(f"metadata fetch HTTP {r.status_code}")
            return

        meta = r.json()
        server_version = meta.get("version", "")

        if server_version == self.current_version:
            self.log(f"up to date (v{self.current_version})")
            return

        hw_list = meta.get("hw_compatibility", [])
        if self.hw_id not in hw_list:
            self.log(f"server has v{server_version} but it's not compatible with {self.hw_id}")
            return

        self.log(f"update found: {self.current_version} -> {server_version}, downloading...")

        try:
            fw = requests.get(meta["url"], timeout=30)
        except requests.RequestException as e:
            self.log(f"firmware download FAILED: {e}")
            return

        if fw.status_code != 200:
            self.log(f"firmware download HTTP {fw.status_code}")
            return

        computed_hash = hashlib.sha256(fw.content).hexdigest()
        expected_hash = meta.get("sha256", "")
        if computed_hash.lower() != expected_hash.lower():
            self.log(f"SHA-256 MISMATCH - expected {expected_hash}, got {computed_hash}")
            return

        # Simulate flash-write + reboot time (real devices take a few seconds)
        delay = random.uniform(*self.flash_delay_range)
        time.sleep(delay)

        self.current_version = server_version
        self.log(f"verified + flashed successfully, now running v{self.current_version}")

    def run(self):
        # Stagger initial start so 100 devices don't all hit the server in
        # the exact same instant (mirrors real devices booting at slightly
        # different times).
        time.sleep(random.uniform(0, min(self.report_interval, self.poll_interval)))

        last_report = 0.0
        last_poll = 0.0

        while not self._stop.is_set():
            now = time.time()

            # Randomly simulate a device going temporarily offline/unreachable
            if self.dropout_chance and random.random() < self.dropout_chance:
                self._offline = not self._offline
                self.log("simulated offline" if self._offline else "back online")

            if not self._offline:
                if now - last_report >= self.report_interval:
                    self.report()
                    last_report = now

                if now - last_poll >= self.poll_interval:
                    self.check_for_update()
                    last_poll = now

            time.sleep(1)


# ----------------------------------------------------------------------------
# Fleet orchestration
# ----------------------------------------------------------------------------

def make_device_id(index):
    # Fake but MAC-like IDs, matching the 12-hex-char format the real
    # firmware generates from its efuse MAC (see buildDeviceId() in main.cpp)
    return f"SIM{index:04d}{random.randint(0, 0xFFFFFF):06X}"


def main():
    parser = argparse.ArgumentParser(description="Simulate a fleet of ESP32 FOTA devices")
    parser.add_argument("--base-url", required=True, help="e.g. https://fotaproject-production.up.railway.app")
    parser.add_argument("--count", type=int, default=10, help="number of simulated devices")
    parser.add_argument("--hw-id", default="esp32-devkit-v1", help="must match HW_ID in config.h")
    parser.add_argument("--poll-interval", type=float, default=20, help="seconds, matches POLL_INTERVAL_MS")
    parser.add_argument("--report-interval", type=float, default=30, help="seconds, matches REPORT_INTERVAL_MS")
    parser.add_argument("--dropout-chance", type=float, default=0.0, help="0.0-1.0 chance per second of toggling offline")
    parser.add_argument("--flash-delay-min", type=float, default=1.0, help="min simulated flash/reboot time (s)")
    parser.add_argument("--flash-delay-max", type=float, default=4.0, help="max simulated flash/reboot time (s)")
    parser.add_argument("--duration", type=float, default=None, help="run for N seconds, then stop (default: run until Ctrl+C)")
    parser.add_argument("--quiet", action="store_true", help="suppress per-device log lines")
    args = parser.parse_args()

    devices = [
        SimulatedDevice(
            device_id=make_device_id(i),
            base_url=args.base_url,
            hw_id=args.hw_id,
            poll_interval=args.poll_interval,
            report_interval=args.report_interval,
            dropout_chance=args.dropout_chance,
            flash_delay_range=(args.flash_delay_min, args.flash_delay_max),
            verbose=not args.quiet,
        )
        for i in range(args.count)
    ]

    print(f"Starting {args.count} simulated devices against {args.base_url}")
    print(f"poll_interval={args.poll_interval}s report_interval={args.report_interval}s")
    print("Press Ctrl+C to stop.\n")

    threads = [threading.Thread(target=d.run, daemon=True) for d in devices]
    for t in threads:
        t.start()

    def shutdown(*_):
        print("\nStopping all simulated devices...")
        for d in devices:
            d.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, shutdown)

    if args.duration:
        time.sleep(args.duration)
        shutdown()
    else:
        while True:
            time.sleep(1)


if __name__ == "__main__":
    main()
