#!/usr/bin/env python3
"""Hikaboshi desktop companion / protocol test tool (BLE central).

Requires: Python 3.9+, bleak
  pip install -r requirements.txt

Examples:
  python hikaboshictl.py scan
  python hikaboshictl.py connect
  python hikaboshictl.py notify "Title" "Body text"
  python hikaboshictl.py battery
  python hikaboshictl.py steps
  python hikaboshictl.py fitness
  python hikaboshictl.py time-sync
  python hikaboshictl.py weather 24.5 1
  python hikaboshictl.py screen on
  python hikaboshictl.py wifi on
  python hikaboshictl.py ota https://example.com/smart_watch.bin
  python hikaboshictl.py watch          # stream battery/steps/distance/calories/control notifies
"""

from __future__ import annotations

import argparse
import asyncio
import struct
import sys
import time
from datetime import datetime, timezone

try:
    from bleak import BleakClient, BleakScanner
    from bleak.backends.device import BLEDevice
except ImportError:
    print("Install bleak:  pip install -r requirements.txt", file=sys.stderr)
    sys.exit(1)

DEVICE_NAME = "Hikaboshi"
SERVICE_UUID = "1d8a503d-e931-369f-164b-6f10596a178d"
NOTIFY_UUID = "2480757d-4f07-9fa5-0f48-e4125a9bdab8"
CONTROL_UUID = "b31cb75e-410c-29ba-0b45-9da7834df66e"
BATTERY_UUID = "12345678-90ab-cdef-1234-567890abcdef"
STEPS_UUID = "fedcba98-7654-3210-fedc-ba9876543210"
DISTANCE_UUID = "a1715cd1-0304-4b5c-b24a-111213141516"
CALORIES_UUID = "b2715cda-0506-4d5e-c35b-212223242526"


async def find_device(timeout: float = 10.0) -> BLEDevice:
    print(f"Scanning for {DEVICE_NAME!r} ({timeout:.0f}s)...")
    # Wake the watch first if connect fails (adv stops after ~5 min idle).
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name or ad.local_name) == DEVICE_NAME,
        timeout=timeout,
    )
    if not dev:
        # Fallback: any device advertising our service UUID
        devices = await BleakScanner.discover(timeout=timeout, return_adv=True)
        for d, ad in devices.values():
            if SERVICE_UUID.lower() in [u.lower() for u in ad.service_uuids]:
                return d
        raise RuntimeError(f"{DEVICE_NAME} not found — wake the watch and retry")
    return dev


def parse_notification(data: bytes) -> dict:
    if len(data) < 3:
        raise ValueError(f"notification too short: {data!r}")
    flags, title_len, body_len = data[0], data[1], data[2]
    if len(data) < 3 + title_len + body_len:
        raise ValueError(f"truncated payload: {data!r}")
    title = data[3 : 3 + title_len].decode("utf-8", errors="replace")
    body = data[3 + title_len : 3 + title_len + body_len].decode(
        "utf-8", errors="replace"
    )
    return {
        "has_data": bool(flags & 0x01),
        "has_unread": bool(flags & 0x02),
        "title": title,
        "body": body,
    }


async def cmd_scan(args: argparse.Namespace) -> None:
    print(f"Scanning ({args.timeout:.0f}s)...")
    devices = await BleakScanner.discover(timeout=args.timeout, return_adv=True)
    found = False
    for d, ad in sorted(devices.values(), key=lambda x: x[0].name or ""):
        name = d.name or ad.local_name or "?"
        svc = [u for u in ad.service_uuids if u.lower() == SERVICE_UUID.lower()]
        mark = " <-- HIKABOSHI" if name == DEVICE_NAME or svc else ""
        print(f"  {d.address}  rssi={ad.rssi:4d}  {name}{mark}")
        if mark:
            found = True
    if not found:
        print("  (Hikaboshi not seen — is the watch awake?)")


async def with_client(args: argparse.Namespace, fn) -> None:
    if args.address:
        addr = args.address
        print(f"Connecting to {addr}...")
        async with BleakClient(addr, timeout=20.0) as client:
            await fn(client)
    else:
        dev = await find_device(timeout=args.timeout)
        print(f"Connecting to {dev.address} ({dev.name})...")
        async with BleakClient(dev, timeout=20.0) as client:
            await fn(client)


async def cmd_connect(args: argparse.Namespace) -> None:
    async def go(client: BleakClient) -> None:
        print(f"Connected: {client.is_connected}")
        for uuid, label in (
            (BATTERY_UUID, "battery"),
            (STEPS_UUID, "steps"),
            (DISTANCE_UUID, "distance"),
            (CALORIES_UUID, "calories"),
            (NOTIFY_UUID, "notification"),
            (CONTROL_UUID, "control"),
        ):
            try:
                data = await client.read_gatt_char(uuid)
                if uuid == BATTERY_UUID:
                    print(f"  {label}: {data[0]}%")
                elif uuid == STEPS_UUID:
                    print(f"  {label}: {struct.unpack('<I', data[:4])[0]}")
                elif uuid == DISTANCE_UUID:
                    print(f"  {label}: {struct.unpack('<I', data[:4])[0]} m")
                elif uuid == CALORIES_UUID:
                    print(f"  {label}: {struct.unpack('<I', data[:4])[0] / 10.0:.1f} kcal")
                elif uuid == NOTIFY_UUID:
                    print(f"  {label}: {parse_notification(data)}")
                else:
                    print(f"  {label}: {data!r}")
            except Exception as e:
                print(f"  {label}: error {e}")

    await with_client(args, go)


async def cmd_notify(args: argparse.Namespace) -> None:
    payload = f"{args.title}|{args.body}".encode("utf-8")

    async def go(client: BleakClient) -> None:
        await client.write_gatt_char(NOTIFY_UUID, payload, response=True)
        print(f"Sent notification: {args.title!r} / {args.body!r}")

    await with_client(args, go)


async def cmd_battery(args: argparse.Namespace) -> None:
    async def go(client: BleakClient) -> None:
        data = await client.read_gatt_char(BATTERY_UUID)
        print(f"Battery: {data[0]}%")

    await with_client(args, go)


async def cmd_steps(args: argparse.Namespace) -> None:
    async def go(client: BleakClient) -> None:
        data = await client.read_gatt_char(STEPS_UUID)
        print(f"Steps: {struct.unpack('<I', data[:4])[0]}")

    await with_client(args, go)


async def cmd_fitness(args: argparse.Namespace) -> None:
    async def go(client: BleakClient) -> None:
        steps = struct.unpack("<I", (await client.read_gatt_char(STEPS_UUID))[:4])[0]
        dist_m = struct.unpack("<I", (await client.read_gatt_char(DISTANCE_UUID))[:4])[0]
        kcal10 = struct.unpack("<I", (await client.read_gatt_char(CALORIES_UUID))[:4])[0]
        print(f"Steps: {steps}")
        print(f"Distance: {dist_m} m ({dist_m / 1000.0:.2f} km)")
        print(f"Calories: {kcal10 / 10.0:.1f} kcal")

    await with_client(args, go)


async def cmd_time_sync(args: argparse.Namespace) -> None:
    ts = int(time.time())
    cmd = f"time={ts}".encode()

    async def go(client: BleakClient) -> None:
        await client.write_gatt_char(CONTROL_UUID, cmd, response=True)
        print(f"Sent time={ts} ({datetime.fromtimestamp(ts, tz=timezone.utc).isoformat()})")

    await with_client(args, go)


async def cmd_weather(args: argparse.Namespace) -> None:
    cmd = f"weather={args.temp},{args.code}".encode()

    async def go(client: BleakClient) -> None:
        await client.write_gatt_char(CONTROL_UUID, cmd, response=True)
        print(f"Sent {cmd.decode()}")

    await with_client(args, go)


async def cmd_screen(args: argparse.Namespace) -> None:
    cmd = f"screen_{args.state}".encode()

    async def go(client: BleakClient) -> None:
        await client.write_gatt_char(CONTROL_UUID, cmd, response=True)
        print(f"Sent {cmd.decode()}")

    await with_client(args, go)


async def cmd_wifi(args: argparse.Namespace) -> None:
    cmd = f"wifi_{args.state}".encode()

    async def go(client: BleakClient) -> None:
        await client.write_gatt_char(CONTROL_UUID, cmd, response=True)
        print(f"Sent {cmd.decode()}")

    await with_client(args, go)


async def cmd_ota(args: argparse.Namespace) -> None:
    if not args.url.startswith("https://"):
        raise SystemExit("OTA URL must start with https://")
    cmd = f"ota={args.url}".encode()

    async def go(client: BleakClient) -> None:
        await client.write_gatt_char(CONTROL_UUID, cmd, response=True)
        print(f"Sent OTA command: {args.url}")

    await with_client(args, go)


async def cmd_watch(args: argparse.Namespace) -> None:
    """Subscribe to all notifies and print until Ctrl-C."""

    def on_battery(_, data: bytearray) -> None:
        print(f"[battery] {data[0]}%")

    def on_steps(_, data: bytearray) -> None:
        if len(data) >= 4:
            print(f"[steps] {struct.unpack('<I', bytes(data[:4]))[0]}")

    def on_distance(_, data: bytearray) -> None:
        if len(data) >= 4:
            m = struct.unpack("<I", bytes(data[:4]))[0]
            print(f"[distance] {m} m")

    def on_calories(_, data: bytearray) -> None:
        if len(data) >= 4:
            print(f"[calories] {struct.unpack('<I', bytes(data[:4]))[0] / 10.0:.1f} kcal")

    def on_control(_, data: bytearray) -> None:
        print(f"[control] {data.decode('utf-8', errors='replace')!r}")

    def on_notify_chr(_, data: bytearray) -> None:
        try:
            print(f"[notify-chr] {parse_notification(bytes(data))}")
        except ValueError:
            print(f"[notify-chr] raw={bytes(data)!r}")

    async def go(client: BleakClient) -> None:
        await client.start_notify(BATTERY_UUID, on_battery)
        await client.start_notify(STEPS_UUID, on_steps)
        await client.start_notify(DISTANCE_UUID, on_distance)
        await client.start_notify(CALORIES_UUID, on_calories)
        await client.start_notify(CONTROL_UUID, on_control)
        await client.start_notify(NOTIFY_UUID, on_notify_chr)
        print("Streaming notifies — Ctrl-C to stop")
        try:
            while True:
                await asyncio.sleep(1.0)
        except asyncio.CancelledError:
            pass
        finally:
            for uuid in (BATTERY_UUID, STEPS_UUID, DISTANCE_UUID, CALORIES_UUID, CONTROL_UUID, NOTIFY_UUID):
                try:
                    await client.stop_notify(uuid)
                except Exception:
                    pass

    await with_client(args, go)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Hikaboshi BLE companion CLI")
    p.add_argument("--address", help="BLE address (skip scan)")
    p.add_argument("--timeout", type=float, default=10.0, help="Scan timeout seconds")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("scan", help="Scan for devices")
    sub.add_parser("connect", help="Connect and dump characteristics")

    n = sub.add_parser("notify", help="Send notification to watch")
    n.add_argument("title")
    n.add_argument("body")

    sub.add_parser("battery")
    sub.add_parser("steps")
    sub.add_parser("fitness", help="Steps + distance + calories")
    sub.add_parser("time-sync")

    w = sub.add_parser("weather")
    w.add_argument("temp", type=float)
    w.add_argument("code", type=int)

    for name in ("screen", "wifi"):
        s = sub.add_parser(name)
        s.add_argument("state", choices=["on", "off"])

    o = sub.add_parser("ota")
    o.add_argument("url", help="https:// URL to .bin")

    sub.add_parser("watch", help="Stream all notifications")
    return p


async def main_async(argv: list[str] | None = None) -> None:
    args = build_parser().parse_args(argv)
    handlers = {
        "scan": cmd_scan,
        "connect": cmd_connect,
        "notify": cmd_notify,
        "battery": cmd_battery,
        "steps": cmd_steps,
        "fitness": cmd_fitness,
        "time-sync": cmd_time_sync,
        "weather": cmd_weather,
        "screen": cmd_screen,
        "wifi": cmd_wifi,
        "ota": cmd_ota,
        "watch": cmd_watch,
    }
    try:
        await handlers[args.cmd](args)
    except KeyboardInterrupt:
        print("\nInterrupted")


def main() -> None:
    try:
        asyncio.run(main_async())
    except RuntimeError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
