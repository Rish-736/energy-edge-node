# How the code runs

Where execution starts, what order things happen in, and which function calls
which — across files.

Read this alongside [DECISIONS.md](DECISIONS.md), which explains *why* each
piece is shaped the way it is. This document only explains *what happens*.

Last updated: Phase 2 complete. The reframe of 16 October 2026 (see
[DECISIONS.md](DECISIONS.md) D82-D85) changed what the project is *for*, not how
the existing code executes, so nothing in this document changed with it. Phase 3
adds a controller task on the gateway, which will be documented here in the same
commit as the code.

---

## 1. The mental model: this is not one program

The first thing to understand is that **there is no single entry point**, because
this repo builds **four different programs** from largely the same source:

| Program | Entry point | Runs on |
|---|---|---|
| **Meter firmware** | `meter_node/src/main.cpp` → `setup()` | ESP32 #1 |
| **Gateway firmware** | `gateway_node/src/main.cpp` → `setup()` | ESP32 #2 |
| **Host simulator** | `meter_node/src/sim_main.cpp` → `main()` | your PC |
| **Unit tests** | `meter_node/test/*/test_*.cpp` → `main()` | your PC |

Which one you get depends on the PlatformIO environment you build. The
`build_src_filter` lines in `platformio.ini` decide which files are included and
which are excluded — that is how `main.cpp` and `sim_main.cpp` can both define
an entry point without colliding.

```bash
pio run -e esp32dev-sim -d meter_node    # meter firmware, fake sensor
pio run -e esp32dev     -d meter_node    # meter firmware, real INA226
pio run -e esp32dev     -d gateway_node  # gateway firmware
pio run -e native-sim   -d meter_node    # host simulator
pio test -e native      -d meter_node    # unit tests
```

### Who owns what

Four shared modules are compiled into both firmwares:

```
common/telemetry/telemetry.*     the sample format + status flags
common/telemetry/ring_buffer.*   the circular buffer
common/telemetry/forwarder.*     store-and-forward (gateway only, by use)
common/modbus/modbus_rtu.*       protocol framing, CRC, slave/master helpers
common/modbus/meter_regs.*       the register map
common/modbus/rs485.*            the physical layer (Arduino-only)
```

---

## 2. Meter firmware — boot

**File:** `meter_node/src/main.cpp`

Arduino calls `setup()` once. In order:

```
setup()
 │
 ├─ Serial.begin(115200)              serial console
 ├─ delay(300)                        let the USB serial settle
 │
 ├─ ring_init(&g_ring)                → common/telemetry/ring_buffer.cpp
 │                                      zeroes the buffer, write_seq = 0
 │
 ├─ energy_reset()                    → meter_node/src/energy.cpp
 │                                      Wh = 0, accumulator not yet primed
 │
 ├─ ina226_init()                     → ina226_real.cpp  OR  ina226_sim.cpp
 │                                      whichever the build flag selected
 │                                      real: I2C begin, check TI id, write
 │                                            config + calibration registers
 │                                      sim:  reseed the PRNG
 │
 ├─ Serial.println / printf           header lines for the log format
 │
 ├─ xTaskCreatePinnedToCore(task_sampler, prio 5, core 1)
 ├─ xTaskCreatePinnedToCore(task_logger,  prio 2, core 0)
 │
 └─ modbus_task_start(&g_ring)        → meter_node/src/modbus_task.cpp
                                        which itself creates:
                                        task_modbus, prio 4, core 1
```

After `setup()` returns, **three tasks are running** and `setup()` is never
called again. `loop()` exists but does nothing except sleep — all real work is
in the tasks.

### Why the shared state is a file-scope global

`g_ring` is declared at file scope in `main.cpp` and its address is handed to
`modbus_task_start()`. The ring is the only thing the three tasks share.

---

## 3. Meter firmware — steady state

Three tasks, each in an infinite loop, never returning.

### task_sampler — every 100 ms, priority 5, core 1

The real-time path. Deliberately short.

```
task_sampler()                              main.cpp
 │
 └─ forever:
     ├─ s.t_ms  = millis()
     ├─ s.flags = ina226_is_simulated() ? TLM_FLAG_SIMULATED : 0
     │
     ├─ ina226_read(&r)                     ina226_sim.cpp / ina226_real.cpp
     │   │                                  returns volts, amps, watts
     │   └─ on failure: set TLM_FLAG_SENSOR_FAIL and keep the cadence
     │
     ├─ s.wh    = energy_update(s.p, s.t_ms)    energy.cpp
     │                                          trapezoidal, real elapsed dt
     ├─ s.truth = ina226_truth_mask()           which loads are really on
     │
     ├─ ring_push(&g_ring, &s)              ring_buffer.cpp
     │                                      stamps s.seq, writes at
     │                                      seq & mask, increments write_seq
     │
     └─ vTaskDelayUntil(&last_wake, 100ms) absolute wake time, so the rate
                                           does not drift
```

**Note what it does not do:** it never waits for a consumer, never touches the
network, and never checks whether anyone is reading. That is why a stalled
consumer cannot delay a sample.

### task_modbus — every 1 ms, priority 4, core 1

**File:** `meter_node/src/modbus_task.cpp`

Two unrelated jobs: keep the registers current, and answer the bus.

```
task_modbus()                               first-time setup:
 ├─ rs485_begin(&s_bus, &Serial2, 19200, rx=16, tx=17, de=4)
 │                                          rs485.cpp — computes t3.5, DE low
 ├─ meter_regs_init(&s_regs, fw, simulated) meter_regs.cpp
 ├─ modbus_slave_init(&s_slave, unit=1, meter_regs_read, meter_regs_write,
 │                    &s_regs)              modbus_rtu.cpp — binds callbacks
 ├─ ring_reader_attach(ring, &s_reader, "modbus")
 │                                          its OWN cursor, starts live
 │
 └─ forever:
     ├─ refresh_registers(ring)
     │   ├─ while (ring_read(...)) got = true     drain to the NEWEST sample
     │   │                                        and keep only that one
     │   ├─ copy v/i/p/wh/flags/truth/seq into s_regs.data
     │   ├─ s_regs.data.uptime_s = millis()/1000
     │   └─ s_regs.data.overruns = s_reader.overruns
     │
     ├─ if (s_regs.energy_reset_requested)
     │     energy_reset()                   a master wrote the command register
     │
     ├─ n = rs485_poll_frame(&s_bus)        rs485.cpp — non-blocking.
     │                                      Reads available bytes, timestamps
     │                                      each. Returns a length only once
     │                                      3.5 char times of silence pass.
     │
     ├─ if (n):
     │   ├─ rn = modbus_slave_handle(&s_slave, buf, n, resp, cap)
     │   │       └─ (see §4 for the full chain)
     │   └─ if (rn) rs485_send(&s_bus, resp, rn)
     │                                      DE high → write → flush → DE low
     │
     ├─ every 10 s: print request/CRC/exception counters
     └─ vTaskDelay(1ms)
```

**The key contrast with the logger:** this task *drains to the newest sample and
discards the rest*, because Modbus registers hold present values, not history.
The logger, reading the same ring, keeps every single sample. Two readers, two
different needs, neither aware of the other.

### task_logger — every 100 ms, priority 2, core 0

```
task_logger()                               main.cpp
 ├─ ring_reader_attach(&g_ring, &g_logger_reader, "logger")
 │
 └─ forever:
     ├─ while (ring_read(...)):             drain EVERY sample
     │     Serial.printf("T,%seq,%t_ms,%v,%i,%p,%wh,%flags,%truth\n")
     │
     ├─ every 10 s: print "# stats seq=... overruns=... heap=..."
     └─ vTaskDelay(100ms)
```

### Who touches the ring

```
                   ring_push                 (task_sampler, core 1)
                       │
              ┌────────▼────────┐
              │    g_ring       │   portMUX critical section on every access
              └────┬───────┬────┘
                   │       │
      ring_read ───┘       └─── ring_read
   (task_logger,            (task_modbus,
    core 0,                  core 1,
    own cursor)              own cursor)
```

One writer, two readers, three tasks, two cores. The `portMUX` critical sections
inside `ring_buffer.cpp` are what make the cross-core struct copies safe.

---

## 4. One Modbus transaction, end to end

This is the cross-file chain worth being able to draw on a whiteboard.

### The gateway asks

```
poll_device(d)                          gateway_node/src/main.cpp
 └─ txn_read_retry(d, FC4, 0x0010, 39, block)
     └─ txn_read(unit, fc, addr, qty, out, &latency, &exception)
         │
         ├─ modbus_master_build_read(...)        common/modbus/modbus_rtu.cpp
         │     builds [unit][fc][addr_hi][addr_lo][qty_hi][qty_lo][crc_lo][crc_hi]
         │
         ├─ rs485_flush_input(&s_bus)            discard any stale reply
         ├─ t0 = micros()
         ├─ rs485_send(&s_bus, req, 8)           DE high → write → flush → DE low
         │
         ├─ rs485_recv_frame(&s_bus, 100ms)      blocking, bounded
         │     └─ loops on rs485_poll_frame() until a frame or timeout
         │
         ├─ latency = micros() - t0
         └─ modbus_master_parse_read(...)        checks CRC, unit, function code,
                                                 byte count; detects exceptions
```

### The meter answers

```
rs485_poll_frame()  returns a complete frame        (task_modbus)
 └─ modbus_slave_handle(s, req, req_len, resp, cap)   modbus_rtu.cpp
     │
     ├─ req_len < 4?                 → return 0 (silence)
     ├─ crc_ok(req, req_len)?         no → crc_errors++, return 0 (silence)
     ├─ req[0] != our unit && != 0?   → ignored++,    return 0 (silence)
     ├─ requests++
     │
     └─ switch (req[1]):
         case 0x03 / 0x04:                        read holding / input
           ├─ validate qty in 1..125
           ├─ for each register:
           │     meter_regs_read(addr + k, &vals[k], ctx)   meter_regs.cpp
           │       └─ switch on address:
           │           identity     → constants / fw_version
           │           float32      → mb_put_f32(pair, value), pick hi/lo word
           │           scaled ints  → clamp_u16 / clamp_i16 / clamp_u32
           │           status       → flags, truth, seq, uptime, overruns
           │           reserved     → 0
           │           out of span  → return false → ILLEGAL_ADDRESS
           ├─ build [unit][fc][byte_count][data...][crc]
           └─ return length
         case 0x06 / 0x10:                        write single / multiple
           └─ meter_regs_write(addr, val, ctx)
                 only 0x0040 accepted; anything else → ILLEGAL_ADDRESS
         default:
           └─ make_exception(ILLEGAL_FUNCTION)
```

Then back in `task_modbus`: `rs485_send(&s_bus, resp, rn)`.

### Why this chain is testable without hardware

Notice that `modbus_slave_handle()` and the master build/parse functions take
**only buffers**. They never touch `rs485.cpp`. So a test can call the master
builder, hand the bytes straight to the slave handler, and hand the reply back
to the master parser — the "cable" is a function call:

```
meter_node/test/test_modbus/test_modbus.cpp

  modbus_master_build_read(...)  ──┐
                                   │  bus_exchange()  ← this IS the cable
  modbus_slave_handle(...)      ◄──┘
           │
           └──► modbus_master_parse_read(...)
```

---

## 5. Gateway firmware — boot

**File:** `gateway_node/src/main.cpp`

```
setup()
 │
 ├─ Serial.begin(115200), delay(300)
 ├─ build s_client_id from the chip's MAC          "een-gw-XXXX"
 ├─ rs485_begin(&s_bus, &Serial2, 19200, 16, 17, 4)
 ├─ print link settings, ring size, MQTT target
 │
 ├─ discover_bus()                      ← runs synchronously, BEFORE any task
 │   └─ for unit = 1..16:
 │       ├─ txn_read(unit, FC3, 0x0000, 5, id, ...)    identity block
 │       ├─ no answer → next unit (silence just means nobody home)
 │       ├─ wrong vendor id → log and skip
 │       └─ match:
 │           ├─ s_dev[n] = { unit, fw, capabilities }
 │           ├─ ring_init(&d->ring)                    per-device ring
 │           └─ forwarder_init(&d->fwd, &d->ring, 20)  link assumed DOWN
 │
 ├─ s_mqtt.setServer / setKeepAlive / setBufferSize(512)
 │
 ├─ xTaskCreatePinnedToCore(task_poll,   prio 4, core 1)
 └─ xTaskCreatePinnedToCore(task_uplink, prio 3, core 0)
```

**Discovery runs in `setup()`, not in a task.** It is a one-off that blocks for
up to 16 × 100 ms, and it must finish before polling starts — there is nothing to
poll until it does.

Unlike the meter, the gateway's `loop()` *does* something: every 10 seconds it
prints per-device statistics. It is still not on any critical path.

---

## 6. Gateway firmware — steady state

### task_poll — every 100 ms, priority 4, core 1

```
task_poll()
 └─ forever:
     ├─ for each discovered device:
     │   └─ poll_device(d)
     │       ├─ txn_read_retry(d, FC4, 0x0010, 39, block)    ← see §4
     │       │     on failure: d->last_valid = false, return
     │       │
     │       ├─ meter_regs_decode_poll(block, &d->last)      meter_regs.cpp
     │       │     mb_get_f32 / mb_get_u32 back into real numbers
     │       │
     │       ├─ build a telemetry_sample_t:
     │       │     t_ms      = millis()
     │       │     t_unix_ms = now_unix_ms()     ← 0 if never synced
     │       │     v/i/p/wh/flags/truth from d->last
     │       │
     │       └─ ring_push(&d->ring, &s)      ← into the ring, link or no link
     │
     └─ vTaskDelayUntil(&last_wake, 100ms)
```

**This task never touches the network.** That is the whole point — it keeps
filling the ring whether or not anything can be published.

### task_uplink — every 20 ms, priority 3, core 0

```
task_uplink()
 ├─ wifi_begin()                        WIFI_STA, sleep off, WiFi.begin()
 │
 └─ forever:
     ├─ linked = net_up() && s_mqtt.connected()
     │
     ├─ if (linked != was_linked):            ← ONE place decides the link state
     │   ├─ for each device: forwarder_set_link(&d->fwd, linked)
     │   └─ log "uplink UP / DOWN" (+ backlog size when coming up)
     │
     ├─ if (!linked):
     │   └─ if (now >= next_attempt):
     │       ├─ WiFi down?  → WiFi.reconnect()
     │       ├─ WiFi up?    → clock_sync() if needed, then mqtt_connect()
     │       └─ backoff = min(backoff * 2, 15s)
     │
     ├─ else:
     │   ├─ backoff = 1s
     │   ├─ s_mqtt.loop()                     PubSubClient keepalive/rx
     │   ├─ for each device: publish_batch(d)
     │   ├─ every 5 s: publish_stats()
     │   └─ hourly: clock_sync()
     │
     └─ vTaskDelay(20ms)
```

`mqtt_connect()` registers the Last Will (`gateway/status` = `offline`,
retained), publishes `online`, then re-announces every discovered meter on
`meter/<unit>/status` with its firmware version, capabilities and whether its
data is simulated — all retained, so a dashboard opened later knows immediately
what exists.

### One MQTT publish

```
publish_batch(d)
 ├─ n = forwarder_take(&d->fwd, batch, 10)         common/telemetry/forwarder.cpp
 │   │
 │   ├─ link down?  → return 0           ← THE store-and-forward mechanism:
 │   │                                     the cursor simply does not advance,
 │   │                                     so samples stay in the ring
 │   ├─ backlog = ring_available(...)
 │   ├─ catching_up = backlog > threshold    decided ONCE for the whole batch
 │   └─ while (n < budget && ring_read(...)):
 │         if (catching_up) out[n].flags |= TLM_FLAG_BACKFILL
 │
 └─ for each sample:
     ├─ kwh  = wh / 1000
     ├─ cost = kwh * TARIFF_INR_PER_KWH
     ├─ co2  = kwh * CO2_G_PER_KWH
     ├─ snprintf the JSON payload               no allocation, ~220 bytes
     └─ s_mqtt.publish("een/bench/meter/1/telemetry", payload)
```

### Who touches what on the gateway

```
   txn_read / rs485_*     ──  ONLY task_poll (after setup)   → no mutex needed
   PubSubClient / WiFi    ──  ONLY task_uplink               → no mutex needed
   d->ring                ──  written by task_poll,
                              read by task_uplink via the forwarder
                              → protected by portMUX inside ring_buffer.cpp
```

Single ownership of the bus and of the MQTT client is deliberate: it removes the
need for locking around either, which is cheaper and safer than adding mutexes.

---

## 7. What happens during an outage

The sequence worth understanding, because it is the demo.

| Time | `task_poll` | `task_uplink` | Ring state |
|---|---|---|---|
| normal | pushes 10/s | takes up to 10 per pass | stays near empty |
| WiFi drops | **pushes 10/s, unchanged** | `linked` false → `forwarder_set_link(false)` → `forwarder_take` returns 0 | **fills up** |
| 25 s later | still pushing | still refusing | ~250 samples held |
| WiFi returns | still pushing | `forwarder_set_link(true)`, backlog 250 > 20 → batches tagged `BACKFILL` | drains at 10 per 20 ms |
| ~0.5 s later | still pushing | caught up, flag stops being set | near empty again |

If the outage exceeds the 512-sample ring, `ring_read()` detects the reader has
been lapped, fast-forwards it to the oldest surviving sample, and adds the exact
number lost to `reader.overruns` — which `forwarder_dropped()` reports and
`publish_stats()` sends out. Nothing is lost silently.

**On the dashboard** the samples carry their *own* timestamps, so the backfilled
batch lands in the gap it came from rather than at the right-hand edge. That is
why the hole visibly fills in.

---

## 8. Host simulator — `native-sim`

**File:** `meter_node/src/sim_main.cpp`

A plain `main()` that drives the **real** simulator, the **real** energy
accumulator and the **real** ring buffer — the same source files the ESP32
builds. Only the board is absent.

```
main(argc, argv)
 ├─ seconds = argv[1] or 120
 ├─ ring_init, energy_reset, ina226_init, ring_reader_attach
 ├─ print the same "# ..." header the firmware prints
 │
 ├─ for k in 0 .. seconds*10:
 │   ├─ sim_set_time_ms(k * 100)        ← virtual clock, so it runs flat out
 │   ├─ ina226_read(&r)
 │   ├─ energy_update, ina226_truth_mask
 │   ├─ ring_push
 │   └─ while (ring_read(...)) printf("T,...")      same CSV as the firmware
 │
 └─ print sample count, energy, overruns to stderr
```

`build_src_filter` excludes `main.cpp` (Arduino `setup`/`loop`),
`ina226_real.cpp` (needs I2C) and `modbus_task.cpp` (needs Arduino), which is
how two entry points coexist in one source tree.

```bash
pio run -e native-sim -d meter_node
./meter_node/.pio/build/native-sim/program 180 > docs/captures/sim_180s.csv
python tools/replay.py docs/captures/sim_180s.csv
```

---

## 9. Unit tests — `native`

Each test directory is a separate program with its own `main()`:

| Directory | Tests | Entry |
|---|---|---|
| `test/test_ring/` | 6 | `main()` → `UNITY_BEGIN` → `RUN_TEST(...)` |
| `test/test_modbus/` | 18 | same |
| `test/test_forwarder/` | 10 | same |

Unity calls `setUp()` before each test and `tearDown()` after. The `native`
environment sets `build_src_filter = -<*>` — **nothing** from `src/` is
compiled, because everything under test lives in `common/`, pulled in as
libraries.

The three suites drive their subject directly:

```
test_ring       → ring_init, ring_push, ring_reader_attach, ring_read
test_modbus     → modbus_master_build_* → modbus_slave_handle → modbus_master_parse_*
test_forwarder  → forwarder_set_link(true/false) + ring_push, then forwarder_take
```

`test_forwarder` is worth noting: the entire store-and-forward feature is
exercised by **toggling a boolean** and counting what comes out. No WiFi, no
broker, no board.

---

## 10. File reference

### Shared — `common/`

| File | Contains | Called by |
|---|---|---|
| `telemetry/telemetry.h` | `telemetry_sample_t`, `TLM_FLAG_*`, `LOAD_*` | everything |
| `telemetry/telemetry.cpp` | `telemetry_truth_str()` | logger, tests |
| `telemetry/ring_buffer.h/.cpp` | `ring_init`, `ring_push`, `ring_reader_attach`, `ring_read`, `ring_available` | both firmwares, forwarder, tests |
| `telemetry/forwarder.h/.cpp` | `forwarder_init`, `forwarder_set_link`, `forwarder_take`, `forwarder_backlog`, `forwarder_dropped` | gateway, tests |
| `modbus/modbus_rtu.h/.cpp` | `modbus_crc16`, `modbus_slave_init/handle`, `modbus_master_build_*`/`parse_*`, `mb_put_f32`/`mb_get_f32`, `mb_put_u32`/`mb_get_u32`, `mb_result_str` | both firmwares, tests |
| `modbus/meter_regs.h/.cpp` | the register map; `meter_regs_init/read/write`, `meter_regs_decode_poll` | meter (serves), gateway (decodes), tests |
| `modbus/rs485.h/.cpp` | `rs485_begin`, `rs485_poll_frame`, `rs485_recv_frame`, `rs485_send`, `rs485_flush_input` | both firmwares. **Arduino-only** (`#ifdef ARDUINO`) |

### Meter — `meter_node/`

| File | Contains |
|---|---|
| `src/main.cpp` | `setup()`, `loop()`, `task_sampler()`, `task_logger()` |
| `src/modbus_task.cpp` | `modbus_task_start()`, `task_modbus()`, `refresh_registers()` |
| `src/ina226_sim.cpp` | the simulator backend; `noise()`, `in_window()`, `sim_set_time_ms()` |
| `src/ina226_real.cpp` | the I2C driver; `reg_read()`, `reg_write()` |
| `src/energy.cpp` | `energy_reset()`, `energy_update()`, `energy_wh()` |
| `src/sim_main.cpp` | host `main()` — excluded from ESP32 builds |
| `include/config.h` | rates, pins, I2C address, Modbus unit/baud, ring size |

Both `ina226_*.cpp` define the *same* four functions; `-D USE_INA226_SIM`
decides which one compiles (each file is wrapped in a matching `#ifdef` /
`#ifndef`).

### Gateway — `gateway_node/`

| File | Contains |
|---|---|
| `src/main.cpp` | `setup()`, `loop()`, `task_poll()`, `task_uplink()`, `discover_bus()`, `poll_device()`, `txn_read()`, `txn_read_retry()`, `clock_sync()`, `now_unix_ms()`, `mqtt_connect()`, `publish_batch()`, `publish_stats()`, `topic()`, `net_up()`, `wifi_begin()` |
| `include/config.h` | RS-485 pins, timeouts, scan range, MQTT settings, tariff, backfill threshold |
| `include/secrets.example.h` | credential template — copy to `secrets.h` (gitignored) |

### Tools and dashboard

| File | Role |
|---|---|
| `tools/replay.py` | parse a capture, validate it, plot it |
| `tools/check_device_model.py` | assert the C header, JSON model and docs agree |
| `tools/mock_publisher.py` | replay a capture to MQTT as if it were the gateway |
| `dashboard/index.html` | the live page — MQTT-over-websockets, canvas charts, CSV replay |
| `dashboard/mosquitto.conf` | broker with both a TCP and a websocket listener |

---

## 11. Three things that surprise people

**1. The sampler does not know anyone is listening.** It pushes into the ring and
sleeps. Everything else — logging, Modbus, MQTT, and eventually the controller
and the classifier — is a *reader* that attached itself. Adding a consumer never
modifies the producer.

**2. The same ring buffer plays two completely different roles.** On the meter it
is sampling history; on the gateway it is the outage buffer. Same file, same
tests, different job — which is why store-and-forward took ~60 lines instead of
a new subsystem.

**3. The protocol code never touches a UART.** `modbus_rtu.cpp` moves bytes
between buffers. `rs485.cpp` moves bytes between a buffer and a wire. Keeping
those separate is what makes 18 of the 34 tests possible with no hardware, and
it is the single structural decision most worth explaining in an interview.
