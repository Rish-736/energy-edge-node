# Interview notes — Intelligent Energy Edge Node

Everything built, the tech stack, the reasoning behind each decision, and the
questions you will actually be asked.

**Status:** Phases 0-3 complete. 52 automated tests passing. ~3,300 lines of
firmware. **Measured result: peak demand cut 58.2% with zero deadline misses.** Runs against a simulator; the layers that touch physical hardware
are written but not yet validated on silicon.

---

## 0. What your resume claims — and why it is all still true

Your resume says:

> *"Building an EcoStruxure-style energy node (metering device → edge gateway →
> dashboard): measures power, shares readings over industrial Modbus-RTU/RS-485,
> and auto-sheds non-critical loads to cut peak demand and cost. Done:
> power/energy sensing, a Modbus-RTU slave with a documented register map, and
> an auto-discovering RS-485 gateway (24 tests). In progress: MQTT dashboard +
> load-shedding controller. Safe DC testbed."*

| Claim | Status |
|---|---|
| EcoStruxure-style, three tiers | true, unchanged |
| Measures power, Modbus-RTU/RS-485 | true, unchanged |
| **Auto-sheds non-critical loads to cut peak demand and cost** | **true** — still exactly what it does. The project now additionally *guarantees* each deferred load meets its deadline. |
| Sensing + Modbus slave + auto-discovering gateway, 24 tests | true — **now 34 tests** |
| In progress: MQTT dashboard | **the dashboard is now finished** |
| In progress: load-shedding controller | **now complete** — Phase 3 |
| Safe DC testbed | true |

**Nothing needs retracting.** If it comes up: *"since I sent that, the MQTT
dashboard is complete and the test count is 34."* Being ahead of your resume is
a good position.

---

## 1. The pitch

### 30 seconds

> "It's an energy node for a site that pays demand charges. It measures power,
> publishes it over industrial Modbus-RTU on an RS-485 bus to an edge gateway,
> and the gateway automatically sheds non-critical loads to cut the site's peak
> demand — while guaranteeing every deferred load still gets the energy it needs
> by its deadline. I built it as three tiers, metering device, edge gateway and
> dashboard, which is the same layering EcoStruxure uses. Sensing, Modbus and
> the MQTT uplink with outage buffering are done and test-covered; the deadline
> controller is next. It's all on a safe low-voltage DC testbed."

### 2 minutes — add these

**The business reason.** Commercial bills have two parts: energy used, and a
**demand charge** set by the single highest 15-minute average in the month. At
an EV charging site the demand charge is reported to be **30–70% of the bill**.
One bad quarter-hour sets it.

**Why EV charging specifically.** A car plugged in at 19:00 and needed at 07:00
wants four hours of charge in a twelve-hour window — **eight hours of slack**.
The driver's requirement is not "charge now", it is "full by morning". Spend the
slack and nobody notices. That is what makes the deferral *free*.

**Why three tiers.** The shed decision lives on the edge gateway, not in the
cloud. Networks drop; if the decision lived upstream, a dropped link would mean
no protection exactly when you need it.

**What's actually done.** Power and energy measurement, a Modbus-RTU slave with
a documented register map, a master that discovers devices on the bus, and an
MQTT uplink that buffers through network outages and backfills on reconnect.
34 tests, all running on a PC.

**The honest gap.** The sensor driver and the RS-485 transport are written from
datasheets and have not run on real parts yet.

---

## 2. Tech stack

| Layer | Choice | Why that one |
|---|---|---|
| **Language** | C++ (embedded C idiom) | Firmware standard; no dynamic allocation in the hot path |
| **MCU** | ESP32 — dual-core Xtensa LX6, 240 MHz, 320 KB RAM, 4 MB flash | Dual-core lets the real-time sampler own a core away from WiFi; WiFi built in for Phase 2 |
| **RTOS** | FreeRTOS | Native to the ESP32 — tasks, priorities, critical sections |
| **Framework** | Arduino-ESP32 | Fast to work in, with full direct FreeRTOS API access |
| **Build** | PlatformIO, 5 environments | Host *and* target builds from one config — this is what makes host tests possible |
| **Tests** | Unity — 34, all host-native | Firmware logic runs on a PC, no flash cycle |
| **Sensor** | INA226 over I2C | 16-bit bidirectional V/I/P monitor with a hardware power register |
| **Fieldbus** | Modbus-RTU over RS-485 (MAX485) | The dominant industrial protocol — Schneider's primary one |
| **Uplink** | MQTT (PubSubClient) + SNTP | Standard IoT messaging; real UTC time |
| **Dashboard** | Plain HTML/JS, MQTT-over-websockets | Single file, no build step |
| **Tooling** | Python 3.13 + matplotlib | Log replay, plotting, register-map validation |
| **VCS** | git → GitHub | One commit per phase, messages explain reasoning |

**Code size:** ~2,600 lines firmware + shared · ~590 lines tests · ~580 lines
Python · ~1,100 lines docs

**Footprint:**

| Node | RAM | Flash |
|---|---|---|
| meter | 9.5% (31 KB) | 24.3% (318 KB) |
| gateway | 22.3% (73 KB) | 33.4% (438 KB) |

Knowing your own footprint is a detail embedded interviewers notice.

---

## 3. The journey of one reading — your whiteboard walkthrough

```
[loads] → [INA226] → [ring buffer] → [Modbus registers]
                           ↓                  ↓
                    [future: NILM]      RS-485 cable
                                              ↓
                                 [gateway: Modbus master]
                                              ↓
                                      [ring buffer #2]
                                              ↓
                 ┌────────────────────────────┼──────────────┐
                 ↓                            ↓              ↓
       [deadline controller]           [MQTT publish]  [cost / CO₂]
                 ↓                            ↓
            [relays]                     [dashboard]
```

1. Current flows through the INA226.
2. The meter reads it 10×/s over I2C.
3. Energy accumulated — watts integrated into watt-hours.
4. The sample goes into a ring buffer.
5. The Modbus task copies the newest values into numbered registers, then waits.
6. The gateway asks over RS-485: *"unit 1, registers 16–54."* One request, 39
   registers.
7. The gateway decodes it, stamps wall-clock time, pushes it into **its own**
   ring buffer.
8. The uplink task drains that buffer and publishes JSON over MQTT.
9. The dashboard draws the next point and updates cost and CO₂.
10. **(Phase 3)** The controller checks slack against deadlines → trips a relay
    → the load stops → the sensor sees the drop. **Loop closed.**

---

## 4. Phase 0 — sensing and telemetry

**The sensor sits behind a swappable interface.** `ina226_read()` with two
implementations — real chip and simulator — selected by one build flag. That is
why the whole system could be built before any part arrived, and it is simply how
testable embedded software is structured.

**The real driver** checks the TI manufacturer ID, configures 16-sample
averaging at 1.1 ms per channel (~35 ms per averaged reading, comfortably inside
the 100 ms period), and calibrates per datasheet §7.5. It reads the chip's
**power register** rather than computing V×I — the chip integrates both channels
over the same conversion window, which beats multiplying two separately sampled
values.

**The ring buffer — the decision everything hangs off.** 256 samples, one
writer, many readers, each with its own cursor.

- **The writer never blocks.** WiFi can hang for ten seconds; measurement
  continues on time.
- **Readers are independent.** Adding a consumer doesn't touch the sampler.
- **Lost data is counted, not hidden.** A reader more than 256 samples behind has
  been lapped; it fast-forwards and records exactly how many it lost.
- **The sample count is published last**, so a reader seeing the new count is
  guaranteed a filled slot.

**Energy done properly.** Trapezoidal integration over **real elapsed time** (not
the nominal 100 ms) into a **`double`** accumulator. Per-sample increments are
~1e-6 Wh; `float` has ~7 significant digits, so after a few hours it would start
rounding each increment away entirely and the counter would simply stop
advancing. Time deltas use unsigned subtraction, which survives the `millis()`
rollover at ~49.7 days.

**Three FreeRTOS tasks**, pinned deliberately:

| Task | Priority | Core | Why |
|---|---|---|---|
| `sampler` | 5 | 1 | A late sample corrupts the energy integral |
| `modbus` | 4 | 1 | A master is waiting on a wire with a timeout running |
| `logger` | 2 | 0 | Falling behind is survivable *and accounted for* |

Core 1 for real-time work because the ESP32's WiFi stack runs on core 0. The
sampler uses **`vTaskDelayUntil`**, not `vTaskDelay` — the latter waits *n* ticks
*from now*, so execution time accumulates as drift forever.

**Measured:** 1,800 samples, 10.01 Hz, **0 dropped**, 1.3284 Wh, **peak/mean
2.57×**.

---

## 5. Phase 1 — Modbus-RTU over RS-485 (the employable one)

**What Modbus is.** A request/response protocol from 1979, still dominant because
it is trivially simple and universally implemented. One master asks; slaves only
answer. Data lives in numbered 16-bit registers. **No start or end byte** — a
frame is delimited purely by 3.5 character times of *silence*. The receiver is a
timer, not a parser.

**What RS-485 is.** Two wires carrying a **differential** signal — noise hits both
equally and cancels, which is why it survives 1 km in electrically filthy
environments. **Half duplex**, so one device may drive the line at a time; the
MAX485's DE pin controls that.

**The key architectural move.** The protocol core knows nothing about UARTs:

```c
size_t modbus_slave_handle(slave, req, req_len, resp, resp_cap);
```

Three payoffs: a master and slave can talk **through memory** (all PC-testable —
in the test file the "RS-485 cable" is a function call); Modbus-TCP later reuses
all of it; and the hardware-dependent part is isolated to ~96 lines.

**The register map** — 39 registers in one request. Decisions worth defending:

- **Two conventions stated explicitly and enforced by tests:** addresses are
  **0-based (protocol)**, floats are **high word first** (PowerLogic convention).
  These cause most Modbus integration failures. `1.0f` must encode as
  `0x3F80,0x0000`.
- **Both float *and* scaled-integer blocks** — many PLCs have no float support,
  so real meters publish both.
- **Writes rejected everywhere except one command register.** Measurements are
  physical facts; a master that could overwrite them could make the device report
  a comfortable lie.
- **`sample_seq` published** so a master can tell "steady load" from "the meter
  silently stopped sampling". Reading power alone cannot distinguish those.
- **Scaled values clamp, never wrap.** A NaN cast to `int16_t` gives a
  *plausible-looking* wrong number; saturation at least looks wrong.

**Error handling.** Three exceptions (illegal function / address / value), and
**three cases that get no reply at all**: bad CRC (the frame may not even have
been for us, and answering noise makes a bad bus worse), another device's ID, and
broadcast (several slaves answering would collide on a half-duplex line).

**Discovery, not hard-coding.** The gateway sweeps unit IDs 1–16, asks each "who
are you?", and adds whoever answers with the right vendor ID. It also reads the
`SIMULATED` capability bit so it knows whether the data is real.

---

## 6. Phase 2 — MQTT, store-and-forward, dashboard

**The headline: store-and-forward.** WiFi drops. A naive publisher discards every
reading taken while down, leaving holes exactly when something interesting may
have happened.

Instead: readings accumulate in the ring buffer regardless, and **the publisher
is just a reader that refuses to advance its cursor while disconnected.** On
reconnect it drains the backlog oldest-first. That is what Schneider's Panel
Server does — its logging memory buffers while the cloud link is down and
publishes what it held once the connection returns.

**The implementation is ~60 lines**, because the ring buffer already had
independent cursors and overrun counting. The refactor that made it possible —
moving the telemetry layer into shared code — means the gateway's outage buffer
*is* the already-tested buffer the meter samples into.

**It states its own limit.** 512 samples, ~51 s. Longer is real loss, and a test
asserts `published + dropped` accounts for every sample the ring ever saw.

**Also in Phase 2:**
- **SNTP/UTC**, tracked as `base + millis()` delta rather than calling `time()`
  per sample — `time()` is second-resolution and can step *backwards* when SNTP
  corrects it, and a log whose time goes backwards is miserable to debug.
- **MQTT Last Will** — if the gateway vanishes, the broker publishes `offline`
  for it. Without it a dead gateway looks identical to an idle one.
- **Two decoupled tasks** — `poll` at fixed 10 Hz that never waits on the
  network, and `uplink` handling WiFi/SNTP/MQTT. Store-and-forward *falls out* of
  that split.
- **Credentials in a gitignored `secrets.h`**; the firmware still builds without
  it.

---

## 6b. Phase 3 — the deadline-aware controller

**The measured result**, from an identical workload run with and without the
controller:

```
                        baseline   controlled
peak demand (window)      62.20 W     25.97 W     -> 58.2% lower
charge delivered           1.506 Wh    1.506 Wh   -> identical
deadline misses                 0           0
thermal runtime met        100.0%       68.4%     -> the service cost
```

**It acts on a forecast, not the present value.** A demand charge is the
average over a billing interval. By the time the average crosses the limit the
interval is nearly over and shedding cannot pull it back. So it projects where
the interval will land:
`projected = (energy_so_far + power x time_remaining) / window`.
That also means shedding late is worth less — with little of the window left,
removing a load barely moves the average.

**Slack picks the load, not a priority list.**
`slack = (deadline - now) - (energy_remaining / rated_power)`.
Shed the load with the most slack; restore the one with the least. A load whose
slack hits zero becomes untouchable and is force-restored if already off. This
makes eligibility *dynamic*: priorities never change, but which load may be
shed right now changes second by second.

**Three numbers in that table worth explaining:**

- *Instantaneous peak barely moved* (106.7 -> 107.0 W) and that is correct —
  demand charges are set by the 15-minute average, not the instant. Chasing
  instantaneous spikes would shed constantly for events that cost nothing.
- *Energy dropped, and it is not a saving.* The charger delivered its full job
  both times; the difference is thermal runtime withheld. A real thermal load
  recovers that later (rebound), which the model does not simulate. Saying
  "58% lower peak AND 12% less energy" would be the dishonest version.
- *`deadline misses = 0`* is asserted in the test suite, not just observed once.

**Two real bugs the tests caught**, both worth telling:

1. A forced restore was **immediately undone** by the shed logic on the same
   step — the load cycled rapidly, defeating `max_shed_ms` entirely. On real
   hardware that destroys a compressor. Fixed with a recovery period.
2. Slack of *exactly* zero is a floating-point knife edge, so the scheduler
   shed a critical load on one step and protected it on the next. Fixed with a
   safety margin, which is also physically right — a job should finish early,
   not exactly on time.

Neither would have been visible from a demo. Both came from tests asserting on
outcomes over time rather than on single calls.

---

## 7. What makes this more than a copy — four pillars, graded honestly

**1. Deadline-guaranteed deferral.** Deadlines are hard constraints; peak is the
objective. "Zero deadline misses" is binary and verifiable.
*Honest status:* studied academically as departure-time-constrained charging;
essentially absent from vendor and hobby material, which talks about capacity
splitting. **Strong framing, not a novel algorithm.**

**2. NILM as actuation verification — the real contribution.** A relay is
open-loop. Contacts weld, channels get miswired, loads get unplugged — and the
controller believes it shed 40 W, believes the peak is handled, stops looking.
The spike happens and nothing notices. Phase 5 verifies the command landed using
**the meter already installed for metering**.
*Honest status:* a 2025 systematic review names this exact closed loop as a
requirement, and its own gap statement is that the components exist separately
with no full embedded deployment closing it.

**3. Service cost as a first-class metric.** Deadline misses reported alongside
kW and ₹.
*Honest status:* discipline, not novelty — but the 40% cold-water statistic below
is what happens when nobody does it.

**4. Retrofit-first.** One meter, ordinary relays, no cloud. Schneider's answer is
PowerTag — one wireless sensor per breaker.
*Honest status:* an accuracy-for-installation-cost trade, and installation cost
is the real deployment barrier.

---

## 8. How it is proven

**52 unit tests, all running on a PC** — no board, no flash cycle.

| Suite | Count | Covers |
|---|---|---|
| ring buffer | 6 | ordering, late readers, overrun accounting, reader independence |
| control | 18 | demand window + projection, slack, shed/restore rules, deadlines |
| Modbus | 18 | CRC, float word order, exceptions, silence cases, writes, retries |
| store-and-forward | 10 | outage buffering, ordered backfill, exact loss accounting |

**The six worth naming:**

| Test | What it prevents |
|---|---|
| `float_word_order_is_high_word_first` | The classic Modbus bug — every float a wild magnitude |
| `crc_of_frame_including_crc_is_zero` | Verifies algorithm *and* byte order in one assertion |
| `overrun_is_counted_not_hidden` | Silent data loss, which makes an energy log worthless |
| `bad_crc_is_silently_dropped` | Answering line noise and making a bad bus worse |
| `outage_beyond_buffer_reports_exact_loss` | published + dropped must equal everything produced |
| `write_to_measurement_register_is_rejected` | A master overwriting a measurement and reading back its own lie |

Plus `tools/check_device_model.py`, which fails loudly if the C header, the JSON
device model and the Markdown documentation ever disagree.

---

## 9. Questions you will be asked

**"Isn't shedding loads just trading someone's comfort for money?"**
*(The best question you can get — you have a real answer.)*
> "That's exactly why I changed the target. The usual demo sheds a domestic
> water heater, and the field data on that is bad — in a trial of 191 households
> on grid-controlled water heaters, about 40% reported cold water and two people
> quit. It fails because the occupant never declared a deadline, so there's no
> slack you can safely spend. An EV charger is the opposite: the car needs four
> hours of charge in a twelve-hour window, so there are eight hours of slack and
> the requirement is 'full by morning', not 'charge now'. I report deadline
> misses as a first-class metric so the service cost is visible, not assumed."

**"Have you run Modbus on real RS-485 hardware?"**
> "Not yet — the transceivers aren't here. The protocol layer is tested end to
> end with a master and slave exchanging frames in memory, so framing, CRC,
> exceptions and error handling are verified. What's untested is the physical
> transport, and I'd expect the first faults to be swapped A/B lines or DE
> timing."

**Better than a yes.** It shows you separate "my logic is correct" from "it works
on hardware" — the distinction embedded teams care about most.

**"Why a ring buffer instead of a queue?"**
> "A queue is consumed — each item removed by one reader. Here every consumer
> needs the same stream at a different rate: the logger wants every sample, the
> register map only the newest, the classifier the raw transients. And the key
> part is what happens to the *producer*: with queues, if one consumer stops
> reading its queue fills and the push either blocks — stalling the sampler — or
> fails, losing the sample for everyone. **With queues a stuck consumer can stall
> the producer; with a ring and cursors a stuck consumer only hurts itself.**"

**"What's actually yours, versus copied from EcoStruxure?"**
> "The architecture is deliberately theirs — three tiers, Modbus, a device model,
> store-and-forward. That's the point: it's the reference design. What's mine is
> the control objective and the verification. They shed by priority; I schedule
> against deadlines and report misses. And their answer to per-load visibility is
> PowerTag — one sensor per breaker — whereas I'm doing it from a single meter,
> and using that classifier to verify my own relay commands actually took effect."

**"Why 10 Hz when metering needs far less?"**
> "To catch switch-on transients — a motor's inrush lasts ~250 ms. Those are the
> fingerprints the disaggregation goal needs, so discarding them early would mean
> rewriting the data path later."

**"Why not mains?"**
> "Deliberate scoping. Mains metering is dangerous and adds nothing I can't learn
> at 12 V — the protocols, the control problem and the architecture are
> identical. What I don't get is AC's real difficulties: power factor, harmonics,
> true RMS. I'd want supervision before touching those."

**"What would you do differently?"**
> "Added real clock time from the start. Timestamps were milliseconds-since-boot
> — fine for a data path, unusable for billing, and the tariff windows need
> wall-clock time. It's fixed now, but it should have been Phase 0."

---

## 10. Concepts to be comfortable explaining

Interviews probe fundamentals, not your code.

**Modbus** — master/slave, registers, function codes, holding vs input registers,
exceptions, why a 1979 protocol is still everywhere (simple, universal,
royalty-free).

**RS-485** — differential signalling and why it rejects noise, half duplex, DE
control, 120 Ω termination at both ends, A/B polarity, common ground.

**FreeRTOS** — tasks, priorities, preemption, `vTaskDelay` vs `vTaskDelayUntil`,
critical sections, priority inversion, why you pin a real-time task to a core.

**I2C** — two wires, addressing, master/slave, why it's for inches not metres.

**Power vs energy** — watts is a rate; watt-hours is the integral, and the thing
you're billed for.

**Demand charge** — set by the highest 15-minute average in the month; 30–70% of
an EV site's bill; why that makes peak shaving valuable even when total energy is
unchanged.

**Rebound / payback** — deferring a thermal load doesn't save energy, it moves
it. When restored it runs harder to catch up. So this reduces *peak*, not
necessarily *consumption* — and only the first claim is being made.

**Staggered restore** — releasing several loads at once creates a brand-new peak,
including motor inrush. Real systems bring large loads back across several demand
periods.

**CRC vs checksum** — a CRC catches burst errors a simple sum misses.

**NILM** — inferring which loads are running from a single aggregate meter, using
each load's switching signature.

---

## 11. What is NOT done — say it before you're asked

| Gap | Status |
|---|---|
| INA226 driver on real silicon | Written from datasheet, **never run** |
| RS-485 transport on hardware | Written, **never run**; MAX485s not yet ordered |
| MQTT/SNTP against a live broker | Compile-verified, logic unit-tested, **not run** |
| Deadline controller | built and tested; **never run on real relays** |
| Buffer state for deferrable loads | Will be **modelled, not measured** — no temperature/SoC sensor |
| Accuracy vs reference multimeter | Not measured |
| TLS / authentication | Phase 4 |
| NILM | Phase 5 |

**Quote the peak reduction carefully.** 58.2% is real and reproducible, but it
is **from a simulated site, not hardware**. Say so: *"58% in a closed-loop
simulation of the site; the controller has not driven real relays yet."*
Quoting it without that qualifier is the one thing that could cost you the
room.

---

## 12. Things not to say

- ❌ "It's an EcoStruxure clone." → ✅ "It's structured on the same three-tier
  architecture, deliberately."
- ❌ Any peak-reduction or cost-saving percentage. Not built yet.
- ❌ "It works" about the RS-485 or sensor layers. Say "tested in simulation;
  hardware bring-up pending."
- ❌ Overstating the simulator. It is a stand-in *and* a NILM training rig —
  a strength — but it is not real sensor data, and the firmware flags every
  simulated sample for exactly that reason.
- ❌ Claiming FreeRTOS depth beyond what you built. You have used tasks,
  priorities, core pinning, `vTaskDelayUntil` and critical sections. You have not
  used queues, semaphores, mutexes or event groups — and you can explain *why*
  (single-ownership of the bus and the MQTT client removes the need for locking).
- ❌ Claiming NILM verification is novel research. Say: "the components exist in
  the literature; what's uncommon is closing that loop on the device itself."
