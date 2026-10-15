# Decisions and why

Every meaningful choice made in this project, what else was on the table, why
this one won, and what it cost.

**How to read this.** Each decision has an ID (`D1`, `D2`, …) so it can be
referenced from code reviews, commit messages or conversation. IDs are never
reused. A decision that gets reversed later is marked **SUPERSEDED** and kept,
because the reasoning that led to the wrong answer is often more instructive
than the right answer.

**If you only read one section:** D20 (the ring buffer), D33 (transport-agnostic
Modbus) and D50 (store-and-forward) shape everything built so far. D82-D85 are
the reframe that decided what the project is actually *for*.

Last updated: Phase 2 complete, plus the Phase 3 reframe (D82-D85).

---

## Part 1 — Project-level decisions

### D1 — Build a safe low-voltage DC testbed, not a mains meter
**Alternatives:** mains (230 V AC) metering; a CT-clamp on a live circuit.
**Chosen:** 12 V DC bench supply, ≤24 V ceiling.
**Why:** mains metering is genuinely dangerous and teaches almost nothing extra.
The protocols, the control problem, the architecture and the firmware are
*identical* at 12 V. The only thing removed is the risk of killing yourself.
**Trade-off accepted:** no AC-specific measurement — power factor, harmonics,
true-RMS, phase imbalance. Those are real skills this project does not develop.
**Framing:** in an interview this is a *strength* if stated as deliberate
scoping, and a weakness if it sounds like something you forgot.

### D2 — Model it on the EcoStruxure three-tier architecture
**Alternatives:** one ESP32 doing everything; a flat sensor→cloud design.
**Chosen:** three tiers — metering device, edge gateway, dashboard.
**Why:** the target employer is Schneider Electric, and this is their reference
architecture (Connected Products → Edge Control → Apps/Analytics). Building one
node per tier means the project *is* interview preparation. It also happens to
be good design: each tier can be replaced without touching the others.
**Trade-off accepted:** more code and more hardware than a single-board version.

### D3 — Put the control decision on the edge, not in the cloud
**Alternatives:** dashboard or a cloud function decides when to shed.
**Chosen:** the gateway decides, locally.
**Why:** this is the whole point of an "Edge Control" tier. Networks drop. If the
shedding decision lived upstream, a dropped link would mean no protection at all
— exactly when you least want it. It also gives a blunt, memorable demo: unplug
the network and watch it keep working.
**Trade-off accepted:** the gateway needs enough state and logic on-device; it
cannot be a dumb pipe.

### D4 — Two physical nodes rather than one
**Alternatives:** one ESP32 acting as both meter and gateway, talking to itself.
**Chosen:** separate meter and gateway boards.
**Why:** real buildings are built that way — meters are separate boxes from the
gateway that collects them. More practically, splitting them forces Modbus over
a *real cable*, which is the single most employable part of this project. One
board talking to itself over a loopback would prove nothing.
**Trade-off accepted:** two boards to flash, and a bus that can be miswired.

### D5 — Keep the raw sample stream flowing through one layer from day one
**Alternatives:** decimate early (publish 1 Hz, discard the rest), add the fast
path later when NILM needs it.
**Chosen:** every sample at full rate goes into one buffer that all consumers
read from.
**Why:** Phase 5 (NILM — identifying appliances from the aggregate signal) needs
the fast raw stream *including* switch-on transients. The dashboard only needs
~1 Hz, so throwing the rest away is very tempting early. Doing that would make
Phase 5 a rewrite of the entire data path rather than a new consumer.
**Trade-off accepted:** more RAM than strictly needed for Phases 0–3.

---

## Part 2 — Toolchain decisions

### D6 — PlatformIO over the Arduino IDE and over bare ESP-IDF
**Alternatives:**
- *Arduino IDE* — simplest to start, but no real project structure, awkward to
  version-control, and cannot run host-side tests.
- *ESP-IDF* — the most "industrial" option, native FreeRTOS, CMake, menuconfig.
  Strongest interview story, steepest learning curve.
- *PlatformIO* — real project layout, dependency management, multiple build
  environments including host builds, works inside existing VS Code.

**Chosen:** PlatformIO with the Arduino-ESP32 framework.
**Why:** it gives ~90% of the credibility of ESP-IDF for ~30% of the pain, and
critically it supports **host (`native`) build environments**. That single
feature is what makes 34 unit tests possible without a board, which in turn is
what let the whole project be written before any hardware arrived.
**Trade-off accepted:** "Arduino framework" sounds less serious than ESP-IDF to
some interviewers. The mitigation is D12 — using raw FreeRTOS APIs directly
rather than `loop()`, so the RTOS claims are true either way.

### D7 — Arduino-ESP32 framework, but do not use `loop()` for real work
**Chosen:** `setup()` creates FreeRTOS tasks; `loop()` only prints diagnostics.
**Why:** Arduino-ESP32 *runs on FreeRTOS underneath* — `loop()` is itself just a
task. So `xTaskCreatePinnedToCore`, queues, critical sections and core pinning
are all available and real. Writing the firmware as explicit tasks means
"structured as FreeRTOS tasks" is an honest claim, not a stretch.
**Trade-off accepted:** slightly more boilerplate than a single `loop()`.

### D8 — Unity as the test framework
**Alternatives:** GoogleTest; no tests; manual serial-output checking.
**Chosen:** Unity (ThrowTheSwitch).
**Why:** it is PlatformIO's default, it is C-friendly (this is C-style C++), and
it runs natively on the host. GoogleTest is more capable but is C++-heavy and
needs more setup for an embedded project.
**Trade-off accepted:** no mocking framework, no parameterised tests. Neither has
been needed — the code under test is deliberately dependency-light.

### D9 — Python for host tooling rather than C++ or a notebook
**Chosen:** Python 3.13 with matplotlib for `replay.py`,
`check_device_model.py`, `mock_publisher.py`.
**Why:** already installed, best-in-class for parsing and plotting, and keeping
the tools in a *different language* from the firmware proves the log format is
genuinely portable rather than accidentally coupled to the C structs.
**Trade-off accepted:** a Python dependency for anyone reproducing the plots.

### D10 — One commit per phase, with a long explanatory message
**Alternatives:** many small commits; one giant commit at the end.
**Chosen:** a phase-sized commit whose message explains the *reasoning*, not the
diff.
**Why:** the git log doubles as a project narrative a reviewer can read. "Added
files" messages would waste that.
**Trade-off accepted:** commits are large, so bisecting inside a phase is not
possible.

---

## Part 3 — Hardware decisions

### D11 — ESP32 over STM32
**Chosen:** ESP32 for both nodes.
**Why:** already owned; **dual-core**, so the real-time sampler can be pinned to
its own core away from WiFi; and WiFi/Bluetooth are built in, which Phase 2
needs. An STM32 would have meant adding a separate network module.
**Trade-off accepted:** the ESP32's WiFi stack is a large, somewhat opaque
dependency sharing the chip with real-time code — which is precisely why D33
(core pinning) matters.

### D12 — INA226 over INA219, and over a shunt + bare ADC
**Alternatives:**
- *Shunt resistor + the ESP32's own ADC* — cheapest, but the ESP32's ADC is
  noisy and non-linear, and you would have to do the voltage/current maths and
  calibration yourself.
- *INA219* — same family, 12-bit, less accurate.
- *INA226* — 16-bit, bidirectional, with a dedicated power register.

**Chosen:** INA226 (~₹150).
**Why:** 16-bit resolution, and critically a **hardware power register**. The
chip integrates voltage and current over the *same* conversion window, which is
more accurate than sampling them separately and multiplying. It also has
programmable averaging, which is free noise reduction.
**Trade-off accepted:** slightly more expensive than an INA219 and needs a
calibration register to be programmed correctly (see D14).

### D13 — MAX485 transceivers for the physical bus
**Alternatives:** plain UART between the boards (3.3 V TTL); SPI; I2C.
**Chosen:** MAX485 (RS-485).
**Why:** TTL UART between two boards would work electrically and be *completely
unrepresentative*. RS-485 is what real industrial buses use: differential,
noise-immune, multi-drop, kilometre-capable. Using it makes "I have worked with
Modbus-RTU over RS-485" a true statement, and that is the line Schneider's job
postings ask for.
**Trade-off accepted:** half-duplex, so the direction pin has to be managed in
software (see D28), and the bus can be miswired in ways that look like total
failure (see D31).

### D14 — INA226 configured for 16-sample averaging, 1.1 ms conversions
**Chosen:** config register `0x4527`.
**Why:** 16 averages at 1.1 ms per channel is ~35 ms per averaged reading, which
fits comfortably inside the 100 ms sample period while removing a lot of noise
for free. Averaging in the chip costs the MCU nothing.
**Trade-off accepted:** ~35 ms of smoothing slightly rounds the very fastest
transient edges. Still far faster than the 250 ms motor inrush that Phase 5
cares about.

### D15 — Modbus at 19200 baud, 8N1
**Alternatives:** 9600 (safer over long cable), 115200 (faster).
**Chosen:** 19200.
**Why:** it is the common industrial default, and it keeps the 3.5-character
inter-frame gap at a comfortable ~2 ms — easy to measure reliably in software.
Above 19200 the Modbus spec stops scaling that gap and fixes it at 1.75 ms
precisely because the computed value gets too small to time accurately.
**Trade-off accepted:** a 39-register reply takes a few milliseconds to
transmit. Irrelevant at a 10 Hz poll rate.

### D16 — 10 Hz sampling
**Alternatives:** 1 Hz (plenty for metering); 1 kHz (better transient capture).
**Chosen:** 10 Hz.
**Why:** metering and Modbus need far less, but 10 Hz is enough to catch
switch-on transients — a motor's inrush lasts ~250 ms, so 10 Hz puts 2–3 samples
inside it. Those transients are the fingerprints Phase 5 needs.
**Trade-off accepted:** 10 Hz is marginal for *fine* transient classification. If
Phase 5 needs more it can be raised; nothing in the design is rate-dependent,
which is the point of D5.

---

## Part 4 — Architecture decisions

### D17 — Put the sensor behind a HAL with two interchangeable backends
**Alternatives:** write only the real driver and wait for hardware; `#ifdef`
fake values inline in the sampling code.
**Chosen:** one interface (`ina226_read()`), two implementations
(`ina226_real.cpp`, `ina226_sim.cpp`), selected by `-D USE_INA226_SIM`.
**Why:** the entire system could be written and tested before any part arrived.
Swapping to real hardware later touches exactly one build flag. This is also
simply how testable embedded software is structured — a hardware abstraction
layer with a fake backend.
**Trade-off accepted:** one extra layer of indirection, and the risk of the
simulator and the real driver drifting apart in behaviour. Mitigated by D18.

### D18 — Make the simulator physically realistic, not a sine wave
**Chosen:** three loads with genuinely distinct electrical signatures — a clean
step (lights), a thermal droop as resistance rises (heater), and a 3.5× inrush
decaying over 250 ms followed by commutation ripple (motor). Plus bus sag under
load.
**Why:** a lazy simulator would make the whole stack *look* tested while hiding
every interesting case. More importantly, these behaviours are exactly what the
Phase 5 classifier will key off — so the simulator is the **training rig**, not
throwaway scaffolding. It earns its keep twice.
**Trade-off accepted:** more code than a stub, and the numbers are invented
rather than measured. Every simulated sample is flagged (D19) so it can never be
mistaken for real data.

### D19 — Flag simulated data everywhere it travels
**Chosen:** `TLM_FLAG_SIMULATED` on every sample, a `SIMULATED` capability bit in
the Modbus identity registers, and a `simulated` field in the MQTT status topic.
**Why:** data that silently claims to be real when it is not is worse than no
data. A master, a dashboard or a future reader of a log file can all tell at a
glance. It also keeps the project honest in an interview.
**Trade-off accepted:** a bit of plumbing in three places.

### D20 — A ring buffer with per-reader cursors, not a queue per consumer
**This is the most important decision in the project.**

**Alternatives:**
- *A FreeRTOS queue per consumer.* A queue is *consumed* — each item is removed
  by one reader. Here every consumer needs **the same** stream. You would need
  one queue per consumer and the producer would have to push to all of them,
  which means the producer blocks or drops when any single queue fills.
- *Just keep the latest value in a global.* Simplest, and loses all history, so
  the logger cannot log and Phase 5 cannot exist.
- *A mutex-protected array.* Readers would contend with the writer and with each
  other.

**Chosen:** one fixed 256-sample circular buffer; each consumer holds its own
cursor (`next_seq`) and reads independently.
**Why:**
- The **writer never blocks and never fails** — it stamps a monotonic sequence
  number, writes at `seq & mask`, and increments. A consumer stuck on WiFi for
  ten seconds cannot delay a single sample.
- **Readers are independent.** Adding a consumer is a local change; the sampler
  is untouched and existing readers cannot tell anyone joined.
- **Different consumers get different semantics from the same data.** The logger
  wants every sample; the Modbus register map fast-forwards to the newest
  because registers hold present values, not history; Phase 5 will want the raw
  transients. One buffer, three needs.
**Trade-off accepted:** a slow reader can be *lapped* and lose data. That is
handled explicitly rather than hidden — see D21.

### D21 — Count overruns rather than blocking or silently dropping
**Alternatives:** block the writer until the slowest reader catches up (D20 says
no); grow the buffer dynamically (no dynamic allocation in firmware); drop
quietly.
**Chosen:** if a reader falls more than `TELEMETRY_RING_SIZE` behind, fast-forward
it to the oldest surviving sample and add the exact number lost to `overruns`.
**Why:** silent data loss is what makes an energy log untrustworthy, and an
untrustworthy log is worth nothing. The count is also published over Modbus and
MQTT, so an internal fault becomes visible from outside the device.
**Trade-off accepted:** data really is lost in that case. The design states its
own limit instead of pretending to be lossless.

### D22 — Publish the sequence number, not just the values
**Chosen:** `sample_seq` is a Modbus register and an MQTT field.
**Why:** it lets a consumer distinguish "the load is steady" from "the meter has
silently stopped sampling". Reading the power value alone **cannot** tell those
apart — a frozen value and a genuinely constant load look identical. A monotonic
counter that stops advancing is unambiguous.
**Trade-off accepted:** four more bytes per sample.

### D23 — Move the telemetry layer into `common/` so both nodes share it
**Alternatives:** a second, gateway-specific buffer implementation; copy-paste.
**Chosen:** `common/telemetry/` (sample struct, ring buffer, forwarder) compiled
into both firmwares.
**Why:** the gateway's store-and-forward buffer has exactly the same
requirements as the meter's sampling buffer — one writer, independent readers,
counted overruns. Sharing means it was already tested the day it was needed, and
there is one definition of the sample format rather than two that can drift.
**Trade-off accepted:** `TELEMETRY_RING_SIZE` had to become a build-flag
override rather than a fixed constant, since the two nodes size it differently
(D41).

### D24 — One sample struct for both tiers
**Chosen:** `telemetry_sample_t` is produced by the meter from its sensor *and*
by the gateway from Modbus polls.
**Why:** it makes D23 possible, and it means a capture from either tier can be
fed to the same tools.
**Trade-off accepted:** the struct carries a `t_unix_ms` field the meter always
leaves zero (it has no clock) and a `truth` field that is zero on real hardware.
Both are documented as "0 means unknown", which is an honest sentinel rather
than an invented value.

---

## Part 5 — Firmware decisions

### D25 — Explicit FreeRTOS tasks with deliberate priorities
**Chosen (meter):** `sampler` prio 5, `modbus` prio 4, `logger` prio 2.
**Chosen (gateway):** `poll` prio 4, `uplink` prio 3.
**Why each number:**
- The sampler is highest because a late sample corrupts the energy integral —
  everything downstream derives from it.
- Modbus sits above the logger because a master is waiting at the other end of a
  wire with a timeout running; serial logging has nobody waiting.
- The logger is lowest because dropping behind is survivable and *accounted for*
  (D21).
**Trade-off accepted:** priorities are a static guess rather than a measured
schedule. Nothing has starved so far.

### D26 — Pin tasks to specific cores
**Chosen:** real-time work on core 1; network and logging on core 0.
**Why:** the ESP32's WiFi and Bluetooth stacks run on core 0. Pinning the
sampler to core 1 keeps it away from a large, unpredictable, interrupt-heavy
subsystem. This is the main reason D11 chose a dual-core part.
**Trade-off accepted:** manual placement rather than letting the scheduler
decide; cross-core data sharing now genuinely needs locking (D27).

### D27 — `portMUX` critical sections inside the ring buffer, no-op off-target
**Alternatives:** a FreeRTOS mutex; atomics; no locking at all.
**Chosen:** `portENTER_CRITICAL` / `portEXIT_CRITICAL` around the struct copy,
compiled away when not building for Arduino.
**Why:** with the writer on core 1 and readers on core 0, copying a ~40-byte
struct is genuinely not atomic — a reader could see half an old sample and half
a new one. A spinlock is right here because the critical section is only a struct
copy long (a few hundred nanoseconds); a mutex would add scheduler overhead and
could cause priority inversion. Making it a no-op off-target is what lets the
**identical** ring buffer code run under host tests.
**Trade-off accepted:** critical sections briefly disable interrupts on that
core. Acceptable at this duration.

### D28 — Drop the RS-485 direction pin only after `flush()`
**Chosen:** `digitalWrite(DE, HIGH)` → `write()` → **`flush()`** →
`digitalWrite(DE, LOW)`.
**Why:** `write()` only queues bytes into the UART's buffer. If DE goes low
before the UART has physically shifted out the final stop bit, the end of every
frame is truncated on the wire — which shows up at the far end as a CRC error on
every single message. This is one of the most common RS-485 bring-up faults.
`flush()` blocks until transmission actually completes.
**Trade-off accepted:** `rs485_send()` blocks for the duration of the frame
(~4 ms for a short frame at 19200). Fine inside a dedicated task.

### D29 — Trapezoidal integration, real elapsed time, `double` accumulator
**Alternatives:** rectangular integration (`power × nominal_period`); `float`
accumulator.
**Chosen:** `wh += 0.5 × (p_prev + p_now) × (dt_actual / 3600000)` into a
`double`.
**Why three separate choices:**
- **Trapezoidal** averages the two endpoints, which is strictly more accurate
  than assuming power was constant across the interval.
- **Real elapsed `dt`**, not the nominal 100 ms, so if a task overruns or the
  scheduler is busy the total stays honest instead of drifting.
- **`double`**, because per-sample increments are around 1e-6 Wh and `float` has
  only ~7 significant digits. After a few hours of accumulation a `float` would
  start rounding each increment away entirely — the counter would simply stop
  advancing.
**Trade-off accepted:** `double` maths is slower on the ESP32 (no hardware
double unit). At 10 Hz this is irrelevant.

### D30 — Unsigned subtraction for all time deltas
**Chosen:** `uint32_t dt = now - last;` with unsigned types throughout.
**Why:** `millis()` wraps to zero after ~49.7 days. Unsigned subtraction gives
the correct delta *across* that wrap; signed arithmetic would produce a huge
negative number and corrupt the energy total once every 50 days.
**Trade-off accepted:** none. This is free correctness.

### D31 — `vTaskDelayUntil` for the sampler, not `vTaskDelay`
**Chosen:** `vTaskDelayUntil(&last_wake, period)`.
**Why:** `vTaskDelay(n)` waits *n* ticks **from now**, so the time spent doing
the work is added on top of every period — the sample rate drifts slowly below
nominal forever. `vTaskDelayUntil` targets an absolute next-wake time and holds
the rate. The measured 10.01 Hz over 1,800 samples is the evidence.
**Trade-off accepted:** if a single iteration ever overruns the period, it
cannot catch up. Acceptable; the sampling path is a few hundred microseconds.

---

## Part 6 — Modbus decisions

### D32 — Write the Modbus stack rather than use a library
**Alternatives:** `eModbus`, `ArduinoModbus`, `modbus-esp8266`.
**Chosen:** write it (~300 lines of protocol core).
**Why:** three reasons, in order of importance.
1. **Interview value.** "I used a Modbus library" and "I implemented Modbus
   framing, CRC and exception handling" are very different sentences, and
   Modbus-RTU is Schneider's number-one protocol topic. The entire point of
   Phase 1 was to learn this.
2. **Testability.** No library gives you a transport-agnostic core you can drive
   from a unit test (D33).
3. **Size.** The implemented subset is four function codes; a general library is
   much larger and mostly unused.
**Trade-off accepted:** a hand-written protocol can be subtly wrong in ways a
battle-tested library would not be. Mitigated by 18 unit tests, including
known-answer tests for the two classic failure modes (D36, D38).

### D33 — Make the protocol core transport-agnostic
**Chosen:** `modbus_slave_handle(slave, req, req_len, resp, resp_cap)` — it takes
a frame and a reply buffer and knows nothing about UARTs, RS-485 or the ESP32.
**Why:** three payoffs, and this is the single best structural decision in
Phase 1.
1. A master and a slave can talk **through memory**, so the whole
   request/response cycle — framing, CRC, float packing, block reads,
   exceptions, broadcast suppression, retries — is verified on a PC before any
   transceiver exists. In the test file, the "RS-485 cable" is a function call.
2. Modbus-TCP (Phase 3.5) reuses all of it; TCP differs only in framing.
3. The hardware-dependent part shrinks to ~96 lines in `rs485.cpp`, which is the
   only code that needs real bring-up.
**Trade-off accepted:** the caller has to do the frame assembly and timing. That
is `rs485.cpp`'s entire job.

### D34 — Implement FC 3, 4, 6, 16 only
**Chosen:** read holding, read input, write single, write multiple.
**Why:** these four cover everything a meter needs. Coil functions (1, 5, 15)
are for discrete I/O this device does not have; diagnostics (8) and device
identification (43) are optional extras. Unsupported codes return a proper
`ILLEGAL_FUNCTION` exception rather than silence, so a master gets a useful
answer.
**Trade-off accepted:** a master expecting FC 43 for identification gets an
exception; the identity block (D35) serves the same purpose.

### D35 — Expose measurements through *both* FC3 and FC4
**Why:** FC4 (input registers) is the semantically correct function for
read-only measured values. But a lot of masters — especially PLCs and budget
HMIs — only ever implement FC3. Real meters answer both for exactly this reason,
so this one does too.
**Trade-off accepted:** a slight semantic muddle, documented explicitly in the
register map.

### D36 — Float32 as high word first, stated and tested
**Alternatives:** low word first; scaled integers only.
**Chosen:** most significant word in the lower register address, matching
Schneider PowerLogic meters.
**Why:** Modbus has no float type and **no agreed word order**, so every vendor
picks one. Getting it backwards makes every float come out as an absurd
magnitude, and it is the classic Modbus integration bug. Picking the convention
of the company this project is aimed at is the right default.
**Guarded by:** `test_float_word_order_is_high_word_first` — a known-answer test
asserting `1.0f` encodes as `0x3F80, 0x0000`.

### D37 — Publish a scaled-integer block *as well as* floats
**Why:** plenty of masters have no float support at all. Real meters publish
both. It costs ~20 lines and makes the device interoperable with far more
equipment.
**Trade-off accepted:** the same value exists at two addresses, so the register
map needs to say which is authoritative. It does.

### D38 — Clamp scaled values instead of letting them wrap
**Chosen:** saturate at the type's limits; map NaN and infinity to 0 or the
limit.
**Why:** a sensor fault or a NaN cast straight to `int16_t` produces a
*plausible-looking* wrong number — 70000 W wrapping to 4464 W. A master acting on
that would do the wrong thing confidently. Saturation at least looks wrong.
**Guarded by:** `test_scaled_block_clamps_garbage`.

### D39 — Protocol (0-based) addressing, stated explicitly
**Why:** vendor documentation often prints 1-based "register numbers" (holding
register 40001 = protocol address 0). Mixing the two conventions is the second
most common Modbus integration failure after word order. The map commits to the
addresses that actually travel on the wire and says so in three places.

### D40 — Reserved addresses inside the mapped span read as zero
**Alternatives:** raise `ILLEGAL_ADDRESS` for every gap.
**Chosen:** gaps inside `0x0000–0x0040` return 0; anything outside raises the
exception.
**Why:** it lets a master fetch all 39 measurement and status registers in **one
transaction** instead of four separate round trips, which is what keeps poll
latency low on a real bus. Genuine mistakes (a wildly wrong address) are still
caught.
**Trade-off accepted:** a typo *inside* the span silently returns 0 rather than
an error.

### D41 — Reject writes to every register except one command register
**Why:** measurements are physical facts. A master that could overwrite the
power register could make the device report a comfortable lie, and anyone
reading the data later would have no way to know. Only `0x0040` accepts writes,
and only two values.
**Guarded by:** `test_write_to_measurement_register_is_rejected`.

### D42 — Stay silent on bad CRC, wrong unit ID, and broadcast
**Why:** this is correct Modbus behaviour, and each case has a distinct reason.
- **Bad CRC:** the frame may not even have been addressed to us — the address
  byte itself could be the corrupted one. Answering corrupted traffic turns line
  noise into bus traffic and makes a bad bus worse.
- **Wrong unit ID:** not our conversation.
- **Broadcast (unit 0):** the write is applied but nothing is sent, because
  several slaves replying simultaneously would collide on a half-duplex line.
**Guarded by:** `test_bad_crc_is_silently_dropped`,
`test_frame_for_another_unit_is_ignored`,
`test_broadcast_write_applies_but_is_not_answered`.

### D43 — Compute the CRC bitwise rather than from a lookup table
**Chosen:** the 8-iteration inner loop.
**Why:** a 512-byte table would save a few microseconds per frame. At 19200 baud
a frame takes milliseconds to transmit, so the CRC is nowhere near the
bottleneck, and the flash is worth more than the cycles.
**Trade-off accepted:** marginally more CPU per frame. Unmeasurable here.

### D44 — Flush the receive buffer before every master request
**Why:** without it, a reply that arrived late from the *previous* question gets
parsed as the answer to *this* one. The result is data that looks completely
plausible but belongs to the wrong register range — the worst available failure
mode, because nothing downstream can detect it.
**Trade-off accepted:** a genuinely slow-but-valid reply gets discarded; the
retry (D45) covers that.

### D45 — Retry twice on timeout, never on an exception
**Why:** a single lost frame on an electrically noisy bus is normal, and treating
it as a device failure would make the system flap. But an *exception* means the
slave understood the request and said no — retrying an illegal address just gets
the same answer, so it returns immediately.
**Trade-off accepted:** a failing device takes 3 × 100 ms before being reported.

### D46 — Discover devices instead of hard-coding unit IDs
**Chosen:** sweep unit IDs 1–16 at startup, ask each for its identity block, add
whoever answers with the right vendor ID.
**Why:** adding a second meter becomes a DIP-switch change rather than a code
change, which is how real commissioning tools behave. It also reads the
`SIMULATED` capability bit, so the gateway knows whether the data is real.
**Trade-off accepted:** the scan stops at 16 rather than the protocol's 247,
because 247 × 100 ms would be a 25-second startup. 16 covers any bench setup;
the limit is a named constant.

### D47 — Track link health per device, not in aggregate
**Why:** on a real bus one bad cable run fails while the others are fine. An
aggregate error counter would hide exactly that. "Unit 3 shows 4% CRC errors
while units 1 and 2 show none" points at a specific cable; "the bus has errors"
does not.

### D48 — Keep the register map in one header, shared by both nodes
**Chosen:** `common/modbus/meter_regs.h` compiled into the slave *and* the
master, plus a consistency checker.
**Why:** the classic integration bug is the two ends of a bus disagreeing about
what lives at an address. One definition makes that impossible. The map also
exists as JSON (for tools) and Markdown (for humans), and
`tools/check_device_model.py` fails loudly if the three drift apart — three
copies of one truth eventually disagree.
**Trade-off accepted:** a shared header couples the two firmwares. That coupling
is the intent.

### D49 — A JSON device model as the semantic layer
**Why:** a raw register holding `17260` means nothing; a register described as
`{name: power, unit: W, type: float32}` means something. This is how Schneider's
Power Monitoring Expert works — devices are *modelled*, and the software builds
its tags from the model rather than hard-coded addresses. The payoff is that
adding a meter becomes a data change.
**Trade-off accepted:** the model is currently read by tools, not yet by the
gateway at runtime. That is a Phase 3 refinement.

---

## Part 7 — MQTT and Phase 2 decisions

### D50 — Store-and-forward by *not advancing a cursor*
**This is the headline feature, and the implementation is deliberately tiny.**

**Alternatives:** write buffered samples to flash/SD and replay on reconnect; a
separate outage queue; accept the loss.
**Chosen:** the MQTT publisher is an ordinary ring-buffer reader that simply
**refuses to advance its cursor while the link is down**. On reconnect it drains
the backlog oldest-first.
**Why:** the ring buffer (D20) already had independent reader cursors and
overrun counting, so the entire feature is "don't read while disconnected" —
about 60 lines. A flash-backed queue would survive a power cycle too, but needs
wear-levelling, a serialisation format and crash-consistency, none of which is
needed for a WiFi dropout. This is how Schneider's Panel Server behaves: its
logging memory buffers while the cloud link is down and publishes what it held
once the connection returns.
**Trade-off accepted:** buffering is RAM-only, so it does **not** survive a
reboot or a power cut — only a link outage. Stated plainly in the README.

### D51 — Report exact loss when an outage outlasts the buffer
**Why:** the buffer is finite (512 samples, ~51 s). Pretending otherwise would
be the same dishonesty as D21 at a different layer. A test asserts that
`published + dropped` accounts for every sample the ring ever saw, so nothing
vanishes unaccounted for.

### D52 — Decide "catching up" once per batch, not per sample
**Chosen:** evaluate the backlog against the threshold at the top of
`forwarder_take()` and apply that verdict to the whole batch.
**Why:** re-evaluating per sample would flip the backfill flag mid-batch as the
backlog shrinks past the threshold, producing a confusing mix of "live" and
"late" labels for one continuous stretch of recovered history.

### D53 — A backfill threshold, so jitter is not reported as an outage
**Chosen:** only call it backfill when the backlog exceeds ~2 poll periods'
worth of samples.
**Why:** a backlog of one or two samples is normal scheduling jitter. Flagging
that as an outage would cry wolf constantly and make the real signal useless.
**Guarded by:** `test_small_backlog_is_not_called_backfill`.

### D54 — The forwarder starts with the link assumed **down**
**Why:** a forwarder that assumed "up" would publish into a void during the
seconds before the first connection completes, losing exactly the first samples.
Pessimism is free here.
**Guarded by:** `test_link_starts_down`.

### D55 — PubSubClient as the MQTT library
**Alternatives:** `AsyncMqttClient` (non-blocking, more complex, callback-heavy);
`espMqttClient` (newer); hand-rolled MQTT.
**Chosen:** `knolleary/PubSubClient`.
**Why:** by far the most documented and widely used ESP32 MQTT client, and
synchronous — which suits the design, because all MQTT work happens inside one
dedicated task (D57) where blocking is harmless. Unlike Modbus (D32), there is
no interview value in hand-rolling MQTT: it is not the skill being demonstrated.
**Trade-off accepted:** PubSubClient is **not thread-safe**, which is why D57
matters, and its default buffer is 256 bytes — raised explicitly to 512.

### D56 — Hand-rolled JSON with `snprintf`, not ArduinoJson
**Why:** the payload shape is fixed and known at compile time, so a general JSON
serialiser earns nothing. `snprintf` into a stack buffer means no heap
allocation on the publish path and ~40 KB less flash. Short keys keep each packet
near 220 bytes.
**Trade-off accepted:** adding a field means editing a format string, and a
mismatched argument list is a runtime bug a serialiser would prevent. Acceptable
for one call site.

### D57 — All network work in exactly one task
**Chosen:** `task_uplink` owns WiFi, SNTP and every PubSubClient call.
**Why:** PubSubClient is not thread-safe (D55), so a single owner removes the
need for any locking around it. The same reasoning applies to the RS-485 bus:
only `task_poll` touches it after startup, so the bus needs no mutex either.
Single ownership is cheaper and safer than locking.
**Trade-off accepted:** that task does several unrelated things, so it is the
most complex function in the project.

### D58 — Split polling from uplink into two tasks
**Why:** WiFi stalls, DNS lookups block, brokers go away. None of that may
disturb metering. `task_poll` runs at a fixed 10 Hz and never waits on the
network; `task_uplink` deals with all of it. **Store-and-forward falls out of
this separation** rather than being bolted on — the ring between them *is* the
buffer.

### D59 — Take one SNTP reading, then track `base + millis()`
**Alternatives:** call `time()` per sample; use `gettimeofday` each time.
**Chosen:** record the epoch once at sync, then add the `millis()` delta.
**Why:** `time()` has 1-second resolution, which is useless for 10 Hz samples.
Worse, repeatedly reading the system clock can step *backwards* when SNTP
applies a correction, and a log whose timestamps go backwards is extremely hard
to reason about. Monotonicity matters more here than absolute precision.
**Trade-off accepted:** slow drift between hourly re-syncs, from the ESP32's RC
oscillator. Irrelevant at this scale.

### D60 — UTC, never local time
**Why:** storing local time makes one hour of every year ambiguous, and makes
correlating two devices in different zones painful. Local time is a
presentation concern for the dashboard.

### D61 — `t_unix_ms = 0` means "clock unknown"
**Why:** an honest sentinel the dashboard tests for and falls back from, rather
than inventing a plausible-looking wrong timestamp. The meter always sends 0
because it has no clock; the gateway fills it in.

### D62 — Register a Last Will and Testament
**Why:** if the gateway vanishes without disconnecting cleanly, the broker
publishes `offline` on its behalf. Without this, a dead gateway looks
*identical* to an idle one, and the dashboard would keep displaying the last
reading as though it were current — showing stale data as live is worse than
showing nothing.

### D63 — Retain status and stats, do not retain telemetry
**Why:** a dashboard opened later should immediately learn what devices exist
and how healthy the link is, so those are retained. Retaining telemetry would
mean a new subscriber first sees one stale reading as if it were live, so it is
not.

### D64 — QoS 0 for telemetry
**Why:** at 10 messages per second on a LAN, QoS 1's acknowledgement round-trip
and the client-side resend queue cost more than they are worth — and the
application already handles loss honestly (D21, D51). QoS is a transport
guarantee; store-and-forward is an application guarantee, and the application
one is stronger here.
**Trade-off accepted:** a packet lost on the wire is gone without being counted.
Revisit if Phase 4 moves to a remote broker.

### D65 — WiFi credentials in a gitignored `secrets.h`
**Chosen:** a committed `secrets.example.h`, a gitignored `secrets.h`, and an
`#if __has_include` guard so the firmware compiles either way.
**Why:** a committed WiFi password in a public repo gets found by automated
scanners within minutes. The `__has_include` guard means the repo stays clonable
and buildable by anyone — it just stays offline, with a compiler warning saying
why.

### D66 — Reconnect with capped exponential backoff
**Chosen:** 1 s doubling to a 15 s ceiling.
**Why:** retrying a down broker every 20 ms burns CPU and floods the log with
identical failures. Backoff with a cap recovers quickly from a brief blip while
staying quiet during a long outage.

### D67 — Rate-limit the backfill drain
**Chosen:** at most 10 samples published per uplink pass.
**Why:** draining 512 buffered samples as fast as possible would monopolise the
uplink and starve the polling task at exactly the moment the system is already
behind. A budget makes catch-up predictable — a full buffer clears in about a
second.

### D68 — One ring per device, not one shared ring
**Why:** `telemetry_sample_t` carries no unit ID, so samples from two meters in
one ring would be indistinguishable. Per-device rings also give per-device
outage accounting, which matches D47's reasoning.
**Trade-off accepted:** RAM scales with device count, which is why
`MAX_DEVICES` is 2 on the gateway. The arithmetic is spelled out in
`gateway_node/include/config.h`.

### D69 — Gateway ring of 512 samples; meter ring of 256
**Why:** they buffer against different things. The meter's 256 samples (~25 s)
cover a consumer stalling. The gateway's 512 (~51 s) cover a WiFi outage, which
lasts longer. Sizing them separately is the reason D23 made the size a build
flag.

---

## Part 8 — Dashboard decisions

### D70 — A single HTML file with no build step
**Alternatives:** React/Vue with a bundler; a Python/Flask server; Grafana.
**Chosen:** one `index.html` — inline CSS, inline JS, one CDN script.
**Why:** it opens by double-clicking, it has no `npm install`, and it will still
work in two years. Grafana would be more capable and is a large dependency to
install and configure for six charts. A bundler adds a build step to a project
that otherwise has none.
**Trade-off accepted:** no component model, so the file is ~570 lines. At this
size that is manageable.

### D71 — Hand-rolled canvas charts instead of Chart.js
**Why:** the charts need one thing no general library does easily — **recolouring
individual line segments** so backfilled samples are visually distinct from live
ones. That is the entire demo. Doing it in Chart.js means fighting its data
model; doing it on a raw canvas is ~80 lines and gives exact control.
**Trade-off accepted:** no tooltips, no zoom, no pan. Not needed for a live view.

### D72 — Plot against each sample's own timestamp, not its arrival time
**This is what makes the store-and-forward demo visible.**
**Why:** if points were plotted as they arrive, backfilled samples would pile up
at the right-hand edge and look like a burst of noise. Plotted against their own
timestamps, they land **in the gap they came from** — so the hole in the chart
visibly fills in. The feature becomes something you can see rather than
something you have to be told about.
**Trade-off accepted:** the insert path needs a binary search to tolerate
out-of-order arrivals.

### D73 — A CSV replay mode that needs no broker
**Why:** it makes the dashboard testable the moment it is written — no Mosquitto,
no ESP32, no WiFi. It exercises the real parser on a real capture. It was also
the only way this file could be verified at all, since the built-in browser
cannot open local files or a locally started server.
**Trade-off accepted:** ~30 lines of code that are not used in production.

### D74 — A local Mosquitto broker, not a cloud one
**Why:** free, works with no internet, and — importantly — **you can kill it**,
which is how the outage demo is produced. A hosted broker would make the single
most impressive demo in the project impossible to trigger on purpose.
**Trade-off accepted:** `allow_anonymous true`, which is fine on a laptop behind
a home router and unacceptable on anything exposed. Phase 4 adds TLS and auth.

### D75 — `mock_publisher.py` as a gateway stand-in
**Why:** it makes the whole store-and-forward demo recordable with **no hardware
at all** — which matters because the demo video is the highest-value artefact in
the project and the parts have not arrived. It publishes the exact topics and
payloads the firmware does, including the Last Will, so the dashboard cannot
tell the difference.
**Trade-off accepted:** it duplicates the firmware's buffering logic in Python,
so the two could drift. It is explicitly a stand-in for the *demo*, not a test
of the firmware — the firmware's own logic is covered by
`test_forwarder`.

---

## Part 9 — Testing decisions

### D76 — Test on the host, not on the target
**Alternatives:** on-target tests via `pio test -e esp32dev`; manual serial
inspection.
**Chosen:** a `native` environment running the firmware's own logic on the PC.
**Why:** a flash-and-check cycle is ~60 seconds; a host test run is ~10 seconds
for all 34. More fundamentally, it is what let the entire project be written
before any hardware existed. The code under test is the *same* source file,
because D27 made the locking a no-op off-target rather than stubbing the module
out.
**Trade-off accepted:** nothing hardware-dependent is covered — the I2C driver
and the RS-485 transport have no tests at all. That gap is stated explicitly
rather than glossed over.

### D77 — A property test for the CRC, not a magic number
**Chosen:** assert that running the checksum over a frame *including* its
appended CRC yields zero.
**Why:** a hard-coded expected value would have to come from somewhere — and if
that somewhere is this same implementation, the test is circular and proves
nothing. The zero-remainder property is a real mathematical property of CRC-16
that holds only if both the algorithm *and* the byte order are correct, so one
assertion covers both.

### D78 — Known-answer tests for the two classic Modbus bugs
**Why:** float word order (D36) and value clamping (D38) are the two failures
that produce *plausible but wrong* data rather than an obvious error. Those
deserve tests that pin exact bytes: `1.0f` must encode as `0x3F80, 0x0000`.

### D79 — Test the honest limits, not just the happy path
**Why:** `test_outage_beyond_buffer_reports_exact_loss` and
`test_after_loss_survivors_are_the_newest_window` assert what happens when the
design's own limits are exceeded. A test suite that only covers success teaches
you nothing about failure, and failure is where energy data goes wrong quietly.

### D80 — An automated consistency check across three representations
**Chosen:** `tools/check_device_model.py` compares the C header, the JSON device
model and the Markdown documentation.
**Why:** three copies of one truth drift apart eventually, and a register map
that disagrees with its own documentation is actively harmful — someone
integrating against it gets wrong data and has no reason to doubt the docs. The
script also catches overlapping addresses and points outside the poll block.

---

### D81 — Set `TELEMETRY_RING_SIZE` in `platformio.ini`, never in a header
**Found while writing [FLOW.md](FLOW.md), not by a failing test.**

**The hazard:** `TELEMETRY_RING_SIZE` sizes an array *inside* a struct
(`telemetry_ring_t`). `common/telemetry/ring_buffer.h` carries its own
`#ifndef` fallback of 256. But `modbus_task.cpp` includes `modbus_task.h` —
which pulls in `ring_buffer.h` — **before** it includes `config.h`. So if the
value lived in `config.h`, that one translation unit would fall back to the
header default while every other unit used the configured value.

Two objects would then disagree about `sizeof(telemetry_ring_t)`. That is a
one-definition-rule violation: the linker cannot detect it, there is no
warning, and the symptom would be memory corruption in whichever task guessed
wrong.

**Chosen:** define it as a build flag in `platformio.ini` for every environment,
and delete it from `config.h` with a comment explaining why. A `-D` reaches
every translation unit regardless of include order. Backed by a
`static_assert` in `modbus_task.cpp` so a future regression fails at compile
time rather than at runtime.
**Why not just reorder the includes:** that would fix today's symptom and leave
the trap armed for the next person who adds a file.
**Trade-off accepted:** the value now lives in the build config rather than
beside the code it configures, which is slightly less discoverable. The comment
in `config.h` points at it.
**Note:** both values were 256, so nothing was actually broken — this was a
latent bug, fixed before it could bite.

---

## Part 9b — The Phase 3 reframe (16 October 2026)

Four decisions taken together after the project's premise was challenged. Worth
reading as a group: they are one change of direction, not four tweaks.

### D82 — Target an EV charging site, not a residential building
**The trigger:** Rishit pushed back on the project's own premise. Shedding a
domestic water heater to shave a bill trades a person's comfort for money, and
he was not willing to present something he did not believe in.

He was right, and the field data agrees. In a published trial of 191 households
on grid-interactive water heaters, **around 40% reported cold water draws and
two participants withdrew over comfort.**

**The diagnosis:** any domain where the deferrable load is attached to a human's
comfort is a bad domain for this project. The trade will always look bad because
it *is* bad.

**Alternatives considered:**
- *Residential/apartments* — rejected. The comfort trade above.
- *Data centre thermal + batch compute* — Schneider's hottest market, and the
  deferrable thing there (batch jobs) is genuinely free to defer. Rejected on
  hardware fit: you cannot shed cooling, and shifting compute needs servers and
  a job scheduler, not an ESP32 and a relay.
- *Cold storage* — credible and ethical, large thermal mass, no comfort
  involved. Smaller hiring market than EV infrastructure.
- *Generic "site with demand charges"* — flexible but vague, and vague is
  forgettable in an interview.

**Chosen:** an EV charging site.
**Why:** deferral is genuinely free — a car plugged in at 19:00 and needed at
07:00 wants ~4 hours of charge in a 12-hour window, so there are ~8 hours of
slack and the requirement is "full by morning", not "charge now". The economics
are real: demand charges are reported at **30–70% of such a site's bill**, with
facility-level load management cutting them **35–55% without reducing charging
availability**. It fits the existing bench hardware with zero changes, and it
widens the audience well beyond Schneider.
**Trade-off accepted:** a narrower, more specific story. That is the point —
specific is memorable.

**Constraint that shaped this:** the resume was already sent, claiming the node
*"auto-sheds non-critical loads to cut peak demand and cost."* Every candidate
reframe had to keep that sentence true. This one does — it still auto-sheds
non-critical loads to cut peak demand; it merely also guarantees the deferred
load finishes. A reframe that contradicted the resume would have been rejected
regardless of its merits.

### D83 — Deadlines as hard constraints; peak as the objective
**Alternatives:** the Phase 3 design as originally sketched — priority ordering
plus a threshold, with comfort as a soft consideration.
**Chosen:** invert it. **Never violate a deadline; minimise peak subject to
that.**

```
Old:  if (power > limit) shed(lowest_priority)   -> "whose comfort do I spend?"
New:  each load needs E Wh by time T;
      schedule to meet every deadline, minimising the 15-min peak
```

**Why:** it converts service from something traded into something *guaranteed*,
and "zero deadline misses" is a binary claim anyone can check. It also gives the
controller actual algorithmic content — a scheduling problem — rather than a
comparison against a constant.
**Trade-off accepted:** more state per load (energy required, deadline, energy
delivered so far) and a harder controller to get right. The `max_shed_duration`
timer from the earlier sketch is retained, but renamed honestly: it is a **crude
proxy for buffer state** used when the real state cannot be measured.
*Honest novelty grading:* studied academically as departure-time-constrained
charging. Absent from vendor and hobby material, which describes capacity
splitting and load balancing. Strong framing; not a novel algorithm.

### D84 — NILM as actuation verification, not just analytics
**The observation:** a relay is an **open-loop** actuator. You command it and
receive no confirmation. Contacts weld shut (common when switching inductive or
high-current loads), channels get miswired, loads get unplugged, appliances with
their own controller ignore you.

The failure this produces is the dangerous kind: the controller believes it shed
40 W, therefore believes the peak is handled, therefore stops looking for more
to shed. The demand charge lands and **nothing in the system ever notices.** The
controller has lied to itself.

**Chosen:** Phase 5's classifier does double duty. Command a load off, then
confirm from the **single aggregate meter** that it actually changed state —
checking the magnitude *and* the shape of the transition, so it still works when
several loads move at once.

**Why this specifically:** the feedback loop comes free from a sensor installed
for a completely different reason. No auxiliary contact on the relay, no current
sensor per channel, no extra wiring — which keeps D4's retrofit-first property
intact.
*Honest novelty grading:* a 2025 systematic review names this exact closed loop
as a requirement — *"after the system issues instructions… it must reliably
verify whether the instructions were executed… forming a closed loop with the
control module"* — and MCU-based NILM is an active research area. The review's
own gap statement is that the components exist separately, with no full embedded
deployment closing the loop. **That gap is where this sits. The strongest genuine
contribution in the project.**

### D85 — Publish the service cost alongside the money
**Chosen:** report deadline misses, service violations and (Phase 5) actuation
faults caught, next to kW and ₹.

```
peak demand reduced   __%      cost saved          __%
deadline misses         0      service violations    0
actuation faults caught __
```

**Why:** a demand-response result quoted without its service cost is incomplete.
"Peak down 35%" alone is precisely the claim that produced the 40% cold-water
statistic in D82. "Peak down 35% with zero deadline misses" is an engineering
claim.
**Trade-off accepted:** none worth mentioning — it is bookkeeping. The reason
almost nobody does it is that it invites scrutiny, which is also the reason to
do it.

---

## Part 10 — Deliberately deferred

Things *not* done yet, and why that is the right call for now.

| Deferred | Why not yet |
|---|---|
| Time-of-use tariffs | Needs the Phase 3 controller to act on them. A half-built pricing table would be worse than a flat rate, which is at least honestly labelled as an assumption. |
| Energy persistence across reboot | Needs flash wear-levelling and a crash-consistent format. Not required to demonstrate anything in Phases 0–3. |
| TLS and MQTT authentication | Phase 4. Adding TLS before the application works would make every bug a two-variable problem. |
| Modbus-TCP | Phase 3.5. Cheap once RTU works (D33 made the core reusable), but it adds nothing until there is something worth gatewaying. |
| A historical data store | The dashboard holds ~4 minutes. A database is a different project; captures on disk cover the analysis need. |
| Runtime use of the device model | The gateway still has the map compiled in (D48). Reading the JSON at runtime is the "real" version and a Phase 3 refinement. |
| NILM | Phase 5, and it depends on everything above it. D5 and D18 exist so that it is additive rather than a rewrite. |

---

## Part 11 — Mistakes made, and what changed

Kept deliberately. Each of these was a real error during development.

### M1 — The host build broke when the Modbus task was added
`native-sim` compiles all of `src/`, which came to include `modbus_task.cpp` —
an Arduino-only file. Caught by running every environment rather than just the
one being worked on.
**Change:** that environment's source filter now excludes Arduino-only files
explicitly, and all five environments are built before any commit.

### M2 — README edits silently did nothing
Several Phase 1 documentation patches never applied, because Python's
`str.replace` does nothing when the pattern does not match — and the file had
CRLF line endings while the patterns used `\n`. The result was reported as
"patched" without verification, leaving a committed README with a stale layout
block and an unchecked roadmap for two phases.
**Change:** patch scripts now report which replacements matched and which
missed; a miss is a failure, not a silent no-op.

### M3 — Mixed character encodings in the README
Those same scripts read and wrote with Windows' default cp1252 codec, leaving
the file a mix of UTF-8 and cp1252 bytes. GitHub renders invalid bytes as `�`.
**Change:** all file I/O specifies UTF-8 explicitly, and a scan now verifies
every tracked file decodes cleanly before committing.

### M4 — `volatile++` and an always-false comparison
Two compiler warnings that were real: a deprecated increment on a `volatile`
field, and `addr < 0` on an unsigned type.
**Change:** `-Wall -Wextra` on every environment, and warnings are fixed rather
than tolerated. All five builds are currently warning-free.

### M5 — A second silent patch failure, found the same way
While cross-checking FLOW.md against the source, the "Telemetry ring" comment in
`meter_node/include/config.h` turned out to still be its original Phase 0 text —
a Phase 2 edit to it had silently missed, exactly as in M2. Writing the
documentation is what surfaced it, which is an argument for doing so.
**Change:** the same one as M2 — verify what a patch actually changed rather
than trusting that it ran. Cross-checking docs against source is now part of
finishing a phase, and it found both M5 and the D81 hazard in one pass.

**The common thread:** every one of these was found by *verifying* rather than
assuming — running all the builds, checking what a patch actually did, reading
the compiler output. That habit is worth more than any individual fix.
