# Intelligent Energy Edge Node

An EcoStruxure-style energy node: it senses DC power, exposes the readings over
**Modbus-RTU/RS-485**, gateways them to **MQTT**, and autonomously **sheds
non-critical load** to cut peak demand and cost.

Built entirely on a **safe low-voltage DC testbed (≤24 V)** — deliberately not
mains. Same protocols, same control problem, no mains risk.

> **Status: Phase 0 complete.** Sensing, telemetry and logging work end to end.
> Runs today against a built-in appliance simulator; swaps to a real INA226 by
> changing the build environment.

---

## Architecture

```
[DC loads] -> [INA226 sense] -> (telemetry ring buffer) -> Modbus-RTU slave (RS-485)
                                       |                            |
                                 [future: NILM]           Modbus master + gateway (ESP32)
                                                                    |
                                     +------------------------------+------------------+
                                     |                              |                  |
                            demand-response                MQTT -> dashboard     cost / CO2
                            controller -> relays            (live charts)
```

Everything downstream reads from **one telemetry ring buffer**. The sampler is
the only producer; the logger, Modbus slave, MQTT publisher, control loop and
(Phase 5) the NILM classifier are independent consumers with their own cursors.
A consumer that stalls cannot stall the sampler or any other consumer — it just
takes an overrun, which is counted rather than hidden.

---

## Repo layout

```
meter_node/     ESP32 — INA226 sampling, energy accumulation, Modbus-RTU slave
gateway_node/   ESP32 — Modbus master, MQTT publisher, demand-response control   (Phase 2+)
dashboard/      web — MQTT-over-websockets live charts                            (Phase 2)
tools/          replay.py — parse, validate and plot a telemetry capture
docs/           captures, plots, Modbus register map
```

---

## Build targets

The meter node has four PlatformIO environments:

| Environment    | Runs on | Purpose |
|----------------|---------|---------|
| `esp32dev-sim` | ESP32   | Simulated appliances — **no sensor needed** (default) |
| `esp32dev`     | ESP32   | Real INA226 over I2C |
| `native-sim`   | PC      | Whole pipeline on the host, emits a capture to stdout |
| `native`       | PC      | Unit tests for the ring buffer |

The only difference between the two ESP32 builds is `-D USE_INA226_SIM`. Both
link the identical telemetry, energy and ring-buffer code.

### Run it

```bash
# unit tests — no hardware
pio test -e native -d meter_node

# generate a 180 s capture on your PC
pio run -e native-sim -d meter_node
./meter_node/.pio/build/native-sim/program 180 > docs/captures/sim_180s.csv

# summarise and plot it
python tools/replay.py docs/captures/sim_180s.csv

# flash the ESP32 (simulated loads) and watch the stream
pio run -e esp32dev-sim -d meter_node -t upload
pio device monitor -e esp32dev-sim
```

---

## The appliance simulator

Three loads on a 12 V bus, each with a deliberately distinct signature:

| Load   | Steady | Signature |
|--------|--------|-----------|
| lights | 5 W    | clean step, no transient |
| heater | 40 W   | step, then ~6% droop as the element warms and its resistance rises |
| motor  | 18 W   | ~3.5× inrush decaying over ~250 ms, then 7 Hz commutation ripple |

The bus sags under load (`V = 12.0 − 0.18·I`), and a seeded PRNG makes every run
byte-identical.

This is not a placeholder for missing hardware — it is also the **Phase 5
training rig**. Every sample carries a `truth` bitmask of which loads were
really on, which is exactly the vector a NILM classifier outputs. The classifier
can be scored against known ground truth before it ever meets a real appliance.

---

## Telemetry log format

Data rows are prefixed `T,` so a capture survives being mixed with boot chatter
and `#` notes on the same serial line.

```
# energy-edge-node telemetry v1
# source=sim sensor_init=ok rate_hz=10 ring=256
# fields: seq,t_ms,v_v,i_a,p_w,wh,flags,truth
T,0,0,11.999,0.0000,0.000,0.000000,128,0
```

| Field   | Meaning |
|---------|---------|
| `seq`   | monotonic sample number — a gap proves dropped samples |
| `t_ms`  | ms since boot |
| `v,i,p` | bus volts, amps, watts (power from the INA226's own register, not V×I) |
| `wh`    | cumulative energy, trapezoidal over real elapsed time |
| `flags` | `0x01` sensor fail · `0x02` over-power · `0x80` simulated |
| `truth` | `0x01` lights · `0x02` heater · `0x04` motor (simulator ground truth) |

---

## Phase 0 results

180 s host capture (`docs/captures/sim_180s.csv`):

```
samples      1800        dropped      0
rate         10.01 Hz    sensor fail  0
voltage      10.97 .. 12.01 V
power        0.00 .. 68.17 W  (mean 26.57)
energy       1.3284 Wh
peak/mean    2.57x   <-- Phase 3 target
```

![telemetry capture](docs/captures/sim_180s.png)

That **2.57× peak-to-mean ratio is the "before" number.** Phase 3's job is to
shrink it by shedding the heater when the motor and lights are already running.

Firmware footprint: RAM 9.4% (30.7 kB), Flash 23.9% (314 kB).

---

## Roadmap

- [x] **Phase 0** — INA226 sampling, telemetry ring buffer, energy accumulation, replayable logs
- [ ] **Phase 1** — Modbus-RTU slave over RS-485; gateway polls it as master
- [ ] **Phase 2** — MQTT gateway + live web dashboard
- [ ] **Phase 3** — demand-response control: shed/restore with hysteresis and load priority
- [ ] **Phase 4** — full FreeRTOS task split, MQTT over TLS
- [ ] **Phase 5** — NILM appliance disaggregation from the aggregate signal

---

## Limitations & future work

- Low-voltage DC testbed only; no mains metering, and no AC concerns
  (power factor, harmonics, true RMS) are modelled.
- Energy is integrated from the boot instant and is not persisted across resets.
- Phase 0 readings come from the simulator unless built with `esp32dev`;
  accuracy against a reference multimeter is still to be measured.

Next after the roadmap: Modbus-TCP and BACnet, OT-security hardening.
