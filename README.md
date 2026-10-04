# Intelligent Energy Edge Node

An EcoStruxure-style energy node: it senses DC power, exposes the readings over
**Modbus-RTU/RS-485**, gateways them to **MQTT**, and autonomously **sheds
non-critical load** to cut peak demand and cost.

Built entirely on a **safe low-voltage DC testbed (≤24 V)** — deliberately not
mains. Same protocols, same control problem, no mains risk.

> **Status: Phase 1 complete.** Sensing, telemetry, logging and a full
> Modbus-RTU slave/master pair all work, verified by 24 host unit tests.
> Runs today against a built-in appliance simulator and a virtual RS-485 bus;
> swaps to real hardware by changing the build environment.

New here? Start with **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** � it
explains the whole system in plain language, no background assumed.

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
| `native`       | PC      | Unit tests � ring buffer and Modbus protocol |

The gateway node has one environment, `esp32dev`.

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

# flash the meter (simulated loads) and watch the stream
pio run -e esp32dev-sim -d meter_node -t upload
pio device monitor -e esp32dev-sim

# flash the gateway; it scans the bus and starts polling
pio run -e esp32dev -d gateway_node -t upload
pio device monitor -e esp32dev -d gateway_node
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

## Modbus-RTU (Phase 1)

The meter is a **Modbus-RTU slave**; the gateway is a **Modbus-RTU master**.
Full register map: **[docs/MODBUS_REGISTERS.md](docs/MODBUS_REGISTERS.md)**.
Machine-readable: **[docs/device_model.json](docs/device_model.json)**.

| | |
|---|---|
| Physical layer | RS-485 half duplex, 19200 8N1 |
| Function codes | `0x03`, `0x04` read � `0x06`, `0x10` write |
| Poll | one FC4 transaction, 39 registers, 10 Hz |
| Float encoding | float32, **high word first** (PowerLogic convention) |
| Addressing | protocol (0-based) |

### Tested without any RS-485 hardware

The protocol code knows nothing about UARTs � you hand it a frame and a reply
buffer. So a master and a slave can talk **through memory**, and the whole
request/response cycle is verified on a PC: CRC, framing, float word order,
block reads, exceptions, broadcast suppression, and the retry path.

18 of the 24 unit tests cover Modbus. The ones worth knowing about:

| Test | Why it exists |
|---|---|
| `float_word_order_is_high_word_first` | Swapped words is *the* classic Modbus bug � every float becomes a wild magnitude. Known-answer test: `1.0f` must encode as `0x3F80,0x0000`. |
| `crc_of_frame_including_crc_is_zero` | Verifies the CRC algorithm *and* its byte order in one assertion. |
| `bad_crc_is_silently_dropped` | A corrupt frame must get **no reply**. Answering line noise turns it into bus traffic. |
| `broadcast_write_applies_but_is_not_answered` | Unit 0 must act without replying, or several slaves collide on a half-duplex line. |
| `write_to_measurement_register_is_rejected` | A master must not be able to overwrite a measurement and read back its own lie. |
| `scaled_block_clamps_garbage` | A NaN or a sensor fault must saturate, not wrap into a plausible wrong number. |

### Things the firmware does that are easy to get wrong

- **DE is dropped only after `flush()`.** The UART must finish shifting out the
  last stop bit before the transceiver stops driving, or the end of every frame
  is truncated and shows up as a CRC error at the far end.
- **Framing is a timer, not a parser.** Modbus-RTU has no end byte; a frame ends
  after 3.5 character times of silence (~2 ms at 19200).
- **The input buffer is flushed before each request.** Otherwise a late reply to
  the *previous* question gets parsed as the answer to this one � which yields
  plausible but wrong data, the worst failure mode available.
- **The slave answers with its own cursor on the ring**, fast-forwarding to the
  newest sample. Registers hold present values, not history � while the logger,
  reading the same ring, keeps every sample. Two readers, two needs, neither
  aware of the other.
- **`sample_seq` is published** so a master can tell "steady load" from "the
  meter has silently stopped sampling". Reading power alone cannot distinguish
  those two, because they look identical.

### Discovery instead of hard-coding

The gateway sweeps unit IDs 1�16 at startup and asks each one "who are you?".
Whoever answers with the right vendor ID is added to the poll list. Adding a
second meter is a DIP-switch change, not a code change � which is how real
commissioning tools behave.

It also reads the `SIMULATED` capability bit, so the gateway knows whether it
is looking at real measurements or synthetic ones. Data that silently claims to
be real when it is not is worse than no data.

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
- Readings come from the simulator unless built with `esp32dev`; accuracy
  against a reference multimeter is still to be measured.
- **The INA226 driver and the RS-485 transport are written from the datasheets
  and have never run on real silicon.** The protocol logic above them is
  tested; the two layers that touch hardware are not, and will need bring-up
  when the parts arrive. See the checklist at the end of
  [docs/MODBUS_REGISTERS.md](docs/MODBUS_REGISTERS.md).
- Timestamps are milliseconds since boot, which is unusable for billing or
  tariff windows. Real clock time (SNTP) arrives in Phase 2.

Next after the roadmap: Modbus-TCP and BACnet, OT-security hardening.
