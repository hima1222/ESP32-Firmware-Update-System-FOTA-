# ESP32 Firmware Update System (FOTA)

## What is this?

This is a system that lets a fleet of ESP32 devices (small computer chips
used in electronics projects) receive software updates automatically over
WiFi — without anyone having to physically plug a cable into each device.

Think of it like how your phone updates its apps in the background. You
don't plug your phone into a computer every time an app needs updating — it
just checks in, sees something new is available, downloads it, and installs
it. This project does the same thing, but for ESP32 hardware devices instead
of phone apps.

It's designed to safely manage around 100 devices at once from a single web
dashboard.

---

## Why does this matter?

Imagine you've built 100 small devices and shipped them out — maybe they're
monitoring temperature in 100 different warehouses, or controlling 100
smart mailboxes. A few weeks later, you discover a bug, or want to add a
new feature.

Without a system like this, someone would have to physically visit every
single device with a laptop and a USB cable to install the fix. That's slow,
expensive, and sometimes impossible if devices are in hard-to-reach places.

With this system, you fix the bug once, upload the new version to a web
dashboard, and every device in the field picks it up on its own — usually
within minutes.

---

## How does a device know when to check for updates?

Every device is designed to check in **four different ways**, so it never
misses an update for long:

1. **Once a day, at midnight** — a routine daily check, in case everything
   else fails.
2. **Every 3 hours** — a more frequent safety-net check throughout the day.
3. **Instantly, when told to** — the web dashboard can send a message that
   reaches devices within seconds, so an admin can push an urgent update
   right away instead of waiting.
4. **By pressing a physical button** — useful when someone is standing right
   next to a device and wants to trigger a check manually.

---

## What actually happens during an update?

When a device decides it's time to check, here's the journey, step by step:

1. **"What's the latest version?"** — the device asks the web server a
   simple question: *"what version should I be running?"* The server replies
   with some basic information: the version number, a security fingerprint,
   and a download link. This step is quick and lightweight — no large file
   is downloaded yet.

2. **"Do I actually need this?"** — the device compares that version number
   to its own current version. If it's already up to date, it stops here and
   goes back to sleep until the next check. No wasted downloads.

3. **Downloading safely** — if there's genuinely something newer, the device
   downloads the new software over an encrypted connection (the same kind
   of security your bank's website uses), so nobody can intercept or tamper
   with it along the way.

4. **Double-checking it's not corrupted** — before trusting the download,
   the device runs a mathematical check (like a digital fingerprint) to
   make sure the file that arrived is exactly the file that was meant to be
   sent — not something that got scrambled or tampered with in transit.

5. **Installing it safely** — this is the clever part. The device doesn't
   overwrite its current, working software. Instead, it writes the new
   version into a *separate, empty storage slot* it keeps in reserve — a bit
   like renovating a spare room instead of demolishing the room you're
   currently living in.

6. **Restarting into the new version** — once the new version is fully
   written and verified, the device restarts and switches over to that spare
   slot.

7. **A quick health check** — right after restarting, the device runs a
   short self-test to make sure everything still works properly with the
   new software.

8. **Confirm or undo** — if the self-test passes, the device officially
   "confirms" the new version and carries on normally. **If the self-test
   fails, the device automatically switches itself back to the previous,
   known-good version** — no one needs to intervene.

---

## What happens if an update goes wrong?

This is the most important safety feature of the whole system, so it's
worth explaining clearly.

Every device always keeps **two full copies** of its software: the one it's
currently running, and the previous one, kept safely in reserve. This is
similar to how some game consoles let you keep an old version of a game
installed in case a new patch breaks something.

There are two ways a device can recover from a bad update:

- **Automatically** — if the new software fails its own health check right
  after installing, the device reverts itself back to the previous version
  without anyone touching it. This happens within seconds of the restart.

- **Manually, on-site** — if a device is physically accessible and behaving
  strangely, someone can simply press its reset button twice, quickly, one
  after another. The device recognizes this as a deliberate signal and
  switches back to its previous known-good version — even if that
  version had already been running fine for weeks. This is a safety net
  for situations the automatic check might not catch.

Because of this, a bad update can never permanently "brick" a device in the
field — there's always a way back.

---

## What does the web dashboard let you do?

- **See which devices are online**, and what version each one is currently
  running.
- **Upload a new version** of the software.
- **Choose which version is "active"** — this is the version devices will
  install the next time they check in.
- **Target a specific device** with a specific version — useful for testing
  an update on one device before rolling it out to all 100.
- **Trigger an emergency rollback** for every device at once, in case a
  released version turns out to be faulty after all.
- **Schedule deployments** — for example, "roll this out at 2 AM tonight,"
  or "roll it out automatically once at least 5 devices are online."

---

## Is this secure?

Yes, by design:

- All downloads happen over an encrypted connection, so the update can't be
  read or modified by anyone intercepting the network traffic.
- Every downloaded file is checked against a security fingerprint before
  it's trusted, so a corrupted or tampered file will be rejected rather than
  installed.
- Devices only accept updates meant for their specific hardware type, so a
  device never accidentally installs software built for different hardware.

---

## Who is this for?

Anyone deploying multiple ESP32-based devices who wants to update them
remotely instead of physically visiting each one — hobbyist projects,
university coursework, small IoT products, or early-stage prototypes for a
fleet of connected devices.

---

## A quick note on scale

This system is built to comfortably manage a fleet of around 100 devices.
Beyond that scale, some of the underlying pieces (like how the web server
keeps track of devices) would benefit from more robust infrastructure — but
for the current scope, everything here is intentionally kept simple and
easy to understand.
