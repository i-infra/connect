# Adding ADALM1000 (M1K) support to Nonolith Connect

## Background

Connect is a WebSocket/REST server that bridges USB hardware to browser-based
tools like pixelpulse. It currently supports only the Nonolith CEE device. The
ADALM1000 (M1K) is a newer, higher-resolution device from Analog Devices with
the same two-channel SMU architecture. The goal is to add M1K support to
connect so that pixelpulse (and any other connect client) can drive M1K
hardware with no changes to the protocol or framework code.

Connect's device model is already factored for this: `CEE_device` subclasses
`StreamingDevice` (which owns the listener/trigger/decimation engine, ring
buffers, waveform generation, and WebSocket/REST API) and `USB_device` (which
owns the libusb handle and control transfer passthrough). A new `M1K_device`
class slots in at the same level.

## Architecture

```
                    Device (device.hpp)
                       |
              StreamingDevice (streaming_device/)
              /                \
    CEE_device (cee/)      M1K_device (m1k/)   <-- new
              \                /
              USB_device (usb_device.hpp)
```

Everything above `M1K_device` in this diagram is untouched. The WebSocket
protocol, listener engine, output source classes, REST API, JSON serialization,
and multi-client support all come from `StreamingDevice` and the server shell.

## Hardware differences: CEE vs M1K

| Parameter              | CEE                          | M1K (fw >= 2.0)              |
|------------------------|------------------------------|------------------------------|
| USB VID:PID            | 0x59e3:0xCEE1                | 0x0456:0xCEE2 / 0x064B:0x784C |
| ADC resolution         | 12-bit                       | 16-bit                       |
| Timer clock            | 4 MHz                        | 48 MHz (÷2 internally)       |
| Min sample period      | `min_per` ticks (~25 µs)     | 240 ticks → 10 µs            |
| Max sample rate        | 40 ksps                      | 100 ksps                     |
| Samples per packet     | 10                           | 256                          |
| IN sample encoding     | 6 bytes packed (4×12-bit nibbles) | 8 bytes interleaved (4×16-bit BE) |
| OUT sample encoding    | 3 bytes packed (2×12-bit)    | 4 bytes interleaved (2×16-bit BE) |
| IN packet structure    | 64-byte: 4-byte header + 10×6B | No header, 256×8B = 2048B   |
| OUT packet structure   | 34-byte: 2-byte header + 10×3B | No header, 256×4B = 1024B   |
| Calibration            | int8 offsets + DAC values + current gain scalar | float offset[8] + gain_p[8] + gain_n[8] |
| Channel modes          | DISABLED(0), SVMI(1), SIMV(2) | HI_Z(0), SVMI(1), SIMV(2), + SPLIT variants (3-5) |
| Voltage range          | 0 – 5 V                      | 0 – 5 V                     |
| Current range          | ±200 mA (configurable limit) | ±200 mA                      |
| Hardware gain control  | Yes (1–64× via CMD_CONFIG_GAIN) | No                         |
| Current limit DAC      | Yes (CMD_ISET_DAC)           | No (overcurrent via ADM1177) |
| Analog front-end       | Fixed                        | Switchable 50Ω / 2.5V per channel |
| Digipot                | None                         | AD5122 dual 128-step per channel |
| Current monitor        | None                         | ADM1177 hot-swap w/ overcurrent alert |
| Temperature sensor     | None                         | AD7682 internal temp per channel |
| LEDs                   | None exposed                 | 3× RGB (hw D) or 3× red (hw F) |

## M1K analog front-end hardware

Each M1K channel has four GPIO-controlled analog switches on PIOB:

| Function       | Channel A | Channel B | Init state | Active |
|----------------|-----------|-----------|------------|--------|
| 50Ω to 2.5V   | PB0       | PB5       | HIGH (off) | LOW    |
| 50Ω to GND    | PB1       | PB6       | HIGH (off) | LOW    |
| Feedback/sense | PB2       | PB7       | LOW (on)   | LOW    |
| Output enable  | PB3       | PB8       | HIGH (off) | LOW    |

These are controlled via GPIO requests 0x50 (set LOW / activate) and
0x51 (set HIGH / deactivate). Pin numbers map as: PIOB pins use
`wValue = pin_number + 0x20` (e.g., PB0 = 0x20, PB5 = 0x25).

The mode switch pins (PB19 for channel A, PB20 for channel B) select
SVMI vs SIMV at the analog level — LOW = SVMI, HIGH = SIMV.

SPLIT modes (rev F hardware) additionally use:
- Channel A: pin 34 (PA2 via `0x51, 34`) set HIGH to enable split input
- Channel B: pin 39 (PA7 via `0x51, 39`) set HIGH to enable split input

## M1K USB control transfer reference

Complete list of vendor requests (bRequest codes) from firmware `main.c`:

| bRequest | Dir  | Purpose                    | wValue             | wIndex              | Data                |
|----------|------|----------------------------|--------------------|---------------------|---------------------|
| 0x00     | IN   | Get version string         | —                  | 0=HW, 1=FW         | char[64]            |
| 0x01     | IN   | Read EEPROM/cal            | —                  | offset              | byte[wLength]       |
| 0x02     | OUT  | Write EEPROM/cal           | —                  | offset              | byte[wLength]       |
| 0x03     | OUT  | Set LEDs                   | 0-7 (3-bit RGB)   | —                   | —                   |
| 0x05     | OUT  | Write serial number        | —                  | offset              | byte[36]            |
| 0x17     | IN   | Read ADM1177 status        | —                  | byte count (1-64)   | byte[count]         |
| 0x19     | IN   | Read temperature           | channel (0/1)      | —                   | uint16              |
| 0x20     | OUT  | Set ADC V1 config          | register value     | —                   | —                   |
| 0x21     | OUT  | Set ADC I1 config          | register value     | —                   | —                   |
| 0x22     | OUT  | Set ADC V2 config          | register value     | —                   | —                   |
| 0x23     | OUT  | Set ADC I2 config          | register value     | —                   | —                   |
| 0x24     | OUT  | Set DAC A config           | config byte        | —                   | —                   |
| 0x25     | OUT  | Set DAC B config           | config byte        | —                   | —                   |
| 0x50     | OUT  | Set GPIO pin LOW           | pin (+ 0x20 = PB)  | —                   | —                   |
| 0x51     | OUT  | Set GPIO pin HIGH          | pin (+ 0x20 = PB)  | —                   | —                   |
| 0x53     | OUT  | Set channel mode           | channel (0/1)      | mode (0/1/2)        | —                   |
| 0x59     | OUT  | Set digipot                | channel (0/1)      | 0xR1R2 (7-bit each) | —                   |
| 0x6F     | IN   | Get USB microframe         | —                  | —                   | uint16              |
| 0x91     | IN   | Read GPIO pin              | pin                | —                   | uint8               |
| 0xBB     | OUT  | Enter SAM-BA bootloader    | —                  | —                   | —                   |
| 0xC5     | OUT  | Configure/start/stop sampling | sample period (0=stop) | start frame    | —                   |
| 0xC6     | OUT  | Fast single-channel mode   | 0=off, 1=A, 2=B   | —                   | —                   |
| 0xCC     | OUT  | Initialize hardware        | —                  | —                   | —                   |

---

## REST API design for M1K features

The M1K exposes hardware controls that have no CEE equivalent. These are
handled as M1K-specific REST endpoints and WebSocket commands within the
`M1K_device` class, routed through its `handleREST()` and `processMessage()`
overrides — the same pattern CEE uses for its GPIO endpoint.

All M1K-specific endpoints live under the device path:
```
/rest/v1/devices/<device-id>/...
```

The general device, channel, output, input, and configuration endpoints are
inherited from `StreamingDevice` unchanged.

### GET /rest/v1/devices/\<id\>/frontend

Returns the current state of all analog front-end switches and digipots
for both channels.

```json
{
  "a": {
    "r50_2v5": false,
    "r50_gnd": false,
    "feedback": true,
    "output_en": false,
    "split": false,
    "pot": [0, 0]
  },
  "b": {
    "r50_2v5": false,
    "r50_gnd": false,
    "feedback": true,
    "output_en": false,
    "split": false,
    "pot": [0, 0]
  }
}
```

Field definitions:
- `r50_2v5`: 50Ω termination to 2.5V reference. When true, the channel
  input presents a 50Ω impedance to 2.5V. Controlled via PB0/PB5.
- `r50_gnd`: 50Ω termination to ground. When true, the channel input
  presents a 50Ω impedance to ground. Controlled via PB1/PB6.
- `feedback`: Feedback/sense path enable. Controlled via PB2/PB7.
  Defaults to true (active LOW, initialized LOW).
- `output_en`: Output driver enable. Controlled via PB3/PB8.
  Set automatically by `set_mode()` for SVMI/SIMV, cleared for HI_Z.
- `split`: SPLIT input pin enable (rev F only). Controlled via PA2/PA7
  (pin 34 for A, 39 for B).
- `pot`: Digipot wiper positions `[r1, r2]`, each 0–127. The AD5122 has
  two independent wipers per channel controlling feedback resistance.

### POST /rest/v1/devices/\<id\>/frontend

Set front-end switches and digipots. Parameters are optional — only
specified fields are changed.

Form data or JSON body:
```
channel=a
r50_2v5=true
r50_gnd=false
pot_r1=64
pot_r2=32
```

Or for both channels at once:
```json
{
  "a": {"r50_2v5": true, "pot": [64, 32]},
  "b": {"r50_gnd": true}
}
```

Returns the full frontend state (same as GET) after applying changes.

Implementation: translates boolean switch states to GPIO set/clear
commands (0x50/0x51 with appropriate pin numbers). Digipot values are
written via `0x59` with wValue=channel, wIndex=`(r1 << 8) | r2`.

Note: `output_en`, `feedback`, and `split` are also settable here for
direct hardware control, but are normally managed automatically by
`set_mode()`. Setting them manually overrides the mode-driven defaults
until the next mode change.

### GET /rest/v1/devices/\<id\>/power

Returns ADM1177 hot-swap controller status and overcurrent state.

```json
{
  "overcurrent": false,
  "status_raw": 0,
  "alert_bit": 8
}
```

Field definitions:
- `overcurrent`: true if the alert bit is set in the most recent ADM1177
  status read, indicating an overcurrent event occurred during the last
  data acquisition.
- `status_raw`: raw status byte from the ADM1177. The full byte is
  exposed for diagnostic use.
- `alert_bit`: which bit position indicates overcurrent (0x4 for fw
  < 2.11, 0x8 for fw >= 2.11). Included so clients can interpret
  `status_raw` without knowing the firmware version.

The overcurrent check is also run automatically at the end of each
capture (in `on_pause_capture()`). If overcurrent is detected, the
`captureState` action message includes an `overcurrent: true` field:
```json
{"_action": "captureState", "state": false, "done": true, "overcurrent": true}
```

### POST /rest/v1/devices/\<id\>/power

Force a fresh ADM1177 status read and return the result. Takes no
parameters. This is useful for polling between captures.

### GET /rest/v1/devices/\<id\>/temperature

Returns raw ADC temperature sensor readings for both channels.

```json
{
  "a": 1842,
  "b": 1837
}
```

Values are raw 16-bit ADC counts from the AD7682 internal temperature
sensor. Conversion to degrees is left to the client (the transfer
function is device-specific and documented in the AD7682 datasheet).

Implementation: two control transfers (`0xC0, 0x19, channel, 0`) with
wLength=2, one per channel.

### GET /rest/v1/devices/\<id\>/leds

Returns current LED state.

```json
{
  "leds": 0,
  "red": false,
  "green": false,
  "blue": false
}
```

### POST /rest/v1/devices/\<id\>/leds

Set LED state. Accepts either the raw bitmask or individual colors.

```
leds=5
```
or:
```
red=true&green=false&blue=true
```

Returns updated LED state (same as GET).

Implementation: `controlTransfer(0x40, 0x03, leds, 0, 0, 0)`. The
server tracks the current state locally since the hardware is write-only.
Note: on rev F hardware, the three "LEDs" are three red LEDs (DS1/DS2/DS3)
rather than RGB. The API uses the same field names regardless — `red`,
`green`, `blue` map to bits 2, 1, 0 respectively, matching the firmware's
bit assignment.

### Channel mode extensions

The existing `set` command's `mode` field is extended for M1K to accept
modes 0–5:

| mode | Name       | Description                                    |
|------|------------|------------------------------------------------|
| 0    | HI_Z       | Floating / high impedance                      |
| 1    | SVMI       | Source voltage, measure current                 |
| 2    | SIMV       | Source current, measure voltage                 |
| 3    | HI_Z_SPLIT | HI_Z with split input pin enabled (rev F)      |
| 4    | SVMI_SPLIT | SVMI with split input pin enabled (rev F)      |
| 5    | SIMV_SPLIT | SIMV with split input pin enabled (rev F)      |

The `deviceConfig` response includes the mode count per channel (6 for
M1K vs 3 for CEE), allowing clients to discover SPLIT support:
```json
{
  "channels": [{
    "id": "a",
    "modes": ["hi_z", "svmi", "simv", "hi_z_split", "svmi_split", "simv_split"],
    ...
  }]
}
```

WebSocket `set` command:
```json
{"_cmd": "set", "channel": "a", "mode": 4, "source": "constant", "value": 2.5}
```

REST:
```
POST /rest/v1/devices/<id>/a/output
mode=4&source=constant&value=2.5
```

Implementation in `set_mode()`:
1. Write potentiometer via `0x59` (pset depends on base mode: 0x0000 for
   SVMI, 0x7f7f for SIMV, 0x3000 for HI_Z)
2. Write mode via `0x53` with `mode % 3` (firmware only knows 0/1/2)
3. For SPLIT variants, set the split pin HIGH via `0x51`
   (pin 34 for channel A, pin 39 for channel B)

### ADC mux configuration

Exposed as part of the `configure` command and configuration endpoint.

WebSocket:
```json
{"_cmd": "configure", "mode": 0, "adcMux": 1, "samples": 10000, "sampleTime": 0.00001}
```

REST:
```
POST /rest/v1/devices/<id>/configuration
sampleTime=0.00001&samples=10000&adcMux=1
```

`adcMux` values:

| Value | Description                | Streams exposed                        |
|-------|----------------------------|----------------------------------------|
| 0     | Default 4-signal           | a/v, a/i, b/v, b/i                    |
| 1     | 2× voltage                 | a/v_odd, b/v_odd, b/v_even, a/v_even  |
| 2     | 2× current                 | b/i_odd, a/i_odd, a/i_even, b/i_even  |
| 4     | Channel A V+I              | a/v_odd, a/i_odd, a/i_even, a/v_even  |
| 5     | Channel B V+I              | b/i_odd, b/v_odd, b/v_even, b/i_even  |
| 7     | Raw uncalibrated           | raw/0, raw/1, raw/2, raw/3            |

When `adcMux != 0`, `configure()` sends the appropriate ADC register
values via control transfers 0x20–0x23 (register sequences match
`M1000_Device::set_adc_mux()` in libsmu) and rebuilds the channel/stream
structure. The `deviceConfig` notification reflects the new stream layout
so clients can adapt automatically.

For modes 1/2/4/5, the four sample slots carry interleaved odd/even
samples of two signals at 2× the base sample rate. These are presented
as four streams (one per slot) — the client can interleave them to
reconstruct the doubled-rate signal.

### Fast single-channel mode

Exposed alongside ADC mux as an additional option in `configure`:

```json
{"_cmd": "configure",  "fastChannel": "a", ...}
```

REST:
```
POST /rest/v1/devices/<id>/configuration
fastChannel=a
```

Values: `""` (off, default), `"a"` (both ADCs sample channel A),
`"b"` (both ADCs sample channel B). Sends `0xC6` with wValue 0/1/2.
This doubles throughput for a single channel by using both ADC chips.

The `deviceConfig` response reflects the change — the inactive channel's
streams are removed.

### Calibration

M1K uses a different calibration format than CEE. The endpoint interface
is the same but the payload differs.

**GET /rest/v1/devices/\<id\>/calibration**

```json
{
  "valid": true,
  "offset": [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
  "gain_p": [1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0],
  "gain_n": [1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0]
}
```

The 8 entries are indexed as:
- 0: Channel A, measure voltage
- 1: Channel A, measure current
- 2: Channel A, source voltage
- 3: Channel A, source current
- 4: Channel B, measure voltage
- 5: Channel B, measure current
- 6: Channel B, source voltage
- 7: Channel B, source current

**POST /rest/v1/devices/\<id\>/calibration**

Write calibration to EEPROM. Accepts JSON body with the same structure.
Passing `{"reset": true}` restores defaults (offset=0, gain=1).

WebSocket equivalents:
```json
{"_cmd": "readCalibration", "id": 1}
{"_cmd": "writeCalibration", "id": 2, "offset": [...], "gain_p": [...], "gain_n": [...]}
```

### Serial number

**GET /rest/v1/devices/\<id\>/serial**

```json
{
  "serial": "M1K-XXXX-XXXX",
  "custom": false
}
```

**POST /rest/v1/devices/\<id\>/serial**

```
serial=MY_CUSTOM_001
```

Empty string resets to factory default. Requires fw >= 2.10.

### Device info extensions

The `deviceConfig` response for M1K includes additional fields beyond
what CEE provides:

```json
{
  "_action": "deviceConfig",
  "device": {
    "id": "com.analogdevices.m1k~M1K-XXXX",
    "model": "com.analogdevices.m1k",
    "hwVersion": "F",
    "fwVersion": "2.17",
    "serial": "M1K-XXXX-XXXX",
    "sampleTime": 0.00001,
    "minSampleTime": 0.00001,
    "maxSampleTime": 0.001,
    "adcMux": 0,
    "fastChannel": "",
    "capabilities": [
      "frontend",
      "power",
      "temperature",
      "leds",
      "split",
      "adcMux",
      "fastChannel"
    ],
    "channels": [...]
  }
}
```

The `capabilities` array lets clients discover M1K-specific features
without hardcoding model checks. A CEE device would omit this field
or return an empty array.

---

## Files to create

### `m1k/m1k.hpp`

Class declaration for `M1K_device`, inheriting `StreamingDevice` and
`USB_device`. Modeled on `cee/cee.hpp`.

```cpp
#define M1K_EEPROM_VALID 0x01ee02dd

struct M1K_EEPROM_cal {
    uint32_t eeprom_valid;
    float offset[8];
    float gain_p[8];
    float gain_n[8];
};

#define M1K_CHUNK_SIZE      256
#define M1K_IN_PACKET_SIZE  (M1K_CHUNK_SIZE * 4 * 2)   // 2048 bytes
#define M1K_OUT_PACKET_SIZE (M1K_CHUNK_SIZE * 2 * 2)    // 1024 bytes
#define M1K_TIMER_CLOCK     48e6
#define M1K_MIN_PER         240     // 100 ksps
#define M1K_MAX_PER         24000   // ~1 ksps

#define M1K_N_TRANSFERS     4

// GPIO pin definitions (PIOB offset = 0x20)
#define M1K_CHA_50R_2V5     (0x20 + 0)   // PB0
#define M1K_CHA_50R_GND     (0x20 + 1)   // PB1
#define M1K_CHA_FEEDBACK    (0x20 + 2)   // PB2
#define M1K_CHA_OUTPUT_EN   (0x20 + 3)   // PB3
#define M1K_CHB_50R_2V5     (0x20 + 5)   // PB5
#define M1K_CHB_50R_GND     (0x20 + 6)   // PB6
#define M1K_CHB_FEEDBACK    (0x20 + 7)   // PB7
#define M1K_CHB_OUTPUT_EN   (0x20 + 8)   // PB8
#define M1K_CHA_SWMODE      (0x20 + 19)  // PB19
#define M1K_CHB_SWMODE      (0x20 + 20)  // PB20
#define M1K_CHA_SPLIT       34            // PA2
#define M1K_CHB_SPLIT       39            // PA7

enum M1K_mode {
    M1K_HI_Z = 0,
    M1K_SVMI = 1,
    M1K_SIMV = 2,
    M1K_HI_Z_SPLIT = 3,
    M1K_SVMI_SPLIT = 4,
    M1K_SIMV_SPLIT = 5,
};
```

`M1K_device` class members:
- `Channel channel_a, channel_b` + `Stream` objects for V and I per channel
- `M1K_EEPROM_cal cal`
- `unsigned mode[2]` — tracks current mode including SPLIT
- `int m1k_per` — timer period in clock ticks
- `uint16_t sof_start` — SOF sync value
- `unsigned adcMux` — current ADC mux mode
- `unsigned fastChannel` — fast single-channel mode (0=off, 1=A, 2=B)
- `unsigned ledState` — tracked locally (hardware is write-only)
- Per-channel frontend state struct for tracking switch positions
- `libusb_transfer* in_transfers[M1K_N_TRANSFERS]`
- `libusb_transfer* out_transfers[M1K_N_TRANSFERS]`
- `boost::mutex outputMutex, transfersMutex`

Override methods:
- `model()`, `hwVersion()`, `fwVersion()`, `serialno()`
- `configure()`, `on_start_capture()`, `on_pause_capture()`, `on_reset_capture()`
- `handleInTransfer()`, `fillOutTransfer()`
- `setOutput()`
- `processMessage()` — M1K-specific WS commands + fallthrough
- `handleREST()` — M1K-specific REST endpoints + fallthrough

Private methods:
- `encode_out()` — float to 16-bit DAC encoding
- `set_mode()` — potentiometer + mode + GPIO for SPLIT
- `readCalibration()`
- `sync()` — SOF synchronization
- `readADM1177()` — overcurrent status check
- `setGPIO()` — helper for 0x50/0x51 pin control
- `setAdcMux()` — send ADC config register sequences

### `m1k/m1k.cpp`

Implementation. Core sampling methods map directly from `device_m1000.cpp`
into connect's calling convention:

**Constructor** — open device, read hw/fw/serial via control transfers
(`0xC0, 0x00` with wIndex 0 and 1), read calibration, call `configure()`.

**`readCalibration()`** — `controlTransfer(0xC0, 0x01, ...)` into
`M1K_EEPROM_cal`. If magic doesn't match, fill defaults.

**`configure()`** — compute `m1k_per` from sampleTime:
```cpp
m1k_per = round(sampleTime * M1K_TIMER_CLOCK) / 2;
clamp(m1k_per, M1K_MIN_PER, M1K_MAX_PER);
sampleTime = m1k_per / M1K_TIMER_CLOCK;
```
Apply ADC mux configuration if `adcMux != 0`.
Rebuild channel/stream structure based on mux mode.
Allocate stream data buffers. Notify clients.

**`on_start_capture()`** — claim interface, set alt setting 1, configure
channel modes via `set_mode()`, stop sampling (`0xC5, 0, 0`), init
hardware (`0xCC`), sync if multi-device, start sampling
(`0xC5, m1k_per, sof_start`), allocate and submit transfers.

**`on_pause_capture()`** — read ADM1177 overcurrent status, reset modes
to HI_Z, stop sampling (`0xC5, 0, 0`), cancel transfers, release
interface. Broadcasts overcurrent status if detected.

**`handleInTransfer()`** — decode loop for interleaved 16-bit format.
256 samples per packet, 8 bytes per sample. Decoding varies by `adcMux`
mode. Default mode (0):
```
bytes [0:1] → ChA voltage:  raw * (5.0/65536), apply offset[0] + gain_p[0]
bytes [2:3] → ChA current:  (raw * (0.4/65536) - 0.195) * 1.25, apply offset[1] + gain_p/n[1]
bytes [4:5] → ChB voltage:  raw * (5.0/65536), apply offset[4] + gain_p[4]
bytes [6:7] → ChB current:  (raw * (0.4/65536) - 0.195) * 1.25, apply offset[5] + gain_p/n[5]
```
Calls `put()` per stream, `sampleDone()` per sample, `packetDone()` at end.

**`fillOutTransfer()`** — iterate samples, call
`channel.source->getValue(capture_o, sampleTime)`, encode via
`encode_out()`, pack as 16-bit BE interleaved.

**`encode_out()`** — per libsmu's encoding:
- SVMI/SVMI_SPLIT: `(val - cal.offset[ch*4+2]) * cal.gain_p[ch*4+2]`,
  constrain 0–5V, multiply by `65536/5`
- SIMV/SIMV_SPLIT: apply cal with sign-dependent gain,
  constrain ±0.2A, `65536 * (2/5 + 0.8*0.2*20*0.5*val)`
- HI_Z: `32768 * 4 / 5` (midscale)

**`set_mode()`** — translate mode enum to hardware:
1. Potentiometer: `0x59, channel, pset` (0x0000/0x7f7f/0x3000)
2. Mode: `0x53, channel, mode % 3`
3. SPLIT GPIO: `0x51, pin` for SPLIT variants

**`processMessage()`** — handles WS commands:
- `readCalibration`, `writeCalibration` — M1K cal format
- `setFrontend` — switch/digipot control
- `setLED` — LED state
- `setSerial` — custom serial number
- Fallthrough to `USB_device` (controlTransfer, enterBootloader)
  then `StreamingDevice` (listen, configure, set, etc.)

**`handleREST()`** — routes M1K-specific paths:
- `frontend` → GET/POST switch and digipot state
- `power` → GET/POST ADM1177 status
- `temperature` → GET ADC temp readings
- `leds` → GET/POST LED state
- `calibration` → GET/POST EEPROM cal data
- `serial` → GET/POST custom serial
- Fallthrough to `StreamingDevice::handleREST()` for everything else

## Files to modify

### `usb.cpp` — device detection

Add M1K VID/PIDs to `deviceAdded()`:

```cpp
#include "m1k/m1k.hpp"

// in deviceAdded(), alongside the existing CEE check:
if ((desc.idVendor == 0x0456 && desc.idProduct == 0xCEE2) ||
    (desc.idVendor == 0x064B && desc.idProduct == 0x784C)) {
    newDevice = device_ptr(new M1K_device(dev, desc));
}
```

### `SConstruct` — build

Add `m1k/m1k.cpp` to the sources list:

```python
sources = Glob('*.cpp') + Glob('streaming_device/*.cpp') \
        + ['cee/cee.cpp', 'm1k/m1k.cpp', 'bootloader/bootloader.cpp']
```

## What requires zero changes

- `streaming_device/streaming_device.hpp` and `.cpp`
- `streaming_device/stream_listener.hpp` and `.cpp`
- `streaming_device/ws_api.cpp`
- `streaming_device/rest_api.cpp`
- `streaming_device/output_source.cpp`
- `server.cpp`, `websocket_handler.hpp`, `websocket_service.cpp`
- `rest_service.cpp`
- `device.hpp`, `device.cpp`
- `event.hpp`, `event.cpp`
- `json_helpers.hpp`, `url.hpp`, `url.cpp`
- `usb_device.hpp`

## Execution order

1. Create `m1k/m1k.hpp` — class declaration, constants, pin definitions.
2. Create `m1k/m1k.cpp` — constructor, `readCalibration()`, `configure()`.
3. Implement `handleInTransfer()` (default mux mode 0 only).
4. Implement `fillOutTransfer()`, `encode_out()`, `set_mode()`.
5. Implement `on_start_capture()`, `on_pause_capture()`, `on_reset_capture()`.
6. Wire up VID/PID detection in `usb.cpp`, add to `SConstruct`.
7. Build and verify basic streaming works with pixelpulse.
8. Add M1K-specific REST endpoints: `frontend`, `power`, `leds`.
9. Add `temperature`, `calibration`, `serial` endpoints.
10. Add `processMessage()` WebSocket command handlers.
11. Implement ADC mux modes in `configure()` and `handleInTransfer()`.
12. Implement fast single-channel mode.
13. Test SPLIT modes with rev F hardware.

## Pixelpulse compatibility

Pixelpulse's core UI operates on the abstract `deviceConfig` JSON. As long
as `M1K_device` sends valid channel/stream structure with appropriate ranges,
the oscilloscope, waveform controls, export, curve tracer, and bode plot all
work unmodified.

The M1K-specific endpoints (frontend, power, temperature, LEDs) are not
required for basic operation — they're available for extended UI or
direct REST/WebSocket scripting.

Steps 1–7 produce a fully functional M1K device in pixelpulse. Steps 8–13
add the M1K-unique hardware controls.
