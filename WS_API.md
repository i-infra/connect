# Nonolith Connect WebSocket API

Client connects to `ws://localhost:9003/ws/v0`. All messages are JSON.

Client-to-server messages use `_cmd` as the command discriminator.
Server-to-client messages use `_action` as the action discriminator.

Most commands accept an optional `id` (integer) that is echoed back in
the `_action: "return"` response, for request/response correlation.

---

## Connection Lifecycle

### Server Hello (server &rarr; client)

Sent immediately on connection.

```json
{"_action": "serverHello", "server": "Nonolith Connect", "version": "...", "gitVersion": "..."}
```

[websocket_service.cpp:27-31](https://github.com/nonolith/connect/blob/29c3c994e297/websocket_service.cpp#L27-L31)

### Device List (server &rarr; client)

Sent on connection and whenever devices are added/removed.

```json
{
  "_action": "devices",
  "devices": {
    "com.nonolithlabs.cee~CEE00001": {
      "id": "com.nonolithlabs.cee~CEE00001",
      "model": "com.nonolithlabs.cee",
      "hwVersion": "B", "fwVersion": "1.2", "serial": "CEE00001"
    }
  }
}
```

[websocket_service.cpp:44-53](https://github.com/nonolith/connect/blob/29c3c994e297/websocket_service.cpp#L44-L53),
[device.cpp:42-52](https://github.com/nonolith/connect/blob/98313d8e56f0/device.cpp#L42-L52)

### `selectDevice`

Select a device for subsequent commands. Must be called before any
device-specific commands.

```json
{"_cmd": "selectDevice", "id": "com.nonolithlabs.cee~CEE00001"}
```

[websocket_service.cpp:67-76](https://github.com/nonolith/connect/blob/29c3c994e297/websocket_service.cpp#L67-L76)

### Device Config (server &rarr; client)

Sent when a client selects a device, and broadcast on configuration
changes. Contains full device state.

```json
{
  "_action": "deviceConfig",
  "device": {
    "id": "...", "model": "...", "hwVersion": "...", "fwVersion": "...", "serial": "...",
    "sampleTime": 0.0001, "minSampleTime": 0.000025,
    "mode": 0, "samples": 120000, "length": 12.0,
    "continuous": true, "raw": false, "currentLimit": 200,
    "captureState": false, "captureDone": false,
    "channels": { "a": { ... }, "b": { ... } }
  }
}
```

[streaming_device/ws_api.cpp:60-71](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/ws_api.cpp#L60-L71),
[streaming_device/streaming_device.cpp:183-194](https://github.com/nonolith/connect/blob/462f78f581fd/streaming_device/streaming_device.cpp#L183-L194)

### Error (server &rarr; client)

Sent on parse or processing errors.

```json
{"_action": "error", "error": "...", "id": 0}
```

[websocket_service.cpp:89-93](https://github.com/nonolith/connect/blob/29c3c994e297/websocket_service.cpp#L89-L93)

---

## Common (StreamingDevice) Commands

These work for all streaming devices (CEE, M1K).

### `configure`

Set capture parameters. Clears captured data and resets listeners.

| Parameter | Type | Description |
|-----------|------|-------------|
| `mode` | int | Device mode (0=off, 1=2SMU) |
| `sampleTime` | float | Seconds per sample |
| `samples` | int | Buffer size in samples |
| `continuous` | bool | Continuous capture (default false) |
| `raw` | bool | Raw ADC mode (default false) |

```json
{"_cmd": "configure", "mode": 1, "sampleTime": 0.0001, "samples": 10000, "continuous": true}
```

Broadcasts `deviceConfig` to all clients.

[streaming_device/ws_api.cpp:22-27](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/ws_api.cpp#L22-L27)

### `startCapture`

Start data acquisition. Broadcasts `captureState`.

```json
{"_cmd": "startCapture"}
```

[streaming_device/ws_api.cpp:29-30](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/ws_api.cpp#L29-L30)

### `pauseCapture`

Pause data acquisition. Broadcasts `captureState`.

```json
{"_cmd": "pauseCapture"}
```

[streaming_device/ws_api.cpp:32-33](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/ws_api.cpp#L32-L33)

### Capture State (server &rarr; client)

Broadcast when capture starts, pauses, or finishes.

```json
{"_action": "captureState", "state": true, "done": false}
```

[streaming_device/streaming_device.cpp:168-174](https://github.com/nonolith/connect/blob/462f78f581fd/streaming_device/streaming_device.cpp#L168-L174)

### Capture Reset (server &rarr; client)

Broadcast when capture buffer is cleared (e.g. on reconfigure). Resets
all active listeners.

```json
{"_action": "captureReset"}
```

[streaming_device/streaming_device.cpp:176-181](https://github.com/nonolith/connect/blob/462f78f581fd/streaming_device/streaming_device.cpp#L176-L181)

### `set`

Set a channel's output source. Broadcasts `outputChanged`.

| Parameter | Type | Description |
|-----------|------|-------------|
| `channel` | string | `"a"` or `"b"` |
| `mode` | int | `0` (disabled), `1` (SVMI/voltage), `2` (SIMV/current) |
| `source` | string | `"constant"`, `"sine"`, `"triangle"`, `"square"`, `"adv_square"`, `"arb"` |
| `value` | float | Output value (constant source) |
| `hint` | string | Client hint (echoed back) |

**Periodic sources** (`sine`, `triangle`, `square`):

| Parameter | Type | Description |
|-----------|------|-------------|
| `offset` | float | DC offset |
| `amplitude` | float | Peak amplitude |
| `period` | float | Period in samples |
| `phase` | float | Phase offset in samples |
| `relPhase` | bool | Relative phase to previous source |

**Advanced square** (`adv_square`):

| Parameter | Type | Description |
|-----------|------|-------------|
| `high` | float | High value |
| `low` | float | Low value |
| `highSamples` | int | High duration in samples |
| `lowSamples` | int | Low duration in samples |
| `phase` | int | Phase offset in samples |
| `relPhase` | bool | Relative phase |

**Arbitrary waveform** (`arb`):

| Parameter | Type | Description |
|-----------|------|-------------|
| `values` | array | Array of `{"t": <samples>, "v": <value>}` points |
| `repeat` | int | Repeat count (0 = infinite) |

```json
{"_cmd": "set", "channel": "a", "mode": 1, "source": "constant", "value": 2.5}
```

[streaming_device/ws_api.cpp:35-39](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/ws_api.cpp#L35-L39),
[streaming_device/output_source.cpp:147-235](https://github.com/nonolith/connect/blob/bec4bc3cd003/streaming_device/output_source.cpp#L147-L235)

### Output Changed (server &rarr; client)

Broadcast when a channel's output source is changed.

```json
{
  "_action": "outputChanged",
  "channel": "a",
  "mode": 1, "startSample": 0, "effective": true,
  "source": "constant", "value": 2.5, "hint": ""
}
```

[streaming_device/streaming_device.cpp:216-222](https://github.com/nonolith/connect/blob/462f78f581fd/streaming_device/streaming_device.cpp#L216-L222)

### `setGain`

Set input stream gain multiplier. Broadcasts `gainChanged`.

| Parameter | Type | Description |
|-----------|------|-------------|
| `channel` | string | `"a"` or `"b"` |
| `stream` | string | `"v"` or `"i"` |
| `gain` | float | Gain multiplier (default 1) |

```json
{"_cmd": "setGain", "channel": "a", "stream": "v", "gain": 2.0}
```

[streaming_device/ws_api.cpp:41-49](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/ws_api.cpp#L41-L49)

### Gain Changed (server &rarr; client)

```json
{"_action": "gainChanged", "channel": "a", "stream": "v", "gain": 2.0}
```

[streaming_device/streaming_device.cpp:224-231](https://github.com/nonolith/connect/blob/462f78f581fd/streaming_device/streaming_device.cpp#L224-L231)

### `setCurrentLimit`

Set current limit (CEE only).

| Parameter | Type | Description |
|-----------|------|-------------|
| `currentLimit` | int | Current limit in mA |

```json
{"_cmd": "setCurrentLimit", "currentLimit": 200}
```

[streaming_device/ws_api.cpp:50-52](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/ws_api.cpp#L50-L52)

### `listen`

Subscribe to streaming sample data. Replaces any existing listener with
the same `id` from this client.

| Parameter | Type | Description |
|-----------|------|-------------|
| `id` | int | Listener ID (for cancellation and response correlation) |
| `decimateFactor` | int | Downsample factor (default 1) |
| `start` | int | Start sample index; negative = relative to latest (default -1) |
| `count` | int | Number of output rows (0 = unlimited) |
| `streams` | array | Array of `{"channel": "a", "stream": "v"}` stream references |
| `binary` | bool | Send updates as binary WebSocket frames instead of JSON (default false) |
| `trigger` | object | Optional trigger configuration (see below) |

**Trigger object:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `type` | string | `"in"` (input stream rising-edge) or `"out"` (output source phase-zero) |
| `channel` | string | Trigger channel |
| `stream` | string | Trigger stream (for `"in"` type) |
| `level` | float | Trigger level (for `"in"` type) |
| `repeat` | bool | Re-arm after each sweep (default true) |
| `holdoff` | int | Holdoff in samples after trigger (default 0) |
| `offset` | int | Sample offset from trigger point (default 0) |
| `force` | int | Force trigger after this many samples (0 = never) |

```json
{
  "_cmd": "listen", "id": 1, "decimateFactor": 10, "start": -1, "count": 1000,
  "streams": [{"channel": "a", "stream": "v"}, {"channel": "a", "stream": "i"}],
  "trigger": {"type": "in", "channel": "a", "stream": "v", "level": 2.5, "repeat": true}
}
```

[streaming_device/ws_api.cpp:14-16](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/ws_api.cpp#L14-L16),
[streaming_device/stream_listener.cpp:29-92](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/stream_listener.cpp#L29-L92)

### Update (server &rarr; client)

Streaming data response for an active listener.

| Field | Type | Description |
|-------|------|-------------|
| `id` | int | Listener ID |
| `idx` | int | Output row index within this sweep |
| `sampleIndex` | int | Buffer sample index (first packet only) |
| `subsample` | float | Sub-sample trigger error (first packet, if triggered) |
| `triggerForced` | bool | True if trigger was force-fired (first packet only) |
| `data` | array | Array of arrays, one per requested stream |
| `done` | bool | True if this is the final packet (non-repeating) |

```json
{
  "_action": "update", "id": 1, "idx": 0,
  "sampleIndex": 50000, "subsample": 0.23,
  "data": [[2.501, 2.503, ...], [0.015, 0.016, ...]]
}
```

[streaming_device/stream_listener.cpp:113-171](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/stream_listener.cpp#L113-L171)

**Binary updates:** if the `listen` command set `"binary": true`, updates for
that listener arrive as binary WebSocket frames instead of the JSON message
above. Frame layout (all little-endian):

| Offset | Type | Field |
|--------|------|-------|
| 0 | u8 | frame type, always `1` (stream update) |
| 1 | u8 | flags: bit0 `done`, bit1 `triggerForced`, bit2 `subsample` valid |
| 2 | u16 | number of streams |
| 4 | u32 | listener `id` |
| 8 | u32 | `idx` |
| 12 | u32 | `sampleIndex` (valid when `idx` == 0) |
| 16 | f32 | `subsample` (valid when flags bit2 set) |
| 20 | u32 | `nchunks` — samples per stream |
| 24 | f32[] | sample data, one contiguous `nchunks`-long block per stream |

This avoids JSON encode/parse overhead for high-rate raw-sample listeners,
and transports NaN (gap/overrun markers) losslessly.

### `cancelListen`

Cancel an active listener.

| Parameter | Type | Description |
|-----------|------|-------------|
| `id` | int | Listener ID to cancel |

```json
{"_cmd": "cancelListen", "id": 1}
```

[streaming_device/ws_api.cpp:18-19](https://github.com/nonolith/connect/blob/29c3c994e297/streaming_device/ws_api.cpp#L18-L19)

---

## CEE-specific Commands

### `readCalibration` (CEE)

Read CEE calibration from EEPROM.

Response:

```json
{
  "_action": "return", "id": 0,
  "offset_a_v": 0, "offset_a_i": 0,
  "offset_b_v": 0, "offset_b_i": 0,
  "dac200_a": 0, "dac200_b": 0,
  "dac400_a": 0, "dac400_b": 0,
  "current_gain_a": -1, "current_gain_b": -1,
  "flags": 255
}
```

[cee/cee.cpp:186-204](https://github.com/nonolith/connect/blob/0afce0c12086/cee/cee.cpp#L186-L204)

### `writeCalibration` (CEE)

Write CEE calibration to EEPROM.

| Parameter | Type | Description |
|-----------|------|-------------|
| `offset_a_v` | int | Channel A voltage offset |
| `offset_a_i` | int | Channel A current offset |
| `offset_b_v` | int | Channel B voltage offset |
| `offset_b_i` | int | Channel B current offset |
| `dac200_a` | int | DAC 200mA cal, channel A |
| `dac200_b` | int | DAC 200mA cal, channel B |
| `dac400_a` | int | DAC 400mA cal, channel A |
| `dac400_b` | int | DAC 400mA cal, channel B |
| `current_gain_a` | int | Current gain, channel A (default -1) |
| `current_gain_b` | int | Current gain, channel B (default -1) |
| `flags` | int | Calibration flags (default 0xff) |

Response: `{"_action": "return", "id": 0, "status": <usb_return_code>}`

[cee/cee.cpp:161-184](https://github.com/nonolith/connect/blob/0afce0c12086/cee/cee.cpp#L161-L184)

### `tempCalibration` (CEE)

Apply temporary calibration offsets (not written to EEPROM).

| Parameter | Type | Description |
|-----------|------|-------------|
| `offset_a_v` | int | Channel A voltage offset (default 0) |
| `offset_a_i` | int | Channel A current offset (default 0) |
| `offset_b_v` | int | Channel B voltage offset (default 0) |
| `offset_b_i` | int | Channel B current offset (default 0) |

No response sent.

[cee/cee.cpp:206-212](https://github.com/nonolith/connect/blob/0afce0c12086/cee/cee.cpp#L206-L212)

---

## M1K-specific Commands

### `readCalibration` (M1K)

Read M1K calibration from EEPROM. The 8-element arrays are indexed as:

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

Response:

```json
{
  "_action": "return", "id": 0,
  "offset": [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
  "gain_p": [1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0],
  "gain_n": [1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0],
  "valid": true
}
```

[m1k/m1k.cpp:592-616](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L592-L616)

### `writeCalibration` (M1K)

Write M1K calibration to EEPROM.

| Parameter | Type | Description |
|-----------|------|-------------|
| `offset` | array[8] | Float offset values |
| `gain_p` | array[8] | Float positive gain values |
| `gain_n` | array[8] | Float negative gain values |

Response: `{"_action": "return", "id": 0, "status": <usb_return_code>}`

[m1k/m1k.cpp:618-637](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L618-L637)

### `getFrontend`

Read analog front-end switch and digipot state.

Response:

```json
{
  "_action": "return", "id": 0,
  "frontend": {
    "a": {
      "r50_2v5": false, "r50_gnd": false, "feedback": true,
      "output_en": false, "split": false, "pot": [0, 0]
    },
    "b": { ... }
  }
}
```

[m1k/m1k.cpp:639-647](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L639-L647)

### `setFrontend`

Set front-end switches and/or digipot. All switch parameters are
optional; only specified fields are changed.

| Parameter | Type | Description |
|-----------|------|-------------|
| `channel` | string | `"a"` or `"b"` (required) |
| `r50_2v5` | bool | 50 ohm to 2.5V switch |
| `r50_gnd` | bool | 50 ohm to GND switch |
| `feedback` | bool | Feedback/sense switch |
| `output_en` | bool | Output enable switch |
| `split` | bool | SPLIT input enable |
| `pot_r1` | int (0-127) | Digipot wiper 1 |
| `pot_r2` | int (0-127) | Digipot wiper 2 |

Note: `output_en`, `feedback`, and `split` are normally managed by
`set_mode()`. Manual overrides persist until the next mode change.

Response: same as `getFrontend`.

```json
{"_cmd": "setFrontend", "channel": "a", "r50_2v5": true, "pot_r1": 64}
```

[m1k/m1k.cpp:649-681](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L649-L681)

### `getPower`

Read ADM1177 hot-swap controller status.

Response:

```json
{
  "_action": "return", "id": 0,
  "power": {
    "overcurrent": false,
    "status_raw": 0,
    "alert_bit": 8
  }
}
```

[m1k/m1k.cpp:683-691](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L683-L691)

### `getTemperature`

Read raw ADC temperature sensor values (AD7682 internal).

Response:

```json
{
  "_action": "return", "id": 0,
  "temperature": {"a": 1842, "b": 1837}
}
```

Values are raw 16-bit ADC counts.

[m1k/m1k.cpp:693-701](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L693-L701)

### `getLED`

Read current LED state.

Response:

```json
{
  "_action": "return", "id": 0,
  "leds": {"leds": 0, "red": false, "green": false, "blue": false}
}
```

On rev F hardware the three "LEDs" are three red LEDs (DS1/DS2/DS3).
Field names map to bits 2, 1, 0 respectively.

[m1k/m1k.cpp:703-711](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L703-L711)

### `setLED`

Set LED state. Accepts raw bitmask or individual colors.

| Parameter | Type | Description |
|-----------|------|-------------|
| `leds` | int (0-7) | 3-bit bitmask (bit 2=red, 1=green, 0=blue); takes precedence |
| `red` | bool | Red LED / DS3 |
| `green` | bool | Green LED / DS2 |
| `blue` | bool | Blue LED / DS1 |

If `leds` is provided, it takes precedence. Otherwise individual color
fields are merged with current state.

Response: same as `getLED`.

```json
{"_cmd": "setLED", "red": true, "green": false}
```

[m1k/m1k.cpp:713-736](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L713-L736)

### `setSerial`

Write custom serial number (fw >= 2.10, max 32 chars).

| Parameter | Type | Description |
|-----------|------|-------------|
| `serial` | string | New serial number |

Response: `{"_action": "return", "id": 0, "status": <usb_return_code>}`

[m1k/m1k.cpp:738-751](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L738-L751)

---

## USB Passthrough

These commands are available on all USB devices (CEE, M1K, bootloader).

### `controlTransfer`

Raw USB control transfer.

**IN transfer** (device &rarr; host, `bmRequestType` bit 7 set):

| Parameter | Type | Description |
|-----------|------|-------------|
| `bmRequestType` | int | Request type (e.g. `0xC0` for vendor IN) |
| `bRequest` | int | Request code |
| `wValue` | int | Value (default 0) |
| `wIndex` | int | Index (default 0) |
| `wLength` | int | Max bytes to read (default 64, max 64) |

Response:

```json
{"_action": "return", "id": 0, "data": [0, 1, 2, ...]}
```

**OUT transfer** (host &rarr; device, `bmRequestType` bit 7 clear):

| Parameter | Type | Description |
|-----------|------|-------------|
| `bmRequestType` | int | Request type (e.g. `0x40` for vendor OUT) |
| `bRequest` | int | Request code |
| `wValue` | int | Value (default 0) |
| `wIndex` | int | Index (default 0) |
| `data` | array/string | Byte array or string to send |

Response:

```json
{"_action": "return", "id": 0, "status": <usb_return_code>}
```

[usb.cpp:145-201](https://github.com/nonolith/connect/blob/327e4335497d/usb.cpp#L145-L201)

### `enterBootloader`

Reboot device into DFU bootloader mode. No response.

```json
{"_cmd": "enterBootloader"}
```

[usb.cpp:193-196](https://github.com/nonolith/connect/blob/327e4335497d/usb.cpp#L193-L196)

---

## Command Dispatch

Commands are dispatched in this order:

1. `selectDevice` &mdash; handled by WebSocket connection layer
2. Device-specific handler (`CEE_device::processMessage` or `M1K_device::processMessage`)
3. `USB_device::processMessage` &mdash; `controlTransfer`, `enterBootloader`
4. `StreamingDevice::processMessage` &mdash; `listen`, `cancelListen`, `configure`, `startCapture`, `pauseCapture`, `set`, `setGain`, `setCurrentLimit`

If no handler matches, the server logs "Unknown command" to stderr.

[websocket_service.cpp:55-96](https://github.com/nonolith/connect/blob/29c3c994e297/websocket_service.cpp#L55-L96),
[m1k/m1k.cpp:753-755](https://github.com/nonolith/connect/blob/8a22b20ac66f/m1k/m1k.cpp#L753-L755),
[cee/cee.cpp:214-217](https://github.com/nonolith/connect/blob/0afce0c12086/cee/cee.cpp#L214-L217)
