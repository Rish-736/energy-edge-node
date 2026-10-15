# Architecture — in plain language

What this project is, how it is put together, and why it is built that way.
It assumes no background in electronics or software.

---

## 1. What problem is this solving?

Electricity bills for businesses have **two** separate charges:

1. **Energy charge** — how many units you used all month.
2. **Demand charge** — based on your **single highest 15-minute burst** of power.

The second one surprises people. A business can use modest power all month,
switch several big things on at the same moment once, and be billed against that
one spike for the entire month.

At an **electric-vehicle charging site** this gets extreme: the demand charge is
reported to be **30–70% of the whole electricity bill**. One bad quarter-hour
sets it.

So there is real money in a device that watches power use and **automatically
switches off whatever can wait**, before a spike forms.

---

## 2. The part that makes this honest: *what* can wait

A lot of demand-response demos shed a home water heater. That is a bad trade and
the field data proves it: in a trial of 191 households on grid-controlled water
heaters, **about 40% reported cold water** and two people quit the programme.

The reason it fails is simple — **the occupant never declared a deadline.** They
might want hot water in five minutes. You have no idea. So any power you take
away might be power they needed.

An EV charger is the opposite:

> A car plugged in at 7 pm and needed at 7 am wants maybe **four hours** of
> charge inside a **twelve-hour** window.

That leaves **eight hours of slack**. The driver's requirement is not "charge
right now" — it is **"full by morning."** Move the charging around inside that
slack and nobody notices anything at all.

**That slack is the resource this whole project spends.**

So the controller never asks *"whose comfort do I sacrifice?"* It asks:

> *Each load needs this much energy by this time. What is the schedule that
> meets every deadline while keeping the biggest 15-minute burst as small as
> possible?*

Service stops being a trade-off and becomes a **promise the controller is not
allowed to break**.

---

## 3. What is physically here?

A 12 V bench supply stands in for the grid. Three loads stand in for the site:

| On the bench | What it stands for | Can it wait? |
|---|---|---|
| a 40 W power resistor | **an EV charger** — needs X energy by a deadline | yes, with a deadline |
| a small 18 W motor | **a thermal load** (cooling, pumping) | yes, within limits |
| 5 W of LEDs | **base load** — lighting, controls | **never** |

A sensor in the wire measures the power. One small computer reads that sensor.
A second small computer asks the first for the numbers, publishes them to a web
page, and decides when to switch something off through a remote-controlled
switch.

**Nothing here runs on mains voltage.** 12 V DC is car-battery territory —
harmless to touch. The protocols, the control problem and the engineering are
identical to the mains version; only the danger is removed. That was deliberate
scoping, not an oversight.

---

## 4. The three-floor design

This is laid out like Schneider Electric's **EcoStruxure** architecture, which
organises energy systems into three layers. Think of it as a three-floor
building.

### Floor 1 — the device that measures ("Connected Products")

The sensor and the computer attached to it. Its only job is to measure power
accurately and keep an honest running energy total. **It decides nothing.**

It publishes its readings in a standard industrial format so that *any* system
can read them, not just ours. Schneider's equivalent is their **PowerLogic**
meters.

→ **`meter_node/`**

### Floor 2 — the box that collects and decides ("Edge Control")

Asks Floor 1 for its numbers, passes them to Floor 3, and — the important part —
**makes the switching decisions itself**.

Why does that matter? Because the internet goes down. If the decision lived in
the cloud, a dropped connection would mean no protection at all. Keeping the
logic on site means it keeps working with the network unplugged. Schneider's
equivalent is the **EcoStruxure Panel Server**.

→ **`gateway_node/`**

### Floor 3 — the screens ("Apps, Analytics & Services")

Charts, running cost, CO2, history. Where a human looks. Useful, but nothing
here is allowed to be *required* — Floor 2 must survive without it. Schneider's
equivalent is **Power Monitoring Expert**.

→ **`dashboard/`**

### Why bother with three floors?

Each floor can be replaced without touching the others. Swap the sensor and
Floors 2 and 3 never notice. Unplug Floor 3 entirely and control continues.
That separation is the whole point, and it is why industrial systems are built
this way.

---

## 5. How the data actually travels

```
  [site loads]
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
              [ deadline-aware control ]      [ MQTT publish ]     [ cost / CO2 ]
                        |                               |
                   [ relays ]                   FLOOR 3  -- dashboard
```

One reading, step by step:

1. The sensor measures volts and amps and works out watts.
2. The meter's computer records it, adds it to the energy total, and drops it
   into a shared memory area (the **ring buffer**, explained next).
3. The meter copies the latest figures into **numbered pigeonholes** called
   registers, then waits. It never speaks unless asked.
4. The gateway sends a question down the two-wire cable: *"device 1, give me
   pigeonholes 16 to 54."* The meter answers. That question-and-answer format is
   **Modbus-RTU**, the most widely used language in industrial equipment; the
   cable standard underneath it is **RS-485**.
5. The gateway stamps the reading with real clock time and publishes it over
   WiFi to a small message service (**MQTT**), which the web page is subscribed
   to, so the chart updates.
6. The gateway's controller checks its schedule — *are we heading for a peak? Is
   there slack left before this load's deadline?* — and if so switches it off.
7. The load stops drawing power. The sensor sees the drop. **The loop closes.**

Nobody touched anything. That is the difference between a *monitoring* project
and a *control* project.

---

## 6. The one design decision that shapes everything

Look again at step 2: every reading goes into a **ring buffer** — a fixed-size
notebook that overwrites its oldest page when full, like CCTV loop recording.

Everything else reads from that one notebook, and each reader keeps **its own
bookmark**.

Three reasons this matters:

**Nothing can block the measurement.** The part that talks to WiFi can freeze for
ten seconds and the sensor is still read on time, because the measuring code
never waits for anybody.

**Adding a feature doesn't disturb the others.** A new consumer just opens its
own bookmark. The sampler is not modified and the existing readers cannot tell
anyone joined.

**Lost data is reported, never hidden.** If a reader falls so far behind that its
pages get overwritten, the system counts exactly how many readings it lost and
says so. Silently losing data is what makes an energy log untrustworthy, and an
untrustworthy log is worth nothing.

> **Why not a queue per reader?** With queues the producer must push into every
> one. If a consumer stops reading, its queue fills, and the push either blocks
> (the sampler stalls and misses readings) or fails (the reading is lost for
> everyone). **With queues, a stuck consumer can stall the producer. With a ring
> and bookmarks, a stuck consumer only hurts itself.**

There is a fourth reason, and it is the long game. The final stretch goal
(**NILM** — working out *which* load switched on purely from the shape of the
total power signal) needs the fast, raw, unfiltered stream, including the brief
spike when a motor starts. The web page only needs one reading per second, so it
is very tempting to throw the rest away early on. Do that, and the stretch goal
means rewriting everything.

So: keep every reading, in one place, from day one.

---

## 7. What is built right now

**Phases 0 to 3 are complete and tested. 52 automated tests, all running on a
PC with no hardware attached.**

### Phase 0 — measuring

| What | Status |
|---|---|
| Power measured 10 times a second | working |
| Running energy total (watt-hours) | working, time-accurate |
| Shared ring buffer with independent readers | working, 6 tests |
| Lost-data detection and counting | working |
| Replayable log format | proven: 1,800 readings, 0 lost |
| Chart tool | `tools/replay.py` |

**The sensor has a stand-in.** The real chip has not arrived, so there are **two
interchangeable versions** behind one interface: the real chip, and a simulator.
One build setting switches them. That is not a workaround — it is how testable
embedded software is written, and it meant the whole system could be built
before any part arrived.

**The simulator earns its keep twice.** The three loads behave like the real
things — a clean step, a slow droop as an element warms, a motor gulping 3.5×
its normal power for a quarter-second at startup. Those behaviours are the
**fingerprints** the NILM stretch goal will use, and because the simulator knows
which loads it switched on, every reading is stored with the **correct answer**
attached. It is the training rig, not scaffolding.

### Phase 1 — speaking the industrial language

The meter answers **Modbus-RTU** over an **RS-485** cable; the gateway asks.
39 registers fetched in a single question, ten times a second.

The clever part: **the protocol code knows nothing about cables.** You hand it a
message and a reply box. So a "master" and a "slave" can talk *through memory*,
and the entire conversation — checksums, message framing, error replies — is
tested on a PC. In the test file, the RS-485 cable is a function call.

The gateway also **discovers** meters rather than being told about them: at
startup it asks every address on the bus "who are you?" and adds whoever
answers correctly.

### Phase 2 — surviving a dropped network

The gateway publishes every reading to a message broker, and a web page charts
it live. The feature worth demonstrating is **store-and-forward**:

> Pull the WiFi. The chart stops and a gap opens. Plug it back in — **the gap
> fills in**, retroactively, because the readings were buffered the whole time.

The implementation is deliberately tiny: the publisher is just another reader of
the ring buffer that **refuses to move its bookmark while disconnected**. When
the link returns it catches up from where it stopped. That is how an industrial
gateway behaves.

It also states its own limit honestly: the buffer holds about 51 seconds. A
longer outage is real data loss, and the system reports exactly how many
readings went missing rather than papering over the gap.

### Phase 3 — deciding what can wait

The controller defends a demand target by deferring loads into their slack. Run
over an identical workload twice, once off and once on:

```
                     baseline   controlled
peak demand            62.2 W      26.0 W     58% lower
charge delivered        1.506 Wh    1.506 Wh  identical
deadline misses             0           0
```

The car left with exactly the same charge. The controller changed *when* the
energy was drawn, not how much the driver got.

It works off a **forecast** — where the billing interval will land if nothing
changes — rather than the present reading, because by the time the average
crosses the limit it is too late to pull it back. And it picks which load to
shed by **slack** (how long it can wait and still finish), so a load running
out of time protects itself automatically.

### The number that matters

From a 180-second recording:

```
power      0.00 .. 68.17 W   (average 26.57 W)
peak/mean  2.57x
```

Power spikes to **2.57× the average**. That is the "before" figure — measured,
not guessed — so the improvement Phase 3 produces will be a real number.

---

## 8. What comes next

| Phase | What it adds | Why it matters |
|---|---|---|
| **3** | **Deadline-aware controller** — rolling 15-minute demand window, each load's energy requirement and deadline, staggered restore | Bills are based on a 15-minute *average*, so the controller forecasts where the window will land and acts early. And it must meet every deadline while doing it. |
| **3.5** | Modbus-TCP gateway | Industrial gateways translate between both forms of Modbus. Cheap once Phase 1 works. |
| **4** | TLS, access control, notes against **IEC 62443** | The standard real energy equipment is certified against. |
| **5** | **NILM** | Identify loads from one meter — *and* verify the controller's commands actually worked. |

### Where this goes beyond the reference architecture

**Deadlines instead of comfort trading.** Most demand-response systems ask "what
can I switch off?" This one asks "what can wait, and until when?" — and then
guarantees it does not miss. **Zero deadline misses** is a claim you can check.

**Verifying that a command actually worked.** This is the most interesting idea
in the project, and it needs explaining:

> A relay is **open-loop**. You command it off and get no confirmation back.
> Contacts weld shut. Channels get miswired. Someone unplugs the load. The
> software marks it "off" and moves on.
>
> So the controller believes it shed 40 W, believes the peak is handled, and
> stops looking for more to shed. The spike happens anyway and **nothing in the
> system ever notices.** The controller has lied to itself.
>
> Phase 5 fixes that using **the meter that is already there**: command the load
> off, then check the aggregate signal. Did total power actually drop by about
> the right amount, with the right shape? If not, raise an alarm.

That closes the loop using a sensor installed for a completely different reason
— no extra wiring, no feedback contact, no sensor per circuit.

**Retrofit-first.** Schneider's answer to "which load is running?" is
**PowerTag** — a wireless sensor clipped onto *every individual breaker*. One
sensor per circuit. This project does it with **one** meter and a classifier.
That is not a claim to have beaten them: per-circuit sensing is more accurate,
and they sell hardware. The point is the trade-off — **installation cost versus
accuracy** — and installation cost is the real barrier to getting energy
monitoring deployed at all.

---

## 9. Honest limitations

- Low-voltage DC only. No mains, and therefore none of AC's real difficulties:
  power factor, harmonics, true-RMS, phase imbalance.
- The real sensor driver and the RS-485 cable layer are written from the
  datasheets and have **never run on actual hardware**. Everything above them is
  tested; those two layers are not.
- The MQTT and clock-sync paths compile and their logic is unit-tested, but they
  have **not run against a live broker**.
- Buffer state for deferrable loads will be **modelled, not measured** — there is
  no temperature or charge sensor on the bench. The interface is built so a real
  one drops in.
- Accuracy has not been checked against a reference multimeter.
- The energy total starts at zero on power-up and is not saved across resets.
- Cost and CO2 figures use flat assumed rates. They are assumptions, not
  measurements.
