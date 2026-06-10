# Nonolith Connect REST API

All endpoints are served on `http://localhost:9003/rest/`.

## Server

### `GET /rest/v1/`

Server identification.

```json
{"server": "Nonolith Connect", "version": "...", "gitVersion": "..."}
```

[rest_service.cpp:48-55](https://github.com/nonolith/connect/blob/c3589e7d415e/rest_service.cpp#L48-L55)

### `GET /rest/v1/devices`

List connected devices.

```json
{
  "com.nonolithlabs.cee~CEE00001": {
    "id": "com.nonolithlabs.cee~CEE00001",
    "model": "com.nonolithlabs.cee",
    "hwVersion": "B",
    "fwVersion": "1.2",
    "serial": "CEE00001"
  }
}
```

[rest_service.cpp:30-33](https://github.com/nonolith/connect/blob/c3589e7d415e/rest_service.cpp#L30-L33),
[device.cpp:42-52](https://github.com/nonolith/connect/blob/98313d8e56f0/device.cpp#L42-L52)

---

## Common (StreamingDevice) endpoints

These apply to all streaming devices (CEE, M1K). All paths below are
relative to `/rest/v1/devices/<device-id>`.

### `GET /`

Device state including channels, streams, capture state, and configuration.

```json
{
  "id": "...", "model": "...", "hwVersion": "...", "fwVersion": "...", "serial": "...",
  "sampleTime": 0.0001, "minSampleTime": 0.000025,
  "mode": 0, "samples": 120000, "length": 12.0,
  "continuous": true, "raw": false, "currentLimit": 200,
  "captureState": false, "captureDone": false,
  "channels": {
    "a": {
      "id": "a", "displayName": "A",
      "output": {"mode": 0, "source": "constant", "value": 0, ...},
      "streams": {
        "v": {"id": "v", "displayName": "Voltage A", "units": "V", "min": 0, "max": 5, "outputMode": 1, "gain": 1, "uncertainty": 0.00244},
        "i": {"id": "i", "displayName": "Current A", "units": "mA", "min": -200, "max": 200, "outputMode": 2, "gain": 1, "uncertainty": 1}
      }
    },
    "b": { ... }
  }
}
```

[streaming_device/rest_api.cpp:312-355](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/rest_api.cpp#L312-L355),
[streaming_device/streaming_device.cpp:56-85](https://github.com/nonolith/connect/blob/462f78f581fd/streaming_device/streaming_device.cpp#L56-L85)

### `POST /`

Set capture state.

| Parameter | Values | Description |
|-----------|--------|-------------|
| `capture` | `true`/`on`/`1`, `false`/`off`/`0` | Start or pause capture |

Returns device state (same as GET).

[streaming_device/rest_api.cpp:248-268](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/rest_api.cpp#L248-L268)

### `GET /configuration`

Device configuration (sampleTime, mode, samples, etc.) without capture state or channel details.

[streaming_device/rest_api.cpp:279-282](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/rest_api.cpp#L279-L282)

### `POST /configuration`

Change device configuration. Clears captured data.

| Parameter | Type | Description |
|-----------|------|-------------|
| `sampleTime` | float | Seconds per sample (clamped to device limits) |
| `samples` | int | Buffer size in samples |
| `currentLimit` | int | Current limit in mA (CEE only) |

[streaming_device/rest_api.cpp:284-308](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/rest_api.cpp#L284-L308)

### `GET /<channel>`

Channel info (id, displayName, output source, streams).

`<channel>` is `a` or `b`.

[streaming_device/rest_api.cpp:272-275](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/rest_api.cpp#L272-L275)

### `GET /<channel>/output`

Current output source configuration for the channel.

```json
{"mode": 1, "source": "constant", "value": 2.5, "startSample": 0, "effective": true, "hint": ""}
```

[streaming_device/rest_api.cpp:20-24](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/rest_api.cpp#L20-L24)

### `POST /<channel>/output`

Set the output source. Accepts form data or JSON body.

**Form data parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `mode` | string/int | `0`/`disabled`/`d`, `1`/`svmi`/`v`, `2`/`simv`/`i` |
| `value` | float | Output value (constant source) |
| `wave` | string | `constant`, `sine`, `triangle`, `square`, `adv_square`, `arb` |
| `hint` | string | Client hint string (echoed back) |
| `amplitude` | float | Waveform amplitude (periodic sources) |
| `frequency` | float | Frequency in Hz (periodic sources) |
| `phase` | float | Phase offset in seconds |
| `relPhase` | `0`/`1` | Relative phase to previous source |

**Additional for `adv_square`:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `value1` | float | High value |
| `value2` | float | Low value |
| `time1` | float | High time in seconds |
| `time2` | float | Low time in seconds |

**Additional for `arb`:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `points` | string | Comma-separated `time:value` pairs (seconds) |
| `repeat` | int | Repeat count (0 = infinite) |

**JSON body:** pass the source description object directly (same fields as
the GET response: `mode`, `source`, `value`/`offset`/`amplitude`/`period`, etc.).

Returns updated output source state.

[streaming_device/rest_api.cpp:26-126](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/rest_api.cpp#L26-L126)

### `GET /<channel>/input`

Stream CSV data. Returns a `text/plain` response with one row per sample.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `resample` | float | `0.01` | Resample interval in seconds |
| `start` | int | `-1` | Start sample index (negative = relative to latest) |
| `count` | int | `1` | Number of output rows |
| `header` | `0`/`1` | `1` | Include column header row |

The response streams data as it becomes available (long-lived connection
for continuous capture).

[streaming_device/rest_api.cpp:168-216](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/rest_api.cpp#L168-L216)

### `POST /<channel>/input`

Set input stream gain.

| Parameter | Type | Description |
|-----------|------|-------------|
| `gain_v` | float | Voltage stream gain multiplier |
| `gain_i` | float | Current stream gain multiplier |

Returns `{"gain": {"v": 1.0, "i": 1.0}}`.

[streaming_device/rest_api.cpp:218-239](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/rest_api.cpp#L218-L239)

---

## CEE-specific endpoints

### `GET /gpio`

Read GPIO port state.

```json
{"status": 0, "in": 0, "dir": 0, "out": 0}
```

[cee/cee.cpp:220-229](https://github.com/nonolith/connect/blob/0afce0c12086/cee/cee.cpp#L220-L229)

### `POST /gpio`

Set GPIO direction and output.

| Parameter | Type | Description |
|-----------|------|-------------|
| `dir` | int | Direction bitmask (0-255) |
| `out` | int | Output bitmask (0-255) |

Returns updated GPIO state.

[cee/cee.cpp:231-260](https://github.com/nonolith/connect/blob/0afce0c12086/cee/cee.cpp#L231-L260)

---

## M1K-specific endpoints

### `GET /frontend`

Analog front-end switch and digipot state for both channels.

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
  "b": { ... }
}
```

| Field | Description |
|-------|-------------|
| `r50_2v5` | 50 ohm termination to 2.5V reference (PB0/PB5) |
| `r50_gnd` | 50 ohm termination to ground (PB1/PB6) |
| `feedback` | Feedback/sense path enable (PB2/PB7, default true) |
| `output_en` | Output driver enable (PB3/PB8, managed by set_mode) |
| `split` | SPLIT input enable (PA2/PA7, rev F hardware) |
| `pot` | AD5122 digipot wiper positions [r1, r2], each 0-127 |

[m1k/m1k.cpp:488-509](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L488-L509)

### `POST /frontend`

Set front-end switches and/or digipot. All parameters optional; only
specified fields are changed.

| Parameter | Type | Description |
|-----------|------|-------------|
| `channel` | string | `a` or `b` (default: `a`) |
| `r50_2v5` | `true`/`false`/`1`/`0` | 50 ohm to 2.5V switch |
| `r50_gnd` | `true`/`false`/`1`/`0` | 50 ohm to GND switch |
| `feedback` | `true`/`false`/`1`/`0` | Feedback/sense switch |
| `output_en` | `true`/`false`/`1`/`0` | Output enable switch |
| `split` | `true`/`false`/`1`/`0` | SPLIT input enable |
| `pot_r1` | int (0-127) | Digipot wiper 1 position |
| `pot_r2` | int (0-127) | Digipot wiper 2 position |

Returns full frontend state (same as GET).

Note: `output_en`, `feedback`, and `split` are normally managed by
`set_mode()`. Manual overrides persist until the next mode change.

[m1k/m1k.cpp:847-879](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L847-L879)

### `GET /power`

ADM1177 hot-swap controller status.

```json
{
  "overcurrent": false,
  "status_raw": 0,
  "alert_bit": 8
}
```

| Field | Description |
|-------|-------------|
| `overcurrent` | true if alert bit set in ADM1177 status |
| `status_raw` | Raw status byte for diagnostics |
| `alert_bit` | Bit position for overcurrent (0x4 for fw < 2.11, 0x8 for fw >= 2.11) |

[m1k/m1k.cpp:528-545](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L528-L545)

### `GET /temperature`

Raw ADC temperature sensor readings (AD7682 internal).

```json
{"a": 1842, "b": 1837}
```

Values are raw 16-bit ADC counts. Conversion to degrees is
device-specific (see AD7682 datasheet).

[m1k/m1k.cpp:549-563](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L549-L563)

### `GET /leds`

Current LED state.

```json
{"leds": 0, "red": false, "green": false, "blue": false}
```

On rev F hardware the three "LEDs" are three red LEDs (DS1/DS2/DS3)
rather than RGB. The field names map to bits 2, 1, 0 respectively.

[m1k/m1k.cpp:567-574](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L567-L574)

### `POST /leds`

Set LED state. Accepts raw bitmask or individual colors.

| Parameter | Type | Description |
|-----------|------|-------------|
| `leds` | int (0-7) | 3-bit bitmask (bit 2=red, 1=green, 0=blue) |
| `red` | `true`/`false`/`1`/`0` | Red LED / DS3 |
| `green` | `true`/`false`/`1`/`0` | Green LED / DS2 |
| `blue` | `true`/`false`/`1`/`0` | Blue LED / DS1 |

If `leds` is provided, it takes precedence. Otherwise individual color
fields are merged with current state.

Returns updated LED state (same as GET).

[m1k/m1k.cpp:881-904](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L881-L904)

### `GET /calibration`

M1K calibration data from EEPROM. The 8-element arrays are indexed as:

| Index | Signal |
|-------|--------|
| 0 | Channel A, measure voltage |
| 1 | Channel A, measure current |
| 2 | Channel A, source voltage |
| 3 | Channel A, source current |
| 4 | Channel B, measure voltage |
| 5 | Channel B, measure current |
| 6 | Channel B, source voltage |
| 7 | Channel B, source current |

```json
{
  "valid": true,
  "offset": [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
  "gain_p": [1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0],
  "gain_n": [1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0]
}
```

[m1k/m1k.cpp:797-823](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L797-L823)

### `POST /calibration`

Write calibration to EEPROM. Accepts JSON body.

```json
{"offset": [...], "gain_p": [...], "gain_n": [...]}
```

Pass `{"reset": true}` to restore defaults (offset=0, gain=1).

Returns `{"status": <usb_return_code>, "valid": true}`.

[m1k/m1k.cpp:906-928](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L906-L928)

### `GET /serial`

Device serial number.

```json
{"serial": "M1K-XXXX-XXXX"}
```

[m1k/m1k.cpp:826-832](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L826-L832)

### `POST /serial`

Write custom serial number (fw >= 2.10, max 32 chars).

| Parameter | Type | Description |
|-----------|------|-------------|
| `serial` | string | New serial number |

Returns `{"status": <usb_return_code>, "serial": "..."}`.

[m1k/m1k.cpp:930-945](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L930-L945)
