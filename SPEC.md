# Nonolith Connect — Behavioral Specification

*Written as the reference for a Rust port. Describes what the C++ implementation
(this repository, as of 2026-07) actually does, including quirks clients depend
on. Companion documents: [REST_API.md](REST_API.md) and [WS_API.md](WS_API.md)
(wire-level detail), and `tests/` (executable spec: 158 doctest cases +
`tests/e2e.py`). Where this document and the tests disagree, the tests win.*

---

## 1. Purpose and scope

Nonolith Connect is a background daemon that owns USB communication with
measurement devices — the Nonolith CEE and the Analog Devices ADALM1000 ("M1K"),
both 2-channel source-measure units — and exposes them to multiple simultaneous
clients over HTTP/REST and WebSocket on `localhost:9003`. The primary client is
Pixelpulse (`../pixelpulse`, TypeScript). Its `src/dataserver-webusb.ts` is a
faithful in-process reimplementation of the WebSocket protocol and is a useful
second reference.

The port must preserve wire compatibility: an unmodified Pixelpulse build must
work against the Rust server.

---

## 2. Process architecture

The C++ server has exactly **two threads**:

1. **Main/event thread** — a single `boost::asio::io_context` runs everything:
   TCP accept, HTTP request handling, WebSocket reads/writes, and all device
   state mutation. Because it is single-threaded, the domain model
   (devices, channels, streams, listeners) needs no locking.
2. **USB thread** — loops on `libusb_handle_events()`. libusb transfer
   callbacks run here; they post completed IN buffers to the io_context
   (`boost::asio::post`) and refill/resubmit OUT buffers **in place** on the
   USB thread. Two mutexes per device (`outputMutex`, `transfersMutex`)
   protect the small state shared across the boundary: the output source +
   `capture_o` counter, and the transfer arrays.

Hotplug events (libusb hotplug callbacks, USB thread) are posted to the event
thread, which constructs/destroys device objects and fires
`device_list_changed`.

**Port note.** The natural Rust mapping is one async runtime task per concern
with the domain model living in a single task (actor) or behind a single
`Mutex`; the hard requirement is only that *broadcast ordering is preserved*:
state change → notification broadcast happens atomically with respect to other
client commands. The OUT-transfer refill path is latency-sensitive (it runs
between USB completions); keep waveform evaluation lock-scoped and allocation-free
as the C++ does.

### Global state

- `devices: Set<Device>` — currently connected devices.
- `device_list_changed` — event; every WebSocket connection subscribes and
  re-sends the device list on each notification.
- Flags from argv: `debug` (dump JSON traffic), `allow-remote` (bind 0.0.0.0
  instead of 127.0.0.1), `allow-any-origin` (skip origin check), `port=N`
  (default 9003).

---

## 3. Network layer

### 3.1 TCP / HTTP

- Listens on 127.0.0.1:9003 (0.0.0.0 with `allow-remote`), `SO_REUSEADDR`.
- 30-second timeout for reading the initial request; no timeout afterwards.
- Responses:
  - **Complete** responses: `Content-Type: application/json`,
    `Server: Nonolith Connect`, `Connection: close`; socket is shut down after
    the response. Every REST response is one-shot except stream CSV.
  - **Streaming** responses (`GET .../input` CSV): headers sent immediately
    with `Transfer-Encoding: chunked`, `Content-Type: text/plain`,
    keep-alive; body chunks are written as data arrives; final empty chunk +
    close when the listener completes.
- `GET /` → `301` redirect to the nonolithlabs.com marketing URL.
- Anything that is neither `/` nor `/rest/...` nor a WS upgrade of `/ws/...`
  → `404`.

### 3.2 Origin policy

Applied to **HTTP requests only** (see quirk Q9): if `allow-any-origin` is not
set and the request carries an `Origin` header, it is rejected with `403` and
body `Origin not allowed` unless the origin is:

- empty or the literal `null`, or
- `^https?://localhost(:[0-9]+)?$`, or
- `^https?://[\w.-]*?nonolithlabs.com$`.

Every REST response also echoes the request `Origin` back as
`Access-Control-Allow-Origin` (set unconditionally on `/rest` paths, even when
no Origin was sent — the header is then empty).

### 3.3 WebSocket

- Upgrade endpoint: any first path segment `ws` (`/ws/v0` in practice).
  Upgrades on other paths are treated as plain HTTP (a WS client sees the
  handshake fail).
- Text frames only from clients (binary client frames are ignored). Server
  sends both text (JSON) and binary (listener update) frames.
- Writes are queued per connection and sent in order; interleaving of text and
  binary frames preserves enqueue order.

---

## 4. REST API

Routing (see `REST_API.md` for full parameter tables):

```
/rest                          -> 404 "No API version selected."
/rest/v1/                      -> {server, version, gitVersion}
/rest/v1/devices               -> {<id>: {id, model, hwVersion, fwVersion, serial}, ...}
/rest/v1/devices/<id>/...      -> device handler; 404 "REST object not found" if
                                  the id is unknown or the device path unhandled
/rest/<anything else>          -> 404 "API version not supported"
```

Device paths (StreamingDevice):

```
GET  /                    full state JSON        POST /               capture=on|off|true|false|1|0
GET  /configuration       config-only JSON       POST /configuration  sampleTime, samples, currentLimit
GET  /<ch>                channel JSON
GET  /<ch>/output         source description     POST /<ch>/output    form or JSON body (see 6.4)
GET  /<ch>/input          CSV stream             POST /<ch>/input     gain_<streamId>=<float>
```

Plus CEE-specific `/gpio` and M1K-specific `/frontend`, `/power`,
`/temperature`, `/leds`, `/calibration`, `/serial` (see §7).

Semantics that must be preserved:

- **Device ids** are `<model>~<serial>` (e.g. `com.analogdevices.m1k~2039...`).
  A trailing `*` matches by model prefix: `com.analogdevices.m1k*` selects the
  first device of that model (set iteration order — effectively arbitrary but
  stable per registration).
- **POST bodies** are parsed as form-urlencoded key=value pairs split on `&`
  and `=`, *without* general URL-decoding (only `%3A`/`%2C` are decoded, and
  only inside the arb `points` parameter). If the body's first byte is `{`, it
  is parsed as JSON instead (implemented for `/output`; the JSON branch of
  `POST /` and `POST /configuration` is a silent no-op that still returns
  state).
- **Error responses** from handler exceptions use HTTP status **402** with body
  `{"error": "<message>"}` (yes, 402 — Pixelpulse doesn't care, but keep it or
  consciously change it; see Q4).
- **`POST /configuration`** clamps `sampleTime` to at most `0.001` s and
  ignores non-positive values; passes `currentLimit` to the device (CEE
  accepts 200/400/2000 mA, others ignored); reconfiguring clears captured data
  and all listeners.
- **CSV stream** (`GET /<ch>/input`): query params `resample` (seconds,
  default 0.01; decimation factor = `round(resample/sampleTime)`, min 1),
  `start` (default −1; negative = relative to latest, clamped to 0), `count`
  (default 1; `0` = unlimited; **negative values throw** — currently an
  unhandled 500, see Q5), `header` (default 1). Header row is
  `<displayName> (<units>)` joined by `,`; data rows are stream values joined
  by `", "`, one row per decimated chunk, values formatted with C++ default
  ostream precision (6 significant digits). The response stays open until
  `count` rows have been sent or the client disconnects.

JSON bodies are emitted by libjson `write_formatted()` (pretty-printed with
tabs). Clients parse JSON, so the Rust port may format however serde does.

---

## 5. WebSocket API

See `WS_API.md` for the full command/action tables. The envelope:

- Client→server: JSON with `_cmd` discriminator, optional integer `id` echoed
  in `return` replies.
- Server→client: JSON with `_action` discriminator, or a binary frame (§6.6).

### Connection lifecycle

1. On connect the server immediately sends
   `{"_action":"serverHello","server":"Nonolith Connect","version":"1.3","gitVersion":...}`
   followed by `{"_action":"devices","devices":{...}}`.
2. `devices` is re-sent to every connection whenever the device list changes.
3. The client must send `{"_cmd":"selectDevice","id":...}` before any other
   command. Selecting attaches the connection to the device, which immediately
   sends `deviceConfig` (full state). Commands sent before selection are
   logged and dropped (no reply). Unknown device ids in `selectDevice` are
   logged and ignored.
4. On disconnect the connection is detached: its listeners are removed from
   the device.

### Command dispatch order

`selectDevice` (connection layer) → device-specific handler (M1K/CEE/bootloader)
→ `USB_device` (`controlTransfer`, `enterBootloader`) → `StreamingDevice`
(`listen`, `cancelListen`, `configure`, `startCapture`, `pauseCapture`, `set`,
`setGain`, `setCurrentLimit`). An unmatched command is logged to stderr and
gets **no reply** (Q6).

### Errors

Any exception while processing a message (parse error, missing property,
unknown channel...) produces
`{"_action":"error","error":"<what>"}` plus `"id"` if the request carried a
non-zero id. The connection stays open.

### Broadcasts

State changes broadcast to **all connections attached to that device** (not
just the requester):

| `_action` | When |
|---|---|
| `deviceConfig` | on attach; after `configure` |
| `captureState` `{state,done}` | start/pause/completion |
| `captureReset` | capture counters reset (also resets listeners' indices) |
| `outputChanged` `{channel, ...source}` | output source set; again with `effective:true` once its effect is visible in input (§6.4) |
| `gainChanged` `{channel,stream,gain}` | gain change |
| `deviceDisconnected` | USB device removed |
| `packetDrop` | CEE only: firmware reported a dropped packet |

---

## 6. Streaming device model

This is the heart of the server; port it precisely.

### 6.1 Structure

A `StreamingDevice` has ordered `channels` (id `a`, `b`), each with ordered
`streams` (id `v`, `i`) and one owned `OutputSource`. Stream metadata: `id`,
`displayName` (e.g. "Voltage A"), `units` ("V"/"mA", "LSB" in raw mode),
`min`/`max` (display range), `outputMode` (which source mode drives this
variable: 1 for voltage, 2 for current), `gain` / `normalGain` (reported
`gain = gain/normalGain`), `uncertainty`.

State fields serialized in `deviceConfig`/`GET /` (all at top level):
`id, model, hwVersion, fwVersion, serial, sampleTime, minSampleTime, mode,
samples, length (=samples*sampleTime), continuous, raw, currentLimit,
captureState, captureDone, channels{...}`. `GET /configuration` returns the
subset without identity/capture/channels.

### 6.2 Capture state machine

States: `{captureState: bool, captureDone: bool}`.

- `configure(mode, sampleTime, samples, continuous, raw)`: pauses capture,
  rebuilds channels/streams, reallocates buffers, zeroes `capture_i`/`capture_o`,
  clears **all listeners**, broadcasts `deviceConfig`. Devices clamp
  `sampleTime` to hardware limits and store the actualized value.
- `start_capture()`: no-op if running. If `captureDone`, first performs a
  reset (zero counters, reset listeners, broadcast `captureReset`). Then
  device hook, `captureState=true`, broadcast `captureState`.
- `pause_capture()`: no-op if not running; device hook, broadcast.
- Completion: after each input packet, if `!continuous && capture_i >= samples`
  the device sets `captureDone=true`, pauses, and broadcasts
  `captureState {state:false, done:true}`.

### 6.3 Sample ring buffer

Per stream, one `f32` buffer of `captureSamples` elements. Two monotonically
increasing `u32` counters index an *infinite* virtual sample stream:

- `capture_i` — input samples received (next write position `capture_i % N`).
- `capture_o` — output samples generated/consumed by the hardware.

Rules (`tests/test_streaming_device.cpp` asserts all of these):

- `put`: ignored if not configured, or if `capture_i >= N` in non-continuous
  mode (overflow samples are dropped but `capture_i` still counts them).
- `get(i)`: NaN if `i >= capture_i` (not yet collected) or, when the ring has
  wrapped (`capture_i > N`), if `i <= capture_i - N` (overwritten — note the
  boundary sample is also NaN). Otherwise `buf[i % N]`. In the non-continuous
  overshoot window (`N <= i < capture_i`) this wraps and returns stale data
  (Q7).
- `resample(start, count)`: arithmetic mean of `count` consecutive samples; NaN
  if the window extends past `capture_i` or begins at/before the overwritten
  boundary.
- `buffer_min() = max(0, capture_i - N)`, `buffer_max() = capture_i`.
- Counters are 32-bit and wrap after ~11 hours at 100 ksps (known limitation
  Q8 — the Rust port should use `u64`).

### 6.4 Output sources

An output source is a pure function `getValue(sampleIndex, sampleTime) -> f32`
plus metadata `{mode, startSample, effective, hint, source}`. `mode` is the
SMU mode the source drives: 0 disabled/Hi-Z, 1 SVMI (source voltage), 2 SIMV
(source current). `hint` is an arbitrary client string echoed back verbatim
(Pixelpulse smuggles `dutycycle:<n>` through it).

Setting a source (`set` command, `POST /<ch>/output`):
1. `initialize(capture_o, previousSource)` — phase adjustment hook.
2. Replace the channel's source; `startSample = capture_o` (base class) or
   `capture_o + 1` (CEE/M1K overrides — the sample currently being encoded
   can't be affected).
3. Broadcast `outputChanged` with `effective:false`.
4. When the device later observes `capture_i > startSample`, it sets
   `effective=true` and re-broadcasts `outputChanged`. Pixelpulse waits for
   this handshake to resolve set-output callbacks. **Do not omit it.**

Source types and exact math (period/phase in *samples*, from
`streaming_device/output_source.cpp`; all asserted in
`tests/test_output_source.cpp`):

| source | `getValue(s)` |
|---|---|
| `constant` | `value` |
| `sine` | `sin((s+phase)·2π/period)·amplitude + offset` |
| `triangle` | `(abs(fmod(s+phase−period/4, period)/period·2 − 1)·2 − 1)·amplitude + offset` — **note Q1**: C `fmod` is sign-preserving, so the first quarter-period (phase 0) yields out-of-range values `offset + amplitude·[1..2]`. Fix in the port (use Rust `rem_euclid`) unless bug-for-bug compat is desired. |
| `square` | `fmod(s+phase, period) < period/2 ? offset+amplitude : offset−amplitude` |
| `adv_square` | `(s+phase) mod (hi+lo) < lowSamples ? low : high` — **low segment first**; throws if `hi+lo == 0` |
| `arb` | piecewise-linear through points `[(t,v)]`; first point must have `t=0`, times non-decreasing; `repeat` 0→1, −1 = infinite, else N passes; after the last pass the final value holds forever; stateful forward-walking evaluator (must be queried with non-decreasing `s`) |

`relPhase` (default true): on `initialize`, if the previous source is of the
same family (periodic↔periodic or adv_square↔adv_square), the new phase is
adjusted so the *phase fraction* at the switchover sample is continuous:
`phase += fmod(sample + prev.phase, prev.period)/prev.period · period − sample`,
then normalized `phase = fmod(phase, period)`.

`getPhaseZeroAfterSample(s)` — used by out-source triggers; returns the first
(possibly fractional) sample index ≥ s at which the waveform is at phase zero:
- constant: `s`
- periodic: `s + fmod(period − fmod(s+phase, period), period)`
- square: `s + ceil(period − fmod(s+phase, period))` (jumps, so it rounds up;
  at an exact boundary it returns the *next* period)
- adv_square: next low→high transition: `s + (per + lo − (s+phase) mod per) mod per`
- arb: `s + (per − (s−phase) mod per) mod per`, or `s` if period 0

Form-encoded output POST converts time-domain parameters to samples:
`period = 1/sampleTime/frequency` (freq ≤ 0 coerced to 0.001 Hz),
`phase_samples = phase_seconds/sampleTime`, adv_square
`highSamples = time1/sampleTime` (min 1), arb `t = round(t_seconds/sampleTime)`.
Mode aliases: `0|disabled|d`, `1|svmi|v`, `2|simv|i` (case-insensitive;
anything else → 0).

### 6.5 Listeners

A listener streams (optionally decimated, optionally triggered) samples from a
set of streams to one client. Created by WS `listen` or REST CSV `GET`.
Parameters: `id` (per-client; a new `listen` with the same id replaces the
old), `streams` `[{channel,stream}]`, `decimateFactor` (≥1; 0 coerced to 1),
`start` (< 0 ⇒ `buffer_max() + start + 1`, clamped ≥ 0), `count` (> 0 = rows to
deliver then auto-remove; ≤ 0 = unlimited — Pixelpulse sends −1), `binary`
(WS only), `trigger` (optional, below).

After every input packet (`packetDone`), each listener computes:

```
if trigger armed and not yet found: 0 chunks
if index + decimateFactor >= capture_i: 0 chunks       # note: >=, withholds data
nchunks = (capture_i - index) / decimateFactor          # integer division
if count > 0: nchunks = min(nchunks, count - outIndex)
```

Each chunk is `resample(stream, index + k·decimateFactor, decimateFactor)` —
i.e. the mean of `decimateFactor` raw samples; NaN propagates for gaps. After
emitting, `index += nchunks·decimateFactor`, `outIndex += nchunks`.

JSON update message:
`{_action:"update", id, idx: <outIndex before this batch>,`
`sampleIndex: <index> (only when idx==0), subsample: <f64> (only when idx==0`
`and triggered), triggerForced: true (only when idx==0 and force fired),`
`data: [[f32...] per stream], done: true (only on the final batch of a`
`count-limited non-repeating listener)}`.

A listener whose `count` completes (and whose trigger is non-repeating) is
removed after sending `done`. Listeners are also removed when their client
disconnects, when `configure` runs, and (REST) when the HTTP socket closes.
`captureReset` rewinds `index`/`outIndex` to 0 on all listeners.

### 6.6 Triggers

Two types, checked in `findTrigger()` before any data is emitted; once found,
the listener is `triggered` and streams normally from the adjusted index.

**`in` (input level crossing):** scan forward from `index`; trigger at the
first sample where `get(stream, i) > level` and the previous sample was
`<= level` (rising edge through `level`). On trigger: `index = i + offset`,
`subsample = 0`.

**`out` (output phase zero):** `zero = channel.source.getPhaseZeroAfterSample(index)`;
triggers when `capture_o >= round(zero)`; `index = round(zero) + offset`,
`subsample = zero − round(zero)` (fractional part, for sub-sample display
alignment).

Common options: `repeat` (default true) — after `count` rows are emitted the
sweep ends: `outIndex=0`, `triggered=false`, `index += holdoff`, re-arm (and
immediately retry in case more data is buffered). `holdoff` — samples to skip
between sweeps. `offset` — shift of the trigger point; a negative offset
larger than holdoff forces `holdoff = −offset` (infinite-loop guard).
`force` — if no trigger is found within `force` samples of the sweep start,
fire anyway at the current scan position; the first update of a forced sweep
carries `triggerForced:true`.

Errors: unknown trigger `type`, unknown channel/stream, missing `level` for
`in` → error reply.

### 6.7 Binary update frames

When `listen` sets `"binary": true`, updates are sent as WS binary frames
instead of JSON (Pixelpulse's hot path). Layout, all little-endian
(`tests/test_stream_listener.cpp` asserts byte-exactly):

| offset | type | field |
|---|---|---|
| 0 | u8 | frame type, always 1 |
| 1 | u8 | flags: bit0 done, bit1 triggerForced, bit2 subsample-valid |
| 2 | u16 | stream count |
| 4 | u32 | listener id |
| 8 | u32 | idx (outIndex) |
| 12 | u32 | sampleIndex (meaningful when idx == 0) |
| 16 | f32 | subsample (meaningful when flags bit2 set) |
| 20 | u32 | nchunks |
| 24 | f32[] | data, stream-major: nchunks values for stream 0, then stream 1, ... |

NaN values pass through verbatim (that is the point of the binary path —
JSON cannot carry NaN).

---

## 7. Device backends

### 7.1 USB discovery

libusb hotplug (arrival + departure) plus an initial full scan. Matching:

| VID:PID | Device |
|---|---|
| 0x59e3:0xCEE1 | CEE |
| 0x59e3:0xBBBB, 0x59e3:0xb003 | Xmega bootloader |

(VID 0x9999 is accepted as an alternate for 0x59e3 on both the CEE and
bootloader rows.)
| 0x0456:0xCEE2, 0x064B:0x784C | M1K |

Constructor failure (open/serial read) logs and ignores the device. Removal:
erase from `devices`, fire `device_list_changed`, broadcast
`deviceDisconnected` to attached clients, drop listeners.

Common to all USB devices: serial number read from the USB string descriptor
into a fixed 32-byte buffer — **may be NUL-padded**; the C++ passes the padded
string through and Pixelpulse strips NULs client-side (Q10; the Rust port
should trim at the first NUL). WS passthrough commands: `controlTransfer`
(vendor requests, IN `wLength` clamped to ≤ 64, reply
`{_action:"return", id, status, data?:[u8]}`) and `enterBootloader`
(vendor request 0xBB; no reply). Control transfers use a 25 ms timeout.

### 7.2 M1K (ADALM1000)

`model = "com.analogdevices.m1k"`. Channels a/b, streams v
(0–5 V, resolution 5/65536) and i (±200 mA, resolution 0.4/65536 A,
reported in mA). Only capture `mode 0` exists (the `configure` mode of §6.2 —
not to be confused with the per-channel SMU modes below); requesting another
mode logs and uses 0.

**Timing.** 48 MHz timer; a sample takes 2 timer ticks (A/B ADC phases):
`per = round(sampleTime · 48e6)/2`, clamped to [240, 24000];
actual `sampleTime = 2·per/48e6`. Default 100 ksps (per=240);
`minSampleTime = 2·240/48e6 = 10 µs`.

**Vendor control requests** (bmRequestType 0xC0 IN / 0x40 OUT):

| req | dir | meaning |
|---|---|---|
| 0x00 | IN | version string: wIndex 0 = hardware, 1 = firmware (ASCII in 64-byte buffer) |
| 0x01 | IN | read calibration EEPROM (struct below) |
| 0x02 | OUT | write calibration EEPROM (struct as payload) |
| 0x03 | OUT | set LEDs, wValue = 3-bit mask (bit2 red, bit1 green, bit0 blue) |
| 0x05 | OUT | write serial, payload = string ≤ 32 bytes (fw ≥ 2.10) |
| 0x17 | IN | ADM1177 power status, wIndex=3, 3 bytes; overcurrent = status & alert_bit, where alert_bit = 0x8 for fw ≥ "2.11" else 0x4 (string compare) |
| 0x19 | IN | temperature, wValue = channel (0/1), 2 bytes big-endian raw ADC |
| 0x50/0x51 | OUT | GPIO pin clear/set, wValue = pin number |
| 0x53 | OUT | set channel mode, wValue = channel, wIndex = mode (0/1/2 only) |
| 0x59 | OUT | digipot, wValue = channel, wIndex = (r1<<8)\|r2 (wipers 0–127) |
| 0x6F | IN | current USB SOF frame counter, 2 bytes |
| 0xC5 | OUT | start sampling (wValue = per, wIndex = sof_start); wValue 0 = stop |
| 0xCC | OUT | init hardware |
| 0xBB | — | reboot to bootloader |

Calibration EEPROM (packed, matches libsmu):
`{u32 magic = 0x01ee02dd, f32 offset[8], f32 gain_p[8], f32 gain_n[8]}`,
index order: ChA measure-V, measure-I, source-V, source-I, then ChB same.
Invalid magic ⇒ defaults (offset 0, gains 1) but `valid:false` is reported.

**Capture start sequence:** claim interface, set alt setting 1, set channel
modes, stop (0xC5,0), init (0xCC), read SOF and compute
`sof_start = (((sof >> 3) + 0x1f) & 0x7FF) << 3` (aligns start to a future
frame boundary), start (0xC5, per, sof_start), then submit 2 IN and 2 OUT bulk
transfers (EP 0x81 / 0x02), each carrying
`packets_per_transfer = max(1, ceil(0.020 / (sampleTime·256) / 2))` packets
(0.050 s buffer target on Windows). Pause: stop, both channels to Hi-Z, cancel
transfers, release interface. Pausing resets modes; `set_mode` also updates
the cached front-end switch state to mirror what firmware does.

**Data format.** IN packet = 2048 bytes = 256 samples × 4 streams × u16
big-endian. fw ≥ 2.0 interleaves per-sample `[AV,AI,BV,BI]`; older firmware
uses four contiguous blocks. Scaling (non-raw):

```
volts   = raw · 5/65536;              value = (volts − cal.offset) · cal.gain_p
current = ((raw · 0.4/65536) − 0.195) · 1.25    // amps
value_mA = (current − cal.offset) · (current > 0 ? gain_p : gain_n) · 1000
```

Raw mode stores the u16 counts unscaled and reports units "LSB", range
0–65535.

OUT packet = 1024 bytes = 256 samples × 2 channels × u16 big-endian
(interleaved `[A,B]` per sample for fw ≥ 2.0, else block). Encoding
(`encode_out`):

- Hi-Z/disabled: constant 26214 (= 32768·4/5, mid-scale).
- SVMI: `v = clamp((val − cal.offset)·gain_p, 0, 5) · 65536/5`.
- SIMV (val in mA → A): calibrate with gain_p/gain_n by sign, clamp ±0.2 A,
  `v = 65536 · (0.4 + 1.6·amps)`.
- Raw: clamp(val, 0, 65535).

Setting an output whose `mode` differs from the channel's current mode issues
`set_mode` immediately if capturing, otherwise defers to capture start.

**Modes** 0–5: hi_z, svmi, simv, and `_split` variants (3–5) which also raise
the SPLIT pin (PA2=34 / PA7=39, rev F hardware); firmware mode = `mode % 3`.
Digipot presets per mode: SIMV 0x7f7f, SVMI 0x0000, Hi-Z 0x3000. Front-end
GPIO pins (active-low except SPLIT): PB0/PB5 50Ω→2.5V, PB1/PB6 50Ω→GND,
PB2/PB7 feedback, PB3/PB8 output-enable (pin numbers 0x20+n).

**Output/input resync.** `capture_o` and `capture_i` advance in lockstep with
a fixed queue lead; host stalls can shift the lead permanently, dragging
out-source trigger alignment. The device tracks the per-64-packet minimum of
`lead = capture_o − capture_i` after a 16-packet settle; when the minimum
drifts more than ±128 samples (half a chunk) from the baseline, `capture_o`
is snapped back by the drift. Port this (`m1k.cpp handleInTransfer`,
commit 971d02e).

**M1K WS commands / REST endpoints** (frontend switches + digipot, power,
temperature, LEDs, calibration read/write/reset, serial) are enumerated in
WS_API.md §M1K and REST_API.md §M1K; they are thin wrappers over the control
requests above, maintaining a local `frontend[2]` cache of switch state
(hardware state is write-only).

### 7.3 CEE

`model = "com.nonolithlabs.cee"`. 4 MHz timer:
`xmega_per = round(sampleTime·4e6)` clamped to `min_per` (from the version
descriptor, else 100); default 10 ksps. fw ≥ 1.2 exposes a version descriptor
(`0xC0 0x00 wIndex=0xff`: `{u8 major, minor, flags, per_ns, min_per}`;
`per_ns` must be 250) and a git-version string (wIndex=2, reported as
`fwVersion = "<fw>/<git>"`).

**Control requests:** 0x80 configure capture (wValue = per, wIndex = 1 run /
0 off), 0x65 set ADC gain (wValue = gaincode<<2, wIndex = stream: 0=a_i,
1=a_v, 2=b_v, 3=b_i; gaincode = log2(gain), gain ∈ {1,2,4,8,16,32,64}),
0x15 current-limit DACs (wValue/wIndex = per-channel cal words), 0xE0/0xE1
read/write calibration EEPROM, 0x20/0x21 GPIO get/set (returns
`[in, dir, out]` bytes), 0xBB bootloader.

Calibration EEPROM: `{u32 magic=0x90e26cee, i8 offset_a_v, offset_a_i,
offset_b_v, offset_b_i, i16 dac200_a, dac200_b, dac400_a, dac400_b,
u32 current_gain_a, current_gain_b, u8 flags}` (flag bit0 = USB-powered ⇒
default current limit 200 mA, else 2000). Current gains default to
`45·0.07·1e5` when unset (0xFFFFFFFF).

**Data.** Bulk EP 0x81/0x02, 4 transfers. IN packet:
`{u8 mode_a, mode_b, flags, mode_seq; 10 × 6-byte samples}` — each sample
packs four 12-bit signed values (AV, AI, BV, BI). Flag bit0 = packet dropped
by firmware → broadcast `{"_action":"packetDrop"}` (suppressed for the first
packet after start). Scaling: `V = (offset + raw) · 5/2048 / gain`;
`mA = (offset + raw) · 2.5/2048/(current_gain/1e5) · 1000 / gain`; a disabled
channel's current reads 0. OUT packet: `{u8 mode_a, mode_b; 10 × 3-byte
samples}` packing two 12-bit values. Encoding: SVMI
`4095·clamp(val,0,5)/5`; SIMV `4095·(1.25 + (gain/1e5)·clamp(val,±limit)/1000)/2.5`.

Gain changes pause and restart capture around the control transfer and set
the stream's reported gain / display range (`i` range =
`min(2.5/(gain/1e5)/normalGain·1000, currentLimit)`).

CEE-specific WS commands: `readCalibration`/`writeCalibration` (flat named
fields, see WS_API.md), `tempCalibration` (RAM-only offsets, **no reply**).
REST: `/gpio`.

### 7.4 Bootloader

Sends `{"_action":"info", serial, magic, version, devid, page_size,
app_section_end, hw_product, hw_version}` on attach (instead of
`deviceConfig`). WS commands: `erase`, `write` (base64/binary JSON `data`
bulk-written to EP 1 after `REQ_START_WRITE`), `crc_app`, `crc_boot`
(→ `{_action:"return", id, crc}`), `reset` (no reply), plus `controlTransfer`.
Request codes are in `bootloader/bootloader.hpp`.

---

## 8. Client compatibility contract (Pixelpulse)

Verified against `pixelpulse/src/dataserver-ws.ts` / `dataserver-common.ts`.
The Rust port must keep:

1. `serverHello.version` — client strips a leading `V` and *string-compares*
   for feature gating (`hasOutTrigger`/`hasAdvSquare` require ≥ "1.2").
   Report a plain numeric string like `"1.3"`.
2. Binary update frames exactly as §6.7 — the client validates frame type and
   length and drops malformed frames.
3. `count: -1` in `listen` = unlimited; `start: -1` = live from latest.
4. The `effective:false → true` double `outputChanged` broadcast.
5. `devices` as an *object keyed by id*, not an array.
6. Channel/stream ids exactly `a`/`b` and `v`/`i`; M1K stream displayName /
   units as today (client hard-codes lookups).
7. `hint` echoed byte-for-byte.
8. Trigger `holdoff`/`offset`/`force` are sample counts.
9. `_action:"return"` replies carry the request `id`; error replies use
   `{_action:"return", id, error, status:-1}` shape only in the WebUSB shim —
   the server's shape is `{_action:"error", error, id?}`; both are handled.
10. Client-side workarounds that must NOT be "fixed" server-side without
    coordinating: M1K channel-B 5 µs skew compensation, CEE 2-sample trigger
    pipeline offset (client applies these itself).

---

## 9. Known bugs and quirks (decide at port time)

| # | Issue | Recommendation |
|---|---|---|
| Q1 | Triangle wave first quarter-period out of range (sign-preserving `fmod`) | **Fix** (use `rem_euclid`); covered by a `may_fail` doctest today |
| Q2 | `getDeviceById("")` throws (`.at()` past the end of an empty string) → 500 | Fix: return None |
| Q3 | `usb_fini` calls `devices.empty()` (no-op) and joins a forever-loop thread | Rewrite shutdown properly |
| Q4 | REST handler errors use HTTP 402 | Keep for compat or switch to 400 (Pixelpulse never reads REST errors) |
| Q5 | REST `GET /input` params unvalidated: `count=-1`, non-numeric `resample` → uncaught exception → 500 | Fix: 4xx |
| Q6 | Unknown WS command gets no reply — client awaiting a `return` hangs (mitigated by client timeouts) | Consider an error reply |
| Q7 | Non-continuous overshoot window returns stale wrapped data from `get()` | Fix: NaN for `i >= N` when non-continuous |
| Q8 | `capture_i`/`capture_o` are u32; wrap after ~11 h @ 100 ksps | Use u64 |
| Q9 | Origin policy is not applied to WebSocket upgrades (only HTTP) | Apply to WS upgrade too |
| Q10 | Serial strings may be NUL-padded (client strips) | Trim at first NUL server-side |
| Q11 | `POST /` and `POST /configuration` silently ignore JSON bodies | Implement or 415 |
| Q12 | `packetDrop` emitted by CEE only; M1K overruns are silent | Consider emitting for M1K |
| Q13 | Arb source evaluator is stateful/forward-only; non-monotonic queries misbehave | Make evaluation pure or document the constraint in the trait |

---

## 10. Rust port guidance

**Suggested crates.** `tokio` (runtime), `axum` or `hyper` + `tokio-tungstenite`
(HTTP + WS; axum's WS extractor is sufficient — but note the chunked-streaming
CSV response and the 403/301/404 plain-text bodies), `serde`/`serde_json`
(protocol types; use `#[serde(rename = "_action")]`-style tags),
`nusb` (pure-Rust async libusb replacement with hotplug support — avoids the
dedicated event thread entirely) or `rusb` if sticking closer to libusb,
`bytes` for frame assembly, `tracing` for the `debug` flag.

**Architecture sketch.**

- One task owns the device registry and a `tokio::sync::broadcast` for
  device-list changes.
- Each device is an actor task owning its `StreamingDevice` state; clients
  interact via an `mpsc` command channel; broadcasts go out over a per-device
  `broadcast` channel carrying pre-serialized messages (mirrors
  `broadcastJSON`). This reproduces the C++ single-threaded ordering
  guarantees without a global lock.
- The C++ raw-pointer graph (`Channel*`, `Stream*`, `OutputSource*` owned by
  channels; listeners holding `Stream*`) should become index-based:
  `ChannelId(usize)` / `StreamId(usize)` into the device's own storage.
  Listeners then hold ids, not references — no lifetimes escape the device.
- `OutputSource` → `enum Source { Constant{..}, Sine{..}, ... }` with
  `fn value(&mut self, sample: u64, sample_time: f64) -> f32` (mut because of
  the arb walker, Q13) and `fn phase_zero_after(&self, sample: u64) -> f64`.
- The OUT-transfer fill path (`fillOutTransfer`) is called from USB completion
  context; with `nusb` this becomes an async loop that keeps N transfers in
  flight — keep the output-source lock scope as tight as the C++ mutex.

**Numeric fidelity.** Waveform math is `f64` intermediate → `f32` storage.
The tests assert values to ~1e-4; match the formulas exactly (including the
`round()` in timer-period computation and the sign conventions in §7
scaling) rather than "improving" them, or calibration data recorded against
the C++ server will read differently.

**Testing strategy.** The doctest suite maps 1:1 onto Rust unit tests —
`tests/support.hpp`'s FakeSession/FakeClientConn/TestDevice become trait-object
fakes; port the assertions verbatim (they encode every quirk above).
`tests/e2e.py` is implementation-agnostic: point it at the Rust binary
unchanged (`e2e.py target/release/nonolith-connect`), keeping the `port=N`
flag. For protocol conformance, run the C++ and Rust servers side by side and
diff REST bodies / WS transcripts; for hardware paths, an M1K and the
Pixelpulse UI remain the final integration test.
