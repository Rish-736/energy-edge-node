# Dashboard — Floor 3

Live telemetry from the gateway, over MQTT.

Nothing here is load-bearing: the gateway keeps polling, and from Phase 3 keeps
shedding load, with this page closed. That is the point of putting control on
the edge rather than upstream.

---

## The quickest way to see it work

No broker, no hardware:

1. Open `index.html` in a browser.
2. Click **Replay a capture** and pick `docs/captures/sim_180s.csv`.

The charts fill in at 20x speed. This exercises the real parser on a real
capture, so it is a genuine check that the page works — just not of the MQTT
path.

---

## The full path, with a broker

### 1. Install and start Mosquitto

```bash
# Windows:  https://mosquitto.org/download/   (or: winget install EclipseFoundation.Mosquitto)
# macOS:    brew install mosquitto
# Linux:    sudo apt install mosquitto mosquitto-clients

mosquitto -c dashboard/mosquitto.conf
```

Two listeners come up: **1883** for the ESP32 and **9001** for the browser. A
web page cannot open a raw TCP socket, so the dashboard needs MQTT wrapped in
websockets; the broker bridges the two.

### 2. Feed it data

**Option A — no hardware.** Replays a capture using the exact topics and
payloads the firmware publishes:

```bash
pip install paho-mqtt
python tools/mock_publisher.py docs/captures/sim_180s.csv
```

**Option B — the real gateway.**

```bash
cp gateway_node/include/secrets.example.h gateway_node/include/secrets.h
# edit it: WiFi SSID/password, and MQTT_HOST = your machine's LAN address
pio run -e esp32dev -d gateway_node -t upload
pio device monitor -e esp32dev -d gateway_node
```

`MQTT_HOST` must be the LAN address of the machine running Mosquitto
(`ipconfig` on Windows, `ip addr` on Linux) — not `localhost`, which from the
ESP32's point of view means the ESP32.

### 3. Open the dashboard

Open `index.html`, leave the broker at `ws://localhost:9001`, click
**Connect**.

---

## The demo worth recording

This is the sequence that makes the project memorable:

```bash
python tools/mock_publisher.py docs/captures/sim_180s.csv --outage 20:25
```

Twenty seconds in, the uplink drops for twenty-five seconds. Watch:

1. The power chart **stops advancing** and a gap opens.
2. The events panel logs that readings are being buffered.
3. The link returns — and **the gap fills in**, drawn in amber.

The chart is plotted against each sample's own timestamp rather than its
arrival time, which is why history can appear retroactively. Nothing measured
during the outage is lost.

With the real gateway, cause the same thing by turning off your WiFi hotspot,
or by killing Mosquitto and restarting it.

That is the feature separating a product from a toy — toys lose data. The
firmware's buffering logic is proven independently by the unit tests in
`meter_node/test/test_forwarder/`.

---

## MQTT topics

| Topic | Payload |
|---|---|
| `een/bench/gateway/status` | `online` / `offline` — retained, set as the gateway's Last Will |
| `een/bench/gateway/stats` | JSON: published, backlog, dropped, outages, RSSI, heap, clock — retained |
| `een/bench/meter/<unit>/status` | JSON: online, firmware version, capabilities, **simulated** — retained |
| `een/bench/meter/<unit>/telemetry` | JSON: one message per sample |

Telemetry payload:

```json
{"unit":1,"seq":1234,"t":1760000000000,"up":123400,
 "v":11.873,"i":5.3104,"p":63.042,"wh":1.328401,
 "cost":0.0106,"co2":0.94,"flags":128,"truth":6,"bf":0}
```

`t` is UTC milliseconds (0 if the gateway never reached an NTP server), `up` is
milliseconds since the meter booted, `bf` marks a sample that arrived late
after an outage, and `truth` is the simulator's ground truth for Phase 5.

### Two details that are easy to skip

**Last Will and Testament.** The broker publishes `offline` on the gateway's
behalf if it vanishes without saying goodbye. Without it, a dead gateway looks
identical to an idle one and the dashboard would keep showing the last reading
as though it were current.

**Retained status and stats.** A dashboard opened later immediately learns what
exists and how healthy it is, instead of staring at an empty page until the
next poll.

---

## Inspecting the bus by hand

```bash
mosquitto_sub -h localhost -t 'een/bench/#' -v
```

---

## Limitations

- **No authentication or TLS.** `allow_anonymous true` is fine on a laptop
  behind a home router and unacceptable on anything exposed. Phase 4 adds TLS,
  credentials and per-topic access control.
- Charts hold the last ~4 minutes in memory; there is no historical store yet.
- Cost and CO₂ use flat assumed rates from `gateway_node/include/config.h`.
  Time-of-use pricing arrives with the Phase 3 controller. Set them to your
  own tariff before quoting any figure.
