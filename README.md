# Intelligent Energy Edge Node

An EcoStruxure-style energy node — **metering device → edge gateway → dashboard**.
It measures power, shares the readings over industrial **Modbus-RTU/RS-485**,
gateways them to **MQTT**, and **automatically sheds non-critical loads to cut
peak demand and cost**.

What makes it different from a thermostat with extra steps: every load it defers
carries an **energy requirement and a deadline**, and the controller is not
permitted to miss one. Service is a guarantee, not something traded away for a
smaller bill.

Built entirely on a **safe low-voltage DC testbed (≤24 V)** — deliberately not
mains. Same protocols, same control problem, no mains risk.

> **Status: Phase 3 complete.** Sensing, telemetry, a Modbus-RTU slave/master
> pair, an MQTT uplink with store-and-forward buffering, a live dashboard, and
> a deadline-aware demand controller — verified by 52 host unit tests.
>
> **Measured result: peak demand cut 58.2%, with zero deadline misses.**

New here? Start with **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — it
explains the whole system in plain language, no background assumed.
Why each choice was made: **[docs/DECISIONS.md](docs/DECISIONS.md)**.
How the code executes: **[docs/FLOW.md](docs/FLOW.md)**.

---

## The problem, and the site this targets

Commercial electricity bills have two parts: the **energy charge** (total kWh)
and the **demand charge**, set by the single highest 15-minute average power
in the billing period. One bad quarter-hour sets that charge for the whole
month.

At an **EV charging site** the demand charge is reported to run **30–70% of the
electricity bill**, and facility-level load management is reported to cut it by
**35–55% without reducing charging availability.**

That site is the target, and it is chosen deliberately:

**A car plugged in at 19:00 and needed at 07:00 requires perhaps four hours of
charge inside a twelve-hour window.** Eight hours of slack. The driver's
requirement is not "charge now" — it is **"full by morning."** Spend the slack
and nobody notices anything at all.

Compare the usual demand-response demo: shedding a domestic water heater. There
the occupant may want hot water in five minutes and has declared no deadline, so
there is no slack that can be spent safely. In a published field trial of 191
households on grid-interactive water heaters, **around 40% reported cold water
draws and two participants withdrew over comfort.** That failure mode is what
this project is designed to avoid, by choosing loads whose deferral is free.

So the controller's question is never *"whose comfort do I sacrifice?"* It is:

```
Each load needs E watt-hours by time T.
Find a schedule that meets every deadline
while keeping the 15-minute peak as low as possible.
```

---

## What makes this more than a scaled-down copy

Four things, graded honestly — two are framing, two are genuine contributions.

**1. Deadline-guaranteed deferral, not comfort-traded shedding.**
The objective is inverted: deadlines are **hard constraints**, peak is the thing
minimised. "Zero deadline misses" is a binary, verifiable claim.
*Honest status:* studied academically as departure-time-constrained charging.
Vendor and hobby material overwhelmingly describes capacity splitting and load
balancing instead. **Strong framing; not a novel algorithm.**

**2. NILM as actuation verification — the strongest card.**
A relay is an open-loop actuator: you command it and get no confirmation back.
Contacts weld, channels get miswired, loads get unplugged. The controller then
believes it shed 40 W, believes the peak is handled, stops looking — and the
demand charge lands anyway with nothing in the system noticing.
Phase 5 closes that loop using **the meter already installed for metering**: the
classifier independently confirms, from the single aggregate signal, that the
commanded load actually changed state.
*Honest status:* a 2025 systematic review names this exact closed loop as a
requirement and notes the components exist separately, with no full embedded
deployment closing it. **This is the real contribution.**

**3. Service cost reported as a first-class metric.**
Deadline misses and service violations are published alongside kW and ₹.
*Honest status:* not a novel idea — it is discipline. But the 40% cold-water
statistic above is exactly what happens when nobody reports it.

**4. Retrofit-first: one meter, ordinary relays, no cloud.**
Schneider's answer to per-load visibility is PowerTag — one wireless sensor per
circuit breaker. This needs **one** meter.
*Honest status:* a deliberate accuracy-for-installation-cost trade. Installation
cost is the actual barrier to deployment.

---

## Architecture

```
[site loads] -> [INA226 sense] -> (telemetry ring buffer) -> Modbus-RTU slave (RS-485)
                                       |                            |
                                 [future: NILM]           Modbus master + gateway (ESP32)
                                                                    |
                                     +------------------------------+------------------+
                                     |                              |                  |
                            deadline-aware shed            MQTT -> dashboard     cost / CO2
                            controller -> relays            (live charts)
```

Three tiers, mapped onto EcoStruxure's own layering:

| Tier | EcoStruxure layer | Their product | Here |
|---|---|---|---|
| 1 | Connected Products | PowerLogic meters | `meter_node/` |
| 2 | Edge Control | Panel Server | `gateway_node/` |
| 3 | Apps & Analytics | Power Monitoring Expert | `dashboard/` |

**The shed decision lives on tier 2, not in the cloud.** Networks drop; if the
decision lived upstream, a dropped link would mean no protection at the moment
you most need it. Unplug the network and the controller keeps working.

Everything downstream reads from **one telemetry ring buffer**. The sampler is
the only producer; the logger, Modbus slave, MQTT publisher, controller and
(Phase 5) the NILM classifier are independent consumers with their own cursors.
A consumer that stalls cannot stall the sampler or any other consumer — it just
takes an overrun, which is counted rather than hidden.

The same buffer runs at both tiers: on the meter it holds sampling history, on
the gateway it *is* the store-and-forward buffer that survives an MQTT outage.

---

## Repo layout

```
meter_node/        ESP32 — INA226 sampling, energy accumulation, Modbus-RTU slave
gateway_node/      ESP32 — Modbus master, MQTT uplink, store-and-forward
common/telemetry/  shared sample format, ring buffer, store-and-forward
common/modbus/     shared protocol — framing, register map, RS-485 transport
dashboard/         web — live MQTT charts, broker config
tools/             replay.py, mock_publisher.py, check_device_model.py
docs/              architecture, decisions, flow, register map, captures
```

Everything under `common/` is compiled into **both** firmwares. The register map
has exactly one definition, so the two ends of the bus cannot drift apart.

---

## Build targets

| Environment | Runs on | Purpose |
|---|---|---|
| `esp32dev-sim` | ESP32 | Simulated loads — **no sensor needed** (default) |
| `esp32dev` | ESP32 | Real INA226 over I2C |
| `native-sim` | PC | Whole pipeline on the host, emits a capture |
| `native` | PC | Unit tests — ring buffer, Modbus, store-and-forward, control |
| `native-site` | PC | Closed-loop site model: baseline vs controlled |

The gateway node has one environment, `esp32dev`. The only difference between
the two ESP32 meter builds is `-D USE_INA226_SIM`.

### Run it

```bash
# unit tests — no hardware (52 tests)
pio test -e native -d meter_node

# verify the register map's three copies still agree
python tools/check_device_model.py

# generate a 180 s capture on your PC
pio run -e native-sim -d meter_node
./meter_node/.pio/build/native-sim/program 180 > docs/captures/sim_180s.csv
python tools/replay.py docs/captures/sim_180s.csv

# flash the meter (simulated loads) and watch the stream
pio run -e esp32dev-sim -d meter_node -t upload
pio device monitor -e esp32dev-sim

# flash the gateway; it scans the bus, polls, and publishes to MQTT
cp gateway_node/include/secrets.example.h gateway_node/include/secrets.h  # then edit
pio run -e esp32dev -d gateway_node -t upload
```

See the dashboard with no broker and no hardware: open `dashboard/index.html`,
click **Replay a capture**, pick `docs/captures/sim_180s.csv`.

---

## The bench loads and what they model

Three loads on a 12 V bus. The physical hardware does not change; what changes
is the role each one plays in the site model.

| Bench hardware | Code name today | Role in the EV-site model | Deferrable? |
|---|---|---|---|
| 40 W power resistor | `LOAD_HEATER` | **EV charger** — needs *E* Wh by deadline *T* | yes, with a deadline |
| 18 W DC motor | `LOAD_MOTOR` | **thermal load** — maintain a buffer within a band | yes, within bounds |
| 5 W LEDs | `LOAD_LIGHTS` | **base load** — lighting, controls, metering | **never shed** |

> The `LOAD_*` identifiers still carry their Phase 0 names. They are renamed in
> Phase 3 alongside the controller, so that code and documentation change in one
> commit rather than drifting apart.

Each has a deliberately distinct electrical signature, which is what the Phase 5
classifier keys off:

| Load | Steady | Signature |
|---|---|---|
| charger (resistor) | 40 W | step, then ~6% droop as the element warms and its resistance rises |
| thermal (motor) | 18 W | ~3.5× inrush decaying over ~250 ms, then 7 Hz commutation ripple |
| base (LEDs) | 5 W | clean step, no transient |

The bus sags under load (`V = 12.0 − 0.18·I`) and a seeded PRNG makes every run
byte-identical.

**The simulator earns its keep twice.** Every sample carries a `truth` bitmask of
which loads were really on — exactly the vector a NILM classifier outputs. So it
is both a stand-in for absent hardware *and* the Phase 5 training and scoring
rig.

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

| Field | Meaning |
|---|---|
| `seq` | monotonic sample number — a gap proves dropped samples |
| `t_ms` | ms since boot |
| `v,i,p` | bus volts, amps, watts (power from the INA226's own register, not V×I) |
| `wh` | cumulative energy, trapezoidal over real elapsed time |
| `flags` | `0x01` sensor fail · `0x02` over-power · `0x04` backfilled · `0x80` simulated |
| `truth` | `0x01` base · `0x02` charger · `0x04` thermal (simulator ground truth) |

---

## How the controller decides (Phase 3)

Full reasoning: [docs/DECISIONS.md](docs/DECISIONS.md) D83, D86–D90.

### It acts on a forecast, not on the present value

A demand charge is set by the average over a billing interval. Knowing the
average *so far* is useless — by the time it crosses the limit the interval is
nearly over and shedding cannot pull it back. So the controller projects where
the interval will **land**:

```
projected = (energy_so_far + current_power * time_remaining) / window
```

That also means shedding late is worth less: with little of the window left,
removing a load barely moves the average. `dw_excess_w()` accounts for this —
the power it asks you to remove grows as the window runs out.

### Slack decides which load, not a fixed priority list

```
energy_remaining = required - delivered
time_needed      = energy_remaining / rated_power
slack            = (deadline - now) - time_needed
```

Shed the load with the **most** slack; restore the one with the **least**.
A load whose slack reaches zero becomes untouchable and is force-restored if
it was already off — the deadline outranks the bill.

This makes eligibility **dynamic**. Priorities never change, but which load may
be shed right now changes second by second as slack drains. A static priority
list would keep shedding the same load even as it ran out of time.

### What it refuses to do

| Refusal | Why |
|---|---|
| Shed a `FIXED` load | Lighting and controls are not negotiable |
| Shed a load with no slack | The deadline is a hard constraint |
| Shed before `min_on_ms` | Compressors need a minimum run or they are damaged |
| Restore before `min_off_ms` | Restart inrush and winding heat |
| Restore more than one load per stagger interval | Releasing several together creates a brand-new peak — the classic way a naive controller makes things worse |
| Re-shed immediately after a forced restore | A drained buffer needs time to refill |

That last one was a real bug, caught by a test: the controller force-restored a
load and then re-shed it on the very same step, cycling the equipment and
defeating `max_shed_ms` entirely.

### Tested without a relay, a sensor, or a board

`sched_step()` takes a power reading and a timestamp and returns a bitmask. It
touches no hardware, so 18 tests drive it through complete scenarios — a full
demand window, a deadline approaching, slack running out — in microseconds.

| Test | Why it exists |
|---|---|
| `deadline_is_met_under_sustained_pressure` | The project's central claim, asserted |
| `critical_load_is_never_shed` | A load with no slack is untouchable even when over target |
| `slack_running_out_forces_a_restore` | The controller gives time back before the deadline bites |
| `fixed_load_is_never_shed` | And it records a refusal rather than failing silently |
| `max_shed_duration_forces_a_restore` | Buffer limits override the demand target |
| `restores_are_staggered` | Prevents the controller causing the peak it exists to avoid |
| `projection_sees_trouble_before_the_average_does` | The forecast, which is what makes it a demand controller |

---

## MQTT, store-and-forward and the dashboard (Phase 2)

The gateway publishes every poll to an MQTT broker; a web page charts it live.
Setup and topics: **[dashboard/README.md](dashboard/README.md)**.

### Store-and-forward — the feature worth demoing

WiFi drops. A naive publisher throws away every reading taken while the link was
down, leaving holes in the energy record precisely when something interesting
may have happened.

Instead, readings accumulate in the ring buffer whether or not the link is up.
The forwarder is just a reader that **refuses to advance its cursor while
disconnected**; when the link returns it drains the backlog oldest-first, and the
missing history arrives late rather than never. This is how an industrial gateway
behaves — Schneider's Panel Server uses its logging memory as exactly this kind
of buffer.

```bash
python tools/mock_publisher.py docs/captures/sim_180s.csv --outage 20:25
```

Twenty seconds in, the uplink drops for twenty-five. On the dashboard the power
chart stops, a gap opens — and when the link returns **the gap fills in**, drawn
in amber. The chart plots each sample against its own timestamp rather than its
arrival time, which is what lets history appear retroactively.

No ESP32 and no sensor needed for that demo. Just a broker.

### What it does not pretend to do

The ring is finite — 512 samples, ~51 s at 10 Hz. A longer outage is **real**
data loss, and the forwarder reports exactly how many readings went missing.
`test_outage_beyond_buffer_reports_exact_loss` asserts that published + dropped
accounts for every sample the ring ever saw.

### Also in Phase 2

- **Real clock time (SNTP, UTC).** Timestamps were milliseconds-since-boot,
  unusable for billing and useless to a tariff window. The gateway takes one SNTP
  reading then tracks `base + millis()` delta rather than calling `time()` per
  sample — `time()` is second-resolution and can step *backwards* when SNTP
  corrects it, and a log whose time goes backwards is very hard to reason about.
- **Last Will and Testament.** If the gateway vanishes without saying goodbye the
  broker publishes `offline` for it. Without this a dead gateway looks identical
  to an idle one, and the dashboard would keep showing the last reading as though
  it were current.
- **Retained status and stats topics**, so a dashboard opened later immediately
  learns what exists and how healthy it is.
- **Credentials in a gitignored `secrets.h`**, with a committed example. The
  firmware still builds without it — it just stays offline.
- **Two decoupled tasks.** `poll` (prio 4, core 1) runs at a fixed 10 Hz and
  never waits on the network; `uplink` (prio 3, core 0) handles WiFi, SNTP and
  MQTT. Store-and-forward falls out of that split rather than being bolted on.

### Tested without WiFi, a broker, or a board

10 of the 52 tests cover the forwarder, because the logic is "a reader that
refuses to advance while the link is down" — drivable entirely by toggling a
boolean:

| Test | Why it exists |
|---|---|
| `link_starts_down` | A forwarder assuming the link was up would publish into a void and lose the first samples |
| `outage_buffers_instead_of_discarding` | The core claim: nothing published, nothing lost |
| `reconnect_backfills_in_order` | Backlog drains oldest-first and respects the rate budget |
| `outage_beyond_buffer_reports_exact_loss` | published + dropped must equal everything produced |
| `after_loss_survivors_are_the_newest_window` | Backfilling stale data while discarding recent data would be worse than dropping cleanly |
| `small_backlog_is_not_called_backfill` | Normal scheduling jitter must not be reported as an outage |

---

## Modbus-RTU (Phase 1)

The meter is a **Modbus-RTU slave**; the gateway is a **Modbus-RTU master**.
Full register map: **[docs/MODBUS_REGISTERS.md](docs/MODBUS_REGISTERS.md)**.
Machine-readable: **[docs/device_model.json](docs/device_model.json)**.

| | |
|---|---|
| Physical layer | RS-485 half duplex, 19200 8N1 |
| Function codes | `0x03`, `0x04` read · `0x06`, `0x10` write |
| Poll | one FC4 transaction, 39 registers, 10 Hz |
| Float encoding | float32, **high word first** (PowerLogic convention) |
| Addressing | protocol (0-based) |

### Tested without any RS-485 hardware

The protocol code knows nothing about UARTs — you hand it a frame and a reply
buffer. So a master and a slave can talk **through memory**, and the whole
request/response cycle is verified on a PC: CRC, framing, float word order, block
reads, exceptions, broadcast suppression, and the retry path.

18 of the 52 unit tests cover Modbus. The ones worth knowing about:

| Test | Why it exists |
|---|---|
| `float_word_order_is_high_word_first` | Swapped words is *the* classic Modbus bug — every float becomes a wild magnitude. Known-answer test: `1.0f` must encode as `0x3F80,0x0000`. |
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
  the *previous* question gets parsed as the answer to this one — plausible but
  wrong data, the worst failure mode available.
- **`sample_seq` is published** so a master can tell "steady load" from "the meter
  has silently stopped sampling". Reading power alone cannot distinguish those.

### Discovery instead of hard-coding

The gateway sweeps unit IDs 1–16 at startup and asks each "who are you?". Whoever
answers with the right vendor ID is added to the poll list. Adding a second meter
is a DIP-switch change, not a code change. It also reads the `SIMULATED`
capability bit, so the gateway knows whether the data is real.

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

That **2.57× peak-to-mean ratio is the "before" number**, measured rather than
estimated. Phase 3's job is to shrink it by deferring the charger into the slack
before its deadline.

Firmware footprint:

| Node | RAM | Flash |
|---|---|---|
| meter (simulated) | 9.5% (31.1 kB) | 24.3% (318 kB) |
| gateway (Modbus + MQTT) | 22.3% (72.9 kB) | 33.4% (438 kB) |

---

## Phase 3 results — the deadline-aware controller

An identical EV-charging workload run twice: once with the controller
disabled, once with it active. Same loads, same profile, same random seed. The
only difference is whether the scheduler's output is allowed to drive the
relays.

```bash
pio run -e native-site -d meter_node
./meter_node/.pio/build/native-site/program
```

```
                        baseline   controlled
peak demand (window)      62.20 W     25.97 W
peak instantaneous       106.67 W    107.02 W
mean power                 22.97 W     20.26 W
charge delivered           1.506 Wh    1.506 Wh  (needed 1.50)
deadline misses                 0           0
sheds / restores                -      18 / 17  (1 forced)
thermal runtime met        100.0%       68.4%

peak demand reduced     58.2%
deadline misses         0
```

![before and after](docs/captures/site_control.png)

**The bottom panel is the point.** Both runs deliver exactly the same charge.
The controller did not save energy or shorten the job — it moved *when* the
energy was drawn, and the car still departed full.

### Three things in that table worth noticing

**Instantaneous peak barely moved** (106.7 → 107.0 W), and that is correct. A
demand charge is set by the 15-minute *average*, not the instant. A controller
that chased instantaneous spikes would shed constantly for events that cost
nothing. The number that fell is the one that is billed.

**The energy figure dropped, and that is not a saving.** The charger delivered
its full job in both runs; the difference is thermal runtime the controller
withheld (100% → 68.4%). A real thermal load claws that back afterwards —
*rebound* — which this model does not simulate. Reporting it as a saving would
be dishonest, so the program prints the service cost explicitly.

**`deadline misses = 0` is the claim the project rests on**, and it is asserted
in the test suite as well as measured here.

---

## Roadmap

- [x] **Phase 0** — INA226 sampling, telemetry ring buffer, energy accumulation, replayable logs
- [x] **Phase 1** — Modbus-RTU slave + master over RS-485, register map, device model, bus discovery
- [x] **Phase 2** — MQTT uplink, live dashboard, **store-and-forward** buffering, SNTP clock, LWT
- [x] **Phase 3** — **deadline-aware shed controller**: rolling demand window with projection, per-load energy + deadline, slack-ordered shedding, staggered restore, equipment protection
- [ ] **Phase 3.5** — Modbus-TCP gateway (transparent RTU↔TCP routing by unit ID)
- [ ] **Phase 4** — full FreeRTOS task split, MQTT over TLS, security notes against **IEC 62443**
- [ ] **Phase 5** — **NILM**: identify loads from one meter, *and* verify commanded sheds actually took effect

---

## Limitations & future work

- Low-voltage DC testbed only; no mains metering, and no AC concerns (power
  factor, harmonics, true RMS) are modelled.
- **The INA226 driver and the RS-485 transport are written from the datasheets
  and have never run on real silicon.** The protocol logic above them is tested;
  the two layers that touch hardware are not. See the bring-up checklist at the
  end of [docs/MODBUS_REGISTERS.md](docs/MODBUS_REGISTERS.md).
- **The MQTT and SNTP paths are compile-verified and their buffering logic is
  unit-tested, but they have not run against a live broker.**
- **MQTT has no authentication or TLS.** The bench broker runs
  `allow_anonymous true`, fine on a laptop behind a home router and unacceptable
  on anything exposed. Phase 4 addresses this.
- Buffer state for deferrable loads will be **modelled, not measured** — there is
  no temperature or state-of-charge sensor on the bench. The interface is
  designed so a real sensor drops in.
- Readings come from the simulator unless built with `esp32dev`; accuracy against
  a reference multimeter is still to be measured.
- Energy is integrated from boot and is not persisted across resets.
- The dashboard holds ~4 minutes in memory. There is no historical store.
- Cost and CO2 use flat assumed rates — ₹8/kWh and 710 gCO2/kWh. Both are
  assumptions, not measurements. Time-of-use pricing arrives with Phase 3.

Next after the roadmap: Modbus-TCP and BACnet, OT-security hardening, and
extending the scheduler to supply-side resources (battery, on-site PV), which is
what the commercial products reach for before they ever shed a load.
