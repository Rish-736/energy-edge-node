# Modbus register map — meter node

Device: `energy-edge-node / meter_node`
Map version: **1**
Vendor ID: `0x4545` ("EE") · Product ID: `0x0001`

The authoritative copy of these addresses is
[`common/modbus/meter_regs.h`](../common/modbus/meter_regs.h), which both the
slave and the master compile against, so the two ends of the bus cannot drift
apart. A machine-readable version is in
[`docs/device_model.json`](device_model.json).

---

## Link settings

| Setting | Value |
|---|---|
| Physical layer | RS-485 half duplex, 2-wire |
| Baud rate | 19200 |
| Framing | 8N1 |
| Unit ID | 1 (configurable) |
| Inter-frame gap (t₃.₅) | ~2.0 ms |
| Function codes | `0x03`, `0x04` read · `0x06`, `0x10` write |

---

## Two conventions worth stating explicitly

These two details cause most Modbus integration failures. Both are decided
here and enforced by unit tests.

### Addressing is 0-based (protocol addresses)

Every address in this document is the value that **actually travels on the
wire**. Much vendor documentation instead prints 1-based "register numbers",
where holding register `40001` means protocol address `0`. If your master
expects that style, add 40001.

### Float32 is high word first

A float occupies two consecutive registers, **most significant word first** —
matching Schneider PowerLogic meters.

`1.0f` is `0x3F800000`, so it transmits as:

```
register N     = 0x3F80     <- high word
register N + 1 = 0x0000     <- low word
```

Get this backwards and every float comes out as a wild magnitude. The test
`test_float_word_order_is_high_word_first` exists purely to stop that
regressing.

---

## Register map

### Identity — `0x0000`–`0x0004` (read)

| Addr | Qty | Name | Type | Meaning |
|---|---|---|---|---|
| `0x0000` | 1 | `vendor_id` | uint16 | always `0x4545` |
| `0x0001` | 1 | `product_id` | uint16 | `0x0001` = meter node |
| `0x0002` | 1 | `fw_version` | uint16 | `major << 8 \| minor` |
| `0x0003` | 1 | `map_version` | uint16 | version of *this* map |
| `0x0004` | 1 | `capabilities` | uint16 | bitfield, see below |

**Capability bits**

| Bit | Name | Meaning |
|---|---|---|
| 0 | `SIMULATED` | readings come from the simulator, **not real hardware** |
| 1 | `FLOAT32` | float block present |
| 2 | `SCALED` | scaled-integer block present |
| 3 | `WRITABLE` | accepts `0x06` / `0x10` |

Bit 0 matters: a master can tell at a glance whether it is looking at real
measurements or synthetic ones. Data that silently claims to be real when it
is not is worse than no data.

### Measurements, float32 — `0x0010`–`0x0017` (read)

| Addr | Qty | Name | Type | Unit |
|---|---|---|---|---|
| `0x0010` | 2 | `voltage` | float32 | V |
| `0x0012` | 2 | `current` | float32 | A |
| `0x0014` | 2 | `power` | float32 | W |
| `0x0016` | 2 | `energy` | float32 | Wh |

`power` comes from the INA226's dedicated power register, not from V × I
computed afterwards — the sensor integrates both channels over the same
conversion window, which is more accurate than multiplying two separately
sampled values.

### Measurements, scaled integers — `0x0020`–`0x0024` (read)

The same four values as fixed-point integers, for masters without float
support. Plenty of PLCs and budget HMIs fall in that category, and real meters
publish both for exactly this reason.

| Addr | Qty | Name | Type | Scale | Range |
|---|---|---|---|---|---|
| `0x0020` | 1 | `voltage_x100` | uint16 | ÷100 → V | 0 … 655.35 V |
| `0x0021` | 1 | `current_ma` | int16 | ÷1000 → A | ±32.767 A |
| `0x0022` | 1 | `power_x10` | int16 | ÷10 → W | ±3276.7 W |
| `0x0023` | 2 | `energy_mwh` | uint32 | ÷1000 → Wh | 0 … 4.29 MWh |

Values are **clamped** to these ranges, not wrapped. A sensor fault or a NaN
therefore shows up as a saturated reading rather than as a plausible-looking
wrong number.

### Status — `0x0030`–`0x0036` (read)

| Addr | Qty | Name | Type | Meaning |
|---|---|---|---|---|
| `0x0030` | 1 | `status_flags` | uint16 | bitfield, see below |
| `0x0031` | 1 | `truth_mask` | uint16 | which loads are really on (simulator only) |
| `0x0032` | 2 | `sample_seq` | uint32 | monotonic sample counter |
| `0x0034` | 2 | `uptime_s` | uint32 | seconds since boot |
| `0x0036` | 1 | `overruns` | uint16 | samples the slave's own Modbus reader lost |

**Status flag bits**

| Bit | Name | Meaning |
|---|---|---|
| 0 | `SENSOR_FAIL` | sensor read failed; values are stale |
| 1 | `OVER_POWER` | above the demand-response threshold |
| 7 | `SIMULATED` | this sample is synthetic |

**`sample_seq` is how a master detects stale data.** If the counter stops
advancing between polls, the meter is alive on the bus but has stopped
sampling — a fault that reading the power value alone cannot reveal, because
a frozen value and a genuinely steady load look identical.

**`overruns`** exposes an internal fault externally: the slave reporting that
it is losing samples inside itself.

### Control — `0x0040` (read/write)

| Addr | Qty | Name | Type | Access |
|---|---|---|---|---|
| `0x0040` | 1 | `cmd` | uint16 | read/write |

| Value | Action |
|---|---|
| `1` | reset the energy accumulator to zero |
| `0` | clear a pending command |

Any other value returns exception `0x02`. **Writes to any other address are
rejected** — measurements are physical facts, and a master that could
overwrite them could make the device report a comfortable lie.

---

## Block polling

Reserved addresses inside `0x0000`–`0x0040` read back as `0` rather than
raising an error, so a master can fetch everything it needs in **one**
transaction instead of four:

| | |
|---|---|
| Function code | `0x04` (read input registers) |
| Start address | `0x0010` |
| Quantity | `39` registers (`0x0010`–`0x0036`) |

That is what `gateway_node` does on every poll. Addresses outside the mapped
span still raise exception `0x02`, so genuine mistakes are still caught.

---

## Exceptions

| Code | Name | Raised when |
|---|---|---|
| `0x01` | Illegal function | function code other than 3/4/6/16 |
| `0x02` | Illegal data address | address outside the map, or a write to a read-only register |
| `0x03` | Illegal data value | quantity of 0 or > 125, or a malformed frame |

### When the slave stays silent instead

Three cases get **no reply at all**, which is correct Modbus behaviour:

- **Bad CRC.** The frame may not even have been addressed to us — and
  answering corrupted traffic turns line noise into bus traffic.
- **Another device's unit ID.**
- **Broadcast (unit 0).** A write is still applied, but nothing is sent back:
  on a half-duplex bus, several slaves answering at once would collide.

---

## Diagnostic counters

Both ends keep their own counters, visible on the serial console.

**Slave:** `requests`, `crc_errors`, `exceptions`, `ignored`, `frames_tx`
**Master, per device:** `polls_ok`, `timeouts`, `crc_errors`, `exceptions`,
average and worst-case round-trip latency

These are what you actually look at when a bus misbehaves. "The cable is bad"
is a guess; "unit 3 shows 4% CRC errors while units 1 and 2 show none" points
at a specific cable run.

---

## Bring-up checklist

When the MAX485 modules arrive, in this order:

1. **A to A, B to B.** Swapped A/B is the single most common fault and looks
   exactly like a dead bus. If discovery finds nothing, try swapping them.
2. **Share a ground** between both nodes. RS-485 is differential but not
   magic; without a common reference the receivers can sit outside their
   input range.
3. **120 Ω termination** at both physical ends of the bus. On a short bench
   cable it usually works without, but it is the correct thing to fit.
4. **Confirm both ends agree** on 19200 8N1.
5. Watch the gateway's serial output for `# found unit 1: ...`.
6. **Check the `SIMULATED` capability bit** so you know whether you are
   looking at real sensor data or the simulator.
