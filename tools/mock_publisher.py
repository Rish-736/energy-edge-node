#!/usr/bin/env python3
"""
mock_publisher.py -- stand in for the gateway node.

Replays a captured telemetry file to an MQTT broker using exactly the topics
and JSON payloads the real gateway_node firmware publishes, including its
store-and-forward behaviour.

WHY THIS EXISTS
    The store-and-forward demo -- uplink drops, the chart gaps, the link
    returns, the gap fills in retroactively -- is the most convincing thing
    this project does. This script lets you record that demo with no ESP32,
    no sensor and no RS-485 wiring: just a broker and the dashboard.

    It also means the dashboard can be developed and debugged against
    realistic traffic without power-cycling hardware every time.

    It is a stand-in, not a simulation of the firmware: the firmware's own
    buffering logic is verified by the unit tests in
    meter_node/test/test_forwarder/.

SETUP
    pip install paho-mqtt
    mosquitto -c dashboard/mosquitto.conf

USAGE
    # straight replay at 1x
    python tools/mock_publisher.py docs/captures/sim_180s.csv

    # the demo: 20 s in, drop the uplink for 25 s, then backfill
    python tools/mock_publisher.py docs/captures/sim_180s.csv --outage 20:25

    # faster than real time
    python tools/mock_publisher.py docs/captures/sim_180s.csv --rate 4
"""

import argparse
import collections
import json
import pathlib
import sys
import time

TARIFF_INR_PER_KWH = 8.0
CO2_G_PER_KWH = 710.0

FLAG_SENSOR_FAIL = 0x01
FLAG_BACKFILL = 0x04
FLAG_SIMULATED = 0x80

# Matches the firmware: 512 samples at 10 Hz is ~51 s of outage coverage.
RING_SIZE = 512
BACKFILL_THRESHOLD = 20
BATCH = 10


def parse_capture(path):
    rows = []
    for line in pathlib.Path(path).read_text(errors="replace").splitlines():
        if not line.startswith("T,"):
            continue
        f = line.split(",")
        if len(f) < 9:
            continue
        try:
            rows.append({
                "seq": int(f[1]), "t_ms": int(f[2]),
                "v": float(f[3]), "i": float(f[4]), "p": float(f[5]),
                "wh": float(f[6]), "flags": int(f[7]), "truth": int(f[8]),
            })
        except ValueError:
            continue
    return rows


def parse_outage(spec):
    """'20:25' -> link drops 20 s in, for 25 s."""
    if not spec:
        return None
    try:
        at, dur = spec.split(":")
        return float(at), float(dur)
    except ValueError:
        sys.exit(f"--outage wants START:DURATION in seconds, got {spec!r}")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture")
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--root", default="een/bench")
    ap.add_argument("--unit", type=int, default=1)
    ap.add_argument("--rate", type=float, default=1.0,
                    help="replay speed multiplier (default 1x)")
    ap.add_argument("--outage", metavar="START:DURATION",
                    help="simulate an uplink outage, in seconds")
    ap.add_argument("--loop", action="store_true",
                    help="restart the capture when it ends")
    args = ap.parse_args()

    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        sys.exit("paho-mqtt is not installed.  pip install paho-mqtt")

    rows = parse_capture(args.capture)
    if not rows:
        sys.exit(f"no 'T,' telemetry rows in {args.capture}")

    outage = parse_outage(args.outage)

    t_tele = f"{args.root}/meter/{args.unit}/telemetry"
    t_mstat = f"{args.root}/meter/{args.unit}/status"
    t_gstat = f"{args.root}/gateway/status"
    t_stats = f"{args.root}/gateway/stats"

    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                             client_id="een-mock")
    except (AttributeError, TypeError):            # paho 1.x
        client = mqtt.Client(client_id="een-mock")

    # Same Last Will the firmware registers, so a hard kill of this script
    # looks to the dashboard exactly like a gateway dropping off.
    client.will_set(t_gstat, "offline", retain=True)

    print(f"connecting to {args.host}:{args.port} ...")
    try:
        client.connect(args.host, args.port, keepalive=15)
    except OSError as e:
        sys.exit(f"cannot reach the broker: {e}\n"
                 f"start one with:  mosquitto -c dashboard/mosquitto.conf")
    client.loop_start()

    client.publish(t_gstat, "online", retain=True)
    client.publish(t_mstat, json.dumps(
        {"online": 1, "fw": "1.0", "caps": 0x0F, "simulated": 1}), retain=True)

    print(f"replaying {len(rows)} samples to {t_tele} at {args.rate}x")
    if outage:
        print(f"uplink will drop {outage[0]:.0f} s in, for {outage[1]:.0f} s")
    print("ctrl-c to stop\n")

    # Our stand-in for the firmware's ring + forwarder.
    ring = collections.deque(maxlen=RING_SIZE)
    published = dropped = outages = peak_backlog = 0
    link_up = True

    t_start = time.monotonic()
    t0_ms = rows[0]["t_ms"]
    last_stats = 0.0
    idx = 0

    try:
        while True:
            now = time.monotonic() - t_start
            sim_s = now * args.rate

            if outage:
                in_outage = outage[0] <= sim_s < outage[0] + outage[1]
                if in_outage and link_up:
                    link_up = False
                    outages += 1
                    print(f"[{sim_s:6.1f}s] uplink DOWN -- buffering")
                elif not in_outage and not link_up:
                    link_up = True
                    print(f"[{sim_s:6.1f}s] uplink UP -- backfilling "
                          f"{len(ring)} buffered samples")

            # Produce every sample whose time has come, link or no link.
            while idx < len(rows) and (rows[idx]["t_ms"] - t0_ms) / 1000.0 <= sim_s:
                if len(ring) == ring.maxlen:
                    dropped += 1          # buffer full: genuine loss, counted
                ring.append(rows[idx])
                idx += 1

            # Drain, rate-limited, exactly as the firmware does.
            if link_up and ring:
                catching_up = len(ring) > BACKFILL_THRESHOLD
                peak_backlog = max(peak_backlog, len(ring))
                for _ in range(min(BATCH, len(ring))):
                    r = ring.popleft()
                    kwh = r["wh"] / 1000.0
                    flags = r["flags"] | (FLAG_BACKFILL if catching_up else 0)
                    client.publish(t_tele, json.dumps({
                        "unit": args.unit, "seq": r["seq"],
                        "t": int(time.time() * 1000)
                             - int(((rows[-1]["t_ms"] - r["t_ms"])) / args.rate),
                        "up": r["t_ms"],
                        "v": round(r["v"], 3), "i": round(r["i"], 4),
                        "p": round(r["p"], 3), "wh": round(r["wh"], 6),
                        "cost": round(kwh * TARIFF_INR_PER_KWH, 4),
                        "co2": round(kwh * CO2_G_PER_KWH, 2),
                        "flags": flags, "truth": r["truth"],
                        "bf": 1 if catching_up else 0,
                    }))
                    published += 1

            if now - last_stats >= 2.0:
                last_stats = now
                client.publish(t_stats, json.dumps({
                    "devices": 1, "published": published,
                    "backlog": len(ring), "peak_backlog": peak_backlog,
                    "dropped": dropped, "outages": outages,
                    "rssi": -54, "heap": 210000, "clock": 1,
                }), retain=True)

            if idx >= len(rows) and not ring:
                if args.loop:
                    idx, t_start = 0, time.monotonic()
                    print("--- looping ---")
                    continue
                break

            time.sleep(0.02)

        print(f"\ndone: published={published} dropped={dropped} "
              f"outages={outages} peak_backlog={peak_backlog}")

    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        client.publish(t_gstat, "offline", retain=True)
        client.publish(t_mstat, json.dumps({"online": 0}), retain=True)
        time.sleep(0.3)
        client.loop_stop()
        client.disconnect()

    return 0


if __name__ == "__main__":
    sys.exit(main())
