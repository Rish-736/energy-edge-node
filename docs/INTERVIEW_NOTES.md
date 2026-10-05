# Interview notes — Intelligent Energy Edge Node

Everything achieved in Phases 0 and 1, the tech stack, the reasoning behind
each decision, and the questions you will actually be asked.

Status as of 6 October 2026: **Phases 0 and 1 complete.** 24 automated tests
passing. ~2,000 lines of firmware. Running against a simulator; the two layers
that touch physical hardware are written but not yet validated on silicon.

---

## 1. The pitch

### 30-second version

> "It's a small-scale energy management node. It measures power, publishes the
> readings over industrial Modbus-RTU on an RS-485 bus, and will automatically
> switch off non-critical loads to cut a building's peak demand charge. I built
> it as three separate tiers — a metering device, an edge gateway that makes the
> control decisions locally, and a dashboard — which is the same layering
> EcoStruxure uses. Right now the metering and Modbus layers are done and
> test-covered; the demand-response controller is next. It's all on a safe
> low-voltage DC testbed rather than mains."

### 2-minute version

Add:

- **Why the problem matters.** Commercial electricity bills have two parts:
  total energy used, *and* a charge based on the single highest demand peak in
  the billing period. One bad fifteen minutes can set the charge for a whole
  month. So automatically flattening peaks is worth real money.
- **Why three tiers.** The control decision lives on the edge gateway, not in
  the cloud. If the network drops, load shedding keeps working. That is the
  entire reason industrial systems are layered this way.
- **What's genuinely done.** Power and energy measurement, a Modbus-RTU slave
  with a documented register map, and a master that discovers devices on the
  bus and polls them — with the protocol verified by 24 tests that run on a PC.
- **The honest gap.** The sensor driver and the RS-485 transport are written
  from datasheets and haven't run on real parts yet.

---

## 2. Tech stack

| Layer | Choice | Why this one |
|---|---|---|
| **Language** | C++ (C-style embedded idiom) | Standard for firmware; no exceptions, no dynamic allocation in the hot path |
| **MCU** | ESP32 (dual-core Xtensa LX6, 240 MHz, 320 KB RAM, 4 MB flash) | Already owned; dual-core lets the real-time sampler sit on its own core, and WiFi is built in for Phase 2 |
| **RTOS** | FreeRTOS | What the ESP32 runs natively — tasks, priorities, critical sections |
| **Framework** | Arduino-ESP32 | Fast to work in while still giving full, direct FreeRTOS API access |
| **Build system** | PlatformIO (5 environments) | Real project structure, dependency management, host + target builds from one config |
| **Test framework** | Unity (ThrowTheSwitch) | PlatformIO's default; runs the firmware's own logic natively on the PC |
| **Host compiler** | MinGW GCC | Compiles the portable code for PC-side tests |
| **Sensor** | INA226 over I2C | 16-bit bidirectional current/voltage/power monitor; more accurate than INA219 |
| **Fieldbus** | Modbus-RTU over RS-485 (MAX485) | The dominant industrial protocol; Schneider's primary one |
| **Tooling** | Python 3.13, matplotlib | Log replay, validation, plotting, register-map consistency checks |
| **Planned** | MQTT, TLS | Phase 2 uplink and Phase 4 security |
| **Version control** | git | — |

### Code size

| Part | Lines |
|---|---|
| Firmware + shared protocol code | ~2,030 |
| Unit tests | ~360 |
| Python tooling | ~340 |
| Documentation | ~490 |

### Firmware footprint (meter node, simulated build)

```
RAM    9.5%   (31,100 of 327,680 bytes)
Flash 24.3%  (317,980 of 1,310,720 bytes)
```

Knowing your own footprint is the kind of detail embedded interviewers notice.

---

## 3. What the system does, end to end

```
[ DC loads ]
     |                                   FLOOR 1 — meter_node (ESP32)
[ INA226 sensor ] --> [ telemetry ring buffer ] --> [ Modbus register map ]
                              |                            |
                       [ future: NILM ]              RS-485 two-wire bus
                                                           |
                                            FLOOR 2 — gateway_node (ESP32)
                                          [ Modbus-RTU master / poller ]
                                                           |
                        +----------------------------------+---------------+
                        |                     |                            |
            [ shed/restore control ]   [ MQTT publish ]          [ cost / CO2 ]
                        |                     |
                   [ relays ]         FLOOR 3 — dashboard
```

Floors 2 and 3's shaded parts are Phases 2–3. Floor 1 and the Modbus link are
built.

---

## 4. Phase 0 — sensing, telemetry, logging

### 4.1 Sensor abstraction with two backends

One interface, `ina226_read()`, with two implementations selected by a single
build flag (`-D USE_INA226_SIM`):

- `ina226_real.cpp` — the actual chip over I2C
- `ina226_sim.cpp` — a synthetic 12 V testbed

**Why it matters:** the entire system can be built and tested before any part
arrives, and swapping the sensor later touches exactly one file. This is the
standard way testable embedded software is structured — a hardware abstraction
layer with a fake backend.

**The real driver** (written from the datasheet):
- Verifies the manufacturer ID register (`0xFE` must read `0x5449`, "TI")
- Configures `0x4527` = 16-sample averaging, 1.1 ms conversion on both
  channels, continuous mode (~35 ms per averaged reading, comfortably inside
  the 100 ms sample period)
- Calibration per datasheet §7.5:
  `current_lsb = max_current / 2^15`, `cal = 0.00512 / (current_lsb × R_shunt)`
- Bus voltage LSB = 1.25 mV; power register LSB = 25 × current_lsb
- Reads the **power register** rather than computing V × I — the chip
  integrates both channels over the same conversion window, which beats
  multiplying two separately sampled values

### 4.2 The telemetry ring buffer — the core design decision

A fixed 256-sample circular buffer. One writer (the sampler), many readers,
**each holding its own cursor**.

```c
typedef struct {
    telemetry_sample_t buf[256];
    volatile uint32_t  write_seq;   // total samples ever written
} telemetry_ring_t;

typedef struct {
    const char *name;
    uint32_t    next_seq;    // this reader's own position
    uint32_t    overruns;    // samples it lost to being lapped
} telemetry_reader_t;
```

**Four properties, each deliberate:**

1. **The writer never blocks or fails.** It stamps a monotonic sequence number,
   drops the sample at `seq & 255`, and increments. The sampler cannot be
   stalled by any consumer — if WiFi hangs for ten seconds, measurement
   continues on time.
2. **Readers are independent.** Adding a consumer is a local change. The
   sampler is not modified and existing readers cannot tell anyone joined.
3. **Data loss is counted, never hidden.** If a reader falls more than 256
   samples behind, it has been lapped. The read fast-forwards to the oldest
   surviving sample and adds the exact number lost to `overruns`. Silent loss
   is what makes an energy log untrustworthy.
4. **`write_seq` is published last.** A reader that sees the new count is
   guaranteed the slot behind it is already filled.

**Why not a FreeRTOS queue per consumer?** A queue is consumed — each item is
removed by one reader. Here every consumer needs *the same* stream, at
different rates, with different history requirements. And a full queue blocks
or drops at the producer, which is exactly what must not happen.

**Why not just keep the latest value?** Phase 5 (NILM) needs the fast raw
stream including switch-on transients. The dashboard needs one reading per
second. Keeping only the latest would make the stretch goal a rewrite.

**Cross-core safety:** the sampler is pinned to core 1 and the logger to core
0, so a struct copy is genuinely not atomic. Copies happen inside a
`portMUX` critical section — a few hundred nanoseconds. Off-target the lock
compiles to nothing, which is how the identical code runs under PC tests.

### 4.3 Energy accumulation

```c
s_wh += 0.5 * (last_p + power_w) * (dt_ms / 3600000.0);
```

- **Trapezoidal rule**, not rectangular — averages the two endpoints over the
  interval.
- **Real elapsed time**, not the nominal 100 ms. If a task overruns or the
  scheduler is busy, the energy figure stays honest rather than drifting.
- **`double` accumulator.** Per-sample increments are around 1e-6 Wh; float32
  has ~7 significant digits, so after a few hours the increments would start
  rounding away entirely.
- **Unsigned subtraction** for `dt`, which survives the `millis()` 32-bit
  rollover at ~49.7 days.

### 4.4 FreeRTOS task structure

| Task | Priority | Core | Job |
|---|---|---|---|
| `sampler` | 5 | 1 | sensor → energy → ring |
| `modbus` | 4 | 1 | ring → registers → RS-485 |
| `logger` | 2 | 0 | ring → serial CSV |

- The sampler is highest because a late sample corrupts the energy integral.
- Modbus sits above the logger because a master is waiting on the far end of a
  wire with a timeout running.
- The sampler uses **`vTaskDelayUntil`**, not `vTaskDelay`. `vTaskDelay` waits
  *n* ticks *from now*, so execution time accumulates as drift;
  `vTaskDelayUntil` targets an absolute wake time and holds the rate.

### 4.5 The appliance simulator

Three loads on a 12 V bus with deliberately distinct signatures:

| Load | Steady | Signature | Models |
|---|---|---|---|
| lights | 5 W | clean step, no transient | LED strip |
| heater | 40 W | ~6% droop as resistance rises with temperature | power resistor |
| motor | 18 W | 3.5× inrush decaying over ~250 ms, then 7 Hz ripple | small DC motor |

Plus bus sag under load (`V = 12.0 − 0.18·I`) and a seeded LCG for noise, so
every run is byte-identical and a regression is always a real regression.

**It earns its keep twice.** Each sample is stored with a `truth` bitmask of
which loads were actually on. That is exactly the vector a NILM classifier
outputs, so Phase 5 can be scored against known ground truth before it ever
meets a real appliance. The simulator is the training rig, not scaffolding.

### 4.6 Replayable logging

```
# energy-edge-node telemetry v1
# fields: seq,t_ms,v_v,i_a,p_w,wh,flags,truth
T,0,0,11.999,0.0000,0.000,0.000000,128,0
```

Data rows are prefixed `T,` so a capture survives being mixed with boot
chatter and `#` notes on one serial line. `tools/replay.py` parses, validates
and plots it — and independently verifies the sequence numbers are contiguous,
confirming from the data itself that nothing was dropped.

### 4.7 Phase 0 measured results

180-second capture, 1,800 samples:

```
rate         10.01 Hz
dropped      0
sensor fail  0
voltage      10.97 .. 12.01 V
power        0.00 .. 68.17 W   (mean 26.57)
energy       1.3284 Wh
peak/mean    2.57x
```

**That 2.57× is the "before" number** Phase 3 exists to shrink. Measured, not
guessed — so the eventual improvement will be a real figure.

---

## 5. Phase 1 — Modbus-RTU over RS-485

### 5.1 What Modbus-RTU is

A request/response protocol from 1979, still dominant in industrial equipment
because it is trivially simple and universally implemented.

- One **master** asks; **slaves** only answer. A slave never speaks unsolicited.
- Data lives in numbered 16-bit **registers**.
- Frame: `[unit_id][function_code][payload][crc_lo][crc_hi]`
- **No start or end byte.** A frame is delimited purely by silence — 3.5
  character times of idle line. So the receiver is a timer, not a parser.

**Function codes implemented:** `0x03` read holding, `0x04` read input,
`0x06` write single, `0x10` write multiple.

### 5.2 What RS-485 is

The electrical layer underneath. Two wires (A and B) carrying a **differential**
signal — noise hits both equally and cancels at the receiver, which is why it
survives 1 km runs in electrically filthy environments.

**Half duplex:** one pair, both directions, so exactly one device may drive the
line at a time. The MAX485's **DE** (driver enable) pin controls that — high to
talk, low to listen. Leave DE high and you jam the whole bus.

### 5.3 Transport-agnostic design — the key architectural move

The protocol core knows nothing about UARTs, RS-485 or the ESP32. You hand it a
received frame and a buffer for the reply:

```c
size_t modbus_slave_handle(modbus_slave_t *s,
                           const uint8_t *req, size_t req_len,
                           uint8_t *resp, size_t resp_cap);
```

**Three payoffs:**

1. A master and a slave can talk **through memory**, so the entire
   request/response cycle is verified on a PC with no hardware. In the test
   file, the "RS-485 cable" is a function call.
2. Modbus-TCP later reuses all of it — TCP differs only in framing (an MBAP
   header instead of a CRC).
3. The hardware-dependent part is isolated to ~96 lines in `rs485.cpp`.

### 5.4 The CRC

CRC-16/MODBUS: polynomial `0xA001`, initial value `0xFFFF`, processed
least-significant-bit first. Computed bitwise rather than from a lookup table —
at these baud rates it is nowhere near a bottleneck, and it keeps 512 bytes of
flash free.

**Byte order gotcha:** the CRC goes on the wire **low byte first**, which is the
opposite of every other 16-bit field in Modbus (those are big-endian). That
inconsistency is in the spec.

**Tested via a property:** running the checksum over a frame that already has
its CRC appended must yield zero. That single assertion verifies the algorithm
*and* its byte order.

### 5.5 The register map

Full detail in [MODBUS_REGISTERS.md](MODBUS_REGISTERS.md). Structure:

| Block | Addresses | Contents |
|---|---|---|
| Identity | `0x0000`–`0x0004` | vendor ID, product ID, firmware version, map version, capability bits |
| Float32 measurements | `0x0010`–`0x0017` | V, I, P, Wh — 2 registers each |
| Scaled integers | `0x0020`–`0x0024` | same four values as fixed-point |
| Status | `0x0030`–`0x0036` | flags, truth mask, sample sequence, uptime, overruns |
| Control | `0x0040` | write 1 to reset the energy counter |

**Decisions worth defending:**

- **Two conventions stated explicitly.** Addresses are **protocol (0-based)**,
  and float32 is **high word first** (PowerLogic convention). These two details
  cause most Modbus integration failures. Both are enforced by tests — `1.0f`
  must encode as `0x3F80,0x0000`.
- **Measurements in both FC3 and FC4.** FC4 (input registers) is semantically
  correct for read-only measured values, but many masters only implement FC3.
  Real meters answer both; so does this.
- **Both float and scaled-integer blocks.** Plenty of PLCs and budget HMIs have
  no float support. Real meters publish both for exactly that reason.
- **Scaled values are clamped, not wrapped.** A NaN or sensor fault saturates
  rather than becoming a plausible-looking wrong number.
- **Reserved addresses inside the span read as 0.** So one FC4 transaction
  fetches all 39 registers of measurements and status instead of four separate
  round trips. Addresses outside the span still raise exception `0x02`.
- **Writes are rejected everywhere except the command register.** Measurements
  are physical facts; a master that could overwrite them could make the device
  report a comfortable lie.
- **`sample_seq` is published.** If the counter stops advancing between polls,
  the meter is alive on the bus but has stopped sampling. Reading the power
  value alone cannot reveal that, because a frozen value and a genuinely steady
  load look identical.
- **`overruns` is published.** The slave reports its own internal data loss,
  making an otherwise invisible fault visible from outside the device.

### 5.6 Error handling

**Exceptions returned:** `0x01` illegal function, `0x02` illegal data address,
`0x03` illegal data value.

**Three cases get no reply at all** — correct Modbus behaviour:

| Case | Why silence |
|---|---|
| Bad CRC | The frame may not even have been addressed to us, and answering corrupted traffic turns line noise into bus traffic |
| Another device's unit ID | Not our conversation |
| Broadcast (unit 0) | The write applies, but several slaves answering at once would collide on a half-duplex line |

### 5.7 RS-485 transport — three things that are easy to get wrong

1. **DE is dropped only after `flush()`.** The UART must finish shifting out the
   last stop bit before the transceiver stops driving the line. Drop it early
   and the end of every frame is truncated, which appears as a CRC error at the
   far end. Classic bring-up fault.
2. **Framing is a timer.** At 19200 baud, t₃.₅ ≈ 2.0 ms. The receiver
   timestamps each byte and declares the frame complete after that gap. Above
   19200 baud the spec stops scaling and fixes the gap at 1.75 ms, because the
   computed value gets too small to measure reliably.
3. **The input buffer is flushed before each request.** Otherwise a late reply
   to the *previous* question gets parsed as the answer to this one — producing
   plausible but wrong data, the worst available failure mode.

### 5.8 The master, and why it discovers rather than hard-codes

At startup the gateway sweeps unit IDs 1–16 and asks each "who are you?".
Whoever answers with the right vendor ID is added to the poll list.

- Adding a second meter is a DIP-switch change, not a code change.
- It reads the `SIMULATED` capability bit, so the gateway knows whether it is
  looking at real measurements or synthetic ones. Data that silently claims to
  be real when it is not is worse than no data.
- This is how real commissioning tools behave.

**Link health is measured, not assumed.** Every transaction is timed; every
failure is classified as timeout, CRC error, or exception — **per device**,
because on a real bus one bad cable run fails while the others are fine, and an
aggregate counter would hide exactly that.

**Retries:** 2, with a 100 ms timeout. A single lost frame on a noisy bus is
normal; treating it as device failure would make the system flap. Exceptions
are *not* retried — the slave understood and said no, so asking again gets the
same answer.

### 5.9 One definition of the register map

`common/modbus/` is compiled into **both** firmwares, so the map has exactly
one definition and the two ends of the bus cannot drift apart.

The map also exists as `docs/device_model.json` (machine-readable) and
`docs/MODBUS_REGISTERS.md` (for humans). Three copies of one truth drift apart
eventually — so `tools/check_device_model.py` fails loudly when they disagree,
checking addresses against the C header, overlaps, poll-block coverage, and
that every address is documented.

**The device model is the semantic layer.** A raw register holding `17260`
means nothing; a register described as `{name: power, unit: W, type: float32}`
means something. Schneider's Power Monitoring Expert works this way — devices
are modelled and the software builds its tags from the model rather than from
hard-coded addresses. The payoff is that adding a meter is a data change.

---

## 6. Testing

**24 tests, all running natively on a PC.** No board, no flash cycle.

| Suite | Count | Covers |
|---|---|---|
| `test_ring` | 6 | FIFO order, sequence stamping, late-reader behaviour, overrun accounting, reader independence |
| `test_modbus` | 18 | CRC, float word order, block reads, exceptions, silence cases, writes, retry path |

**The six worth naming in an interview:**

| Test | What it prevents |
|---|---|
| `float_word_order_is_high_word_first` | The classic Modbus bug — swapped words make every float a wild magnitude |
| `crc_of_frame_including_crc_is_zero` | Verifies the CRC algorithm *and* its byte order in one assertion |
| `overrun_is_counted_not_hidden` | Silent data loss, which makes an energy log worthless |
| `bad_crc_is_silently_dropped` | Answering line noise and making a bad bus worse |
| `broadcast_write_applies_but_is_not_answered` | Bus collisions on a half-duplex line |
| `write_to_measurement_register_is_rejected` | A master overwriting a measurement and reading back its own lie |

### Five build environments

| Environment | Runs on | Purpose |
|---|---|---|
| `esp32dev-sim` | ESP32 | Simulated loads, no sensor needed |
| `esp32dev` | ESP32 | Real INA226 over I2C |
| `native-sim` | PC | Whole pipeline on the host, emits a capture |
| `native` | PC | The 24 unit tests |
| gateway `esp32dev` | ESP32 | Modbus master |

---

## 7. Numbers to have ready

| | |
|---|---|
| Sampling rate | 10 Hz |
| Ring buffer | 256 samples ≈ 25.6 s of history |
| Modbus baud | 19200, 8N1 |
| Inter-frame gap (t₃.₅) | ~2.0 ms |
| Poll | one FC4 transaction, 39 registers |
| Master timeout / retries | 100 ms / 2 |
| Bus scan range | unit IDs 1–16 |
| Measured sample rate | 10.01 Hz, 0 dropped over 1,800 samples |
| Peak-to-mean baseline | **2.57×** |
| RAM / Flash | 9.5% / 24.3% |
| Tests | 24, all passing |
| Firmware size | ~2,030 lines |

---

## 8. Questions you will be asked

**"Have you actually run Modbus on real RS-485 hardware?"**
> "Not yet — the transceivers are on order. The protocol layer is written and
> tested end to end with a master and slave exchanging frames in memory, so
> framing, CRC, exceptions and error handling are verified. What's untested is
> the physical transport. I expect the first faults to be swapped A/B lines or
> DE timing."

This answer is *better* than a yes. It shows you distinguish "my logic is
correct" from "it works on hardware" — the distinction embedded teams care
about most, and the one most candidates blur.

**"Why a ring buffer instead of a queue?"**
> "A queue is consumed — each item is removed by one reader. Here every
> consumer needs the same stream at a different rate. The logger wants every
> sample; the Modbus register map only wants the newest; a future classifier
> wants the raw transients. Per-reader cursors give all three from one buffer,
> and a full queue would block or drop at the producer, which is exactly what
> must not happen to the sampler."

**"What happens if a consumer falls behind?"**
> "It gets lapped and we count exactly how many samples it lost, then resync to
> the oldest surviving one. The count is published over Modbus, so a master can
> see the slave is losing data internally. Silently dropping samples is what
> makes an energy log untrustworthy."

**"Why 10 Hz?"**
> "Metering and Modbus need far less. 10 Hz is there to catch switch-on
> transients — a motor's inrush lasts about 250 ms. Those transients are the
> fingerprints the disaggregation stretch goal needs, so throwing them away
> early would mean rewriting the data path later."

**"Why did you not use mains?"**
> "Deliberate scoping. Mains metering is genuinely dangerous and adds nothing I
> can't learn on a 12 V bench supply — the protocols, the control problem and
> the architecture are identical. What I don't get is AC's real difficulties:
> power factor, harmonics, true-RMS. I'd want supervision before touching those."

**"Walk me through what happens between the sensor and the dashboard."**
Use the diagram in §3 and the seven numbered steps in
[ARCHITECTURE.md](ARCHITECTURE.md#4-how-the-data-actually-travels).

**"How is this related to what we do?"**
> "It's laid out as three tiers deliberately: a metering device, an edge
> gateway that makes the control decisions locally, and a dashboard — which
> maps onto Connected Products, Edge Control and Apps/Analytics. The reason
> the control logic sits on the gateway rather than upstream is edge autonomy:
> unplug the network and load shedding keeps working."

**"What would you do differently?"**
> "I'd have added real clock time from the start. Timestamps are currently
> milliseconds since boot, which is fine for a data path but unusable for
> billing or tariff windows — and the demand-response controller needs
> wall-clock time to know when peak hours are. It's a Phase 2 fix, but it
> should have been Phase 0."

---

## 9. Concepts to be comfortable explaining

Interviews probe fundamentals, not your code.

**Modbus** — master/slave, registers, function codes, holding vs input
registers, exceptions, why a 1979 protocol is still everywhere (simple,
universal, royalty-free).

**RS-485** — differential signalling and why it rejects noise, half duplex, DE
control, 120 Ω termination at both ends, A/B polarity, common ground.

**FreeRTOS** — tasks, priorities, preemption, `vTaskDelay` vs
`vTaskDelayUntil`, critical sections, what priority inversion is, why you pin a
real-time task to a core.

**I2C** — two wires, addressing, master/slave, why it's for inches not metres
(as opposed to RS-485).

**Power vs energy** — watts is a rate; watt-hours is the integral, and the
thing you're billed for.

**Peak demand billing** — why the highest 15-minute average in a month can
dominate a commercial bill, and why that makes shedding valuable.

**CRC vs checksum** — a CRC catches burst errors a simple sum misses.

**NILM** — inferring which appliances are running from a single aggregate
meter, using each load's switching signature.

---

## 10. What is NOT done — say this before you're asked

| Gap | Status |
|---|---|
| INA226 driver on real silicon | Written from datasheet, **never run** |
| RS-485 transport on real hardware | Written, **never run**; MAX485s on order |
| Accuracy vs reference multimeter | Not measured |
| Demand-response controller | **Phase 3 — not built** |
| MQTT and dashboard | **Phase 2 — not built** |
| Real clock time (SNTP) | Not done; timestamps are ms-since-boot |
| Energy persistence across reset | Not done |
| TLS / security hardening | Phase 4 |
| NILM | Phase 5 stretch goal |

**Never claim a peak-reduction percentage.** The controller doesn't exist yet,
so there is no number. You have a measured *baseline* (2.57× peak-to-mean) and
that is the honest thing to quote. An unverifiable figure is the fastest way to
lose a technical interviewer.

---

## 11. Roadmap, and where it goes beyond the reference architecture

| Phase | Adds |
|---|---|
| 2 | MQTT, dashboard, **store-and-forward buffering**, real clock time |
| 3 | Demand response: **rolling demand window**, tariff schedule, hysteresis, load priority, alarms |
| 3.5 | Modbus-TCP gateway (transparent RTU↔TCP routing by unit ID) |
| 4 | Full FreeRTOS task split, TLS, security notes against **IEC 62443** |
| 5 | **NILM** — identify appliances from one sensor |

**Two deliberate differentiators:**

**Store-and-forward (Phase 2).** Readings buffer during a network outage and
backfill when it returns. The demo is ten seconds: unplug WiFi, the dashboard
flatlines, plug it back in, the gap fills in retroactively. This is the feature
that separates a product from a toy — toys lose data. The ring buffer already
does most of the work.

**Rolling demand window (Phase 3).** Bills are based on a 15-minute average,
not the instant. So:

```
Naive:  if (power > threshold) shed();         ← a thermostat
Real:   track the rolling average, forecast
        where the window will close, shed
        early enough to stay under target
```

The second is what demand-control software actually does. It's the difference
between "I added an if-statement" and "I understand how electricity is billed."

**NILM (Phase 5).** Schneider's answer to "which load is running?" is PowerTag
— a wireless sensor clipped onto *each individual circuit breaker*. One sensor
per circuit. Phase 5 does it with **one** meter and a classifier.

Frame it as a trade-off, never as beating them: per-circuit sensing is more
accurate, and they sell hardware, so of course that's their answer. The point
is **installation cost and retrofit difficulty versus accuracy** — and retrofit
cost is the real barrier to energy monitoring getting deployed. Knowing which
trade-off you're making is the useful part.

---

## 12. Things not to say

- ❌ "It's an EcoStruxure clone." → ✅ "It's structured on the same three-tier
  architecture."
- ❌ Any peak-reduction or cost-saving percentage. Not built yet.
- ❌ "It works" about the RS-485 or sensor layers. Say "tested in simulation;
  hardware bring-up pending."
- ❌ Overstating the simulator. It's a stand-in *and* a NILM training rig —
  that's a strength, but it is not real sensor data, and the firmware flags
  every simulated sample for exactly that reason.
- ❌ Claiming FreeRTOS expertise beyond what you built. You've used tasks,
  priorities, core pinning, `vTaskDelayUntil` and critical sections. You have
  not used queues, semaphores, mutexes or event groups yet — those come in
  Phase 2.
