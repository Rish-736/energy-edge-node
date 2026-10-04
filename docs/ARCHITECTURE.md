# Architecture — in plain language

This document explains what this project is, how it is put together, and why it
is built that way. It assumes no background in electronics or software.

---

## 1. What problem is this solving?

Electricity costs money, and large buildings get charged twice over:

1. **For how much** energy they use (kilowatt-hours).
2. **For their worst moment** — the single highest spike of demand in the month.

That second charge surprises people. A factory can use modest power all month,
switch three big machines on at the same time once, and get billed against that
spike for the whole month.

So there is real money in a device that watches power use and **automatically
switches off whatever matters least** before a spike happens.

That is what this project is: a small, safe model of such a device.

---

## 2. What is physically here?

A 12 V bench supply stands in for the electricity grid. Three loads stand in for
appliances: a resistor (a "water heater"), a small motor (a "fan"), some LEDs
("lights").

A sensor sits in the wire and measures how much power is flowing. One small
computer reads that sensor. A second small computer asks the first for the
numbers, publishes them to a web page, and decides when to switch something off
through a remotely controlled switch.

**Nothing here runs on mains voltage.** 12 V DC is car-battery territory —
harmless to touch. The protocols, the control problem and the engineering are
identical to the mains version; only the danger is removed. That was a
deliberate scoping decision, not a limitation we hit by accident.

---

## 3. The three-floor design

This project is deliberately laid out like Schneider Electric's **EcoStruxure**
architecture, which organises energy systems into three layers. Think of it as
a three-floor building.

### Floor 1 — the devices that measure ("Connected Products")

The sensor and the small computer attached to it. Its only job is to measure
power accurately and keep a truthful running total of energy. It does not decide
anything.

It publishes its readings in a standard industrial format so that *any* system
can read them, not just ours. Schneider's equivalent products are their
**PowerLogic** meters.

→ In this repo: **`meter_node/`**

### Floor 2 — the box that collects and decides ("Edge Control")

This floor asks Floor 1 for its numbers, passes them up to Floor 3, and — the
important part — **makes the switching decisions itself**.

Why does that matter? Because the internet goes down. If the decision to shed a
load lived in the cloud, then a dropped connection would mean no control at all.
By keeping the control logic on-site, the system keeps protecting against spikes
even with the network completely unplugged. Schneider's equivalent product is
the **EcoStruxure Panel Server**.

→ In this repo: **`gateway_node/`**

### Floor 3 — the screens and the analysis ("Apps, Analytics & Services")

Charts, running costs, CO2 figures, history. This is where a human looks. It is
useful, but nothing here is allowed to be *required* — Floor 2 must survive
without it. Schneider's equivalent is **Power Monitoring Expert**.

→ In this repo: **`dashboard/`**

### Why bother with three floors?

Because each floor can be replaced without touching the others. Swap the sensor
on Floor 1 and Floors 2 and 3 never notice. Unplug Floor 3 entirely and the
system still controls loads. That separation is the whole point of the design,
and it is why industrial systems are built this way.

---

## 4. How the data actually travels

```
  [ loads ]
      |                            FLOOR 1  -- meter_node
  [ sensor ] --> [ telemetry ring buffer ] --> [ Modbus register map ]
                        |                               |
                 [ future: NILM ]                 two-wire cable
                                                        |
                                              FLOOR 2  -- gateway_node
                                        [ Modbus master / poller ]
                                                        |
                        +-------------------------------+------------------+
                        |                               |                  |
              [ shed/restore control ]        [ MQTT publish ]     [ cost / CO2 ]
                        |                               |
                   [ relays ]                   FLOOR 3  -- dashboard
```

Step by step, for one reading:

1. The sensor measures volts and amps and works out watts.
2. The meter's computer records it, adds it to the energy total, and drops it
   into a shared memory area (the **ring buffer**, explained below).
3. The meter copies the latest figures into **numbered pigeonholes** called
   registers, then waits. It never speaks unless asked.
4. The gateway sends a question down the two-wire cable: *"device 1, give me
   pigeonholes 16 to 26."* The meter answers. This question-and-answer format is
   **Modbus-RTU**, the most widely used language in industrial equipment, and
   the cable standard underneath it is **RS-485**.
5. The gateway publishes the numbers over WiFi to a small message service
   (**MQTT**), which the web page is subscribed to, so the chart updates.
6. At the same moment the gateway checks its rules — *is power too high? is this
   an expensive hour?* — and if needed switches off the least important load.
7. The load stops drawing power. The sensor sees the drop. **The loop closes.**

Nobody touched anything. That is the "autonomous" part, and it is the difference
between a monitoring project and a control project.

---

## 5. The one design decision that shapes everything

Look again at step 2: every reading goes into a **ring buffer** — a fixed-size
notebook that overwrites its oldest page when it runs out, like CCTV loop
recording.

Everything else in the system reads from that one notebook, and each reader
keeps **its own bookmark**.

This matters for three reasons:

**Nothing can block the measurement.** The part that talks to WiFi can freeze
for ten seconds and the sensor keeps being read on time, because the measuring
code never waits for anybody.

**Adding a feature doesn't disturb the others.** A new consumer just opens its
own bookmark. The sampler is not modified, and the existing readers cannot tell
anyone joined.

**Lost data is reported, never hidden.** If a reader falls so far behind that
its pages get overwritten, the system counts exactly how many readings it lost
and says so. Silently losing data is what makes an energy log untrustworthy,
and an untrustworthy log is worthless.

There is a fourth reason, and it is the long game. The final stretch goal
(**NILM** — working out *which* appliance switched on purely from the shape of
the total power signal) needs the fast, raw, unfiltered stream, including the
brief spike when a motor starts. The web page only needs one reading per second,
so it is very tempting to throw the rest away early on. Do that, and the stretch
goal means rewriting everything.

So: keep every reading, in one place, from day one. That decision was made
before any other code was written.

---

## 6. What is built right now

**Phase 0 is complete and tested.**

| What | Status |
|---|---|
| Power measurement at 10 readings/second | working |
| Running energy total (watt-hours) | working, time-accurate |
| Shared ring buffer with independent readers | working, 6 tests passing |
| Lost-data detection and counting | working |
| Replayable log format | proven: 1,800 readings, 0 lost |
| Chart tool | `tools/replay.py` |
| Appliance simulator with known correct answers | three loads |
| Builds for real hardware | warning-free |
| Builds and runs on a PC | no hardware needed |

### The sensor has a stand-in

The real sensor chip has not arrived yet, so there are **two interchangeable
versions** of it behind one interface: the real chip, and a simulator. One build
setting switches between them. Nothing else in the project changes.

That is not a workaround — it is how testable embedded software is written. It
also means the whole system can be built and verified before any part arrives.

### The simulator earns its keep twice

Three simulated loads, each behaving the way the real thing does:

| Load | Steady power | Its distinctive behaviour |
|---|---|---|
| lights | 5 W | switches on cleanly, no fuss |
| heater | 40 W | power fades ~6% as the element warms up and resists more |
| motor | 18 W | gulps 3.5x normal power for ~1/4 second at startup, then ripples |

Those behaviours are not decoration. They are exactly the **fingerprints** the
NILM stretch goal will use to tell appliances apart. And because the simulator
knows which loads it switched on, every reading is stored with the **correct
answer** attached — so the classifier can be scored honestly, long before it
ever meets a real appliance.

### The number that matters

From a 180-second recording:

```
power      0.00 .. 68.17 W   (average 26.57 W)
peak/mean  2.57x
```

Power spikes to **2.57x the average**. That is the "before" figure. The whole
job of the control phase is to shrink it — and because it was measured rather
than guessed, the improvement will be a real number.

---

## 7. What comes next

| Phase | What it adds | Why it matters |
|---|---|---|
| **1** | Modbus-RTU over RS-485; register map; device model | The industrial language. The single most employable skill here. |
| **2** | MQTT gateway, web dashboard, **store-and-forward**, real clock time | Readings buffer during a network outage and **backfill when it returns** — the feature that separates a product from a toy. |
| **3** | Demand-response control: **rolling demand window**, tariff schedule, alarms | Bills are based on a 15-minute average, not the instant. So the controller forecasts where the window will land and acts early — not a simple threshold. |
| **3.5** | Modbus-TCP gateway | Industrial gateways translate between both Modbus forms. Cheap to add once Phase 1 works. |
| **4** | TLS, access control, security notes against **IEC 62443** | The standard real energy equipment is certified against. |
| **5** | **NILM** — identify appliances from one sensor | The differentiator. See below. |

### Where this goes beyond the reference architecture

Two places, deliberately.

**Identifying appliances from a single meter.** Schneider's answer to "which
load is running?" is **PowerTag** — a wireless sensor clipped onto *every
individual circuit breaker*. One sensor per circuit. Phase 5 here does it with
**one** meter and a trained classifier.

That is not a claim to have beaten them. Per-circuit sensing is more accurate,
and they sell hardware, so of course that is their answer. The point is the
trade-off: **installation cost and retrofit difficulty versus accuracy.** Fitting
a sensor to every breaker in an existing building is expensive and disruptive,
and that cost is the real barrier to energy monitoring getting deployed. Doing
it in software from one meter is the cheaper, less accurate alternative. Knowing
which trade-off you are making is the useful part.

**Control that survives the cloud.** The shedding decision lives on the edge
device, not in software upstream. The demonstration is simple and blunt:
**unplug the network and watch it keep working.**

---

## 8. Honest limitations

- Low-voltage DC only. No mains, and therefore none of AC's real difficulties:
  power factor, harmonics, true-RMS measurement, phase imbalance.
- The real sensor driver is written from the datasheet but has **never run on
  actual silicon**. Expect to debug it when the part arrives.
- Accuracy has not been checked against a reference multimeter yet.
- The energy total starts from zero at power-on and is not saved across resets.
- Timestamps are currently "milliseconds since switch-on", which is useless for
  billing or tariffs. Real clock time arrives in Phase 2.
