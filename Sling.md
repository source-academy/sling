# Sling protocol

A "client" is a host running some sort of programming environment e.g. the
  Source Academy frontend, which is a Source programming environment.

A "device" is a host running an SVML interpreter (e.g. Sinter). A device shall
have a "device ID".

Clients and devices communicate using messages over MQTT, facilitated by an MQTT
broker.

Messages have a type. Messages of a given type are published on the MQTT topic
`<device ID>/<message type>`, which makes it easy to restrict clients to only
devices they are authorised to access, and devices to only send and receive
messages on their specific topics.

## Data types

Data types are specified using Rust type names:

- `i32` represents a 32-bit signed integer
- `u32` represents a 32-bit unsigned integer
- `f32` represents a 32-bit IEEE 754 binary floating point
- `str` represents a UTF-8 null-terminated string
- `bool` represents a boolean, which is a `u8` representing `true` if nonzero,
  and `false` otherwise

## Message types

### `run` (Client &rarr; Device)

Payload: compiled SVML program

Causes the device to run the given program, if it is not already running another
program.

Upon receipt of the message, the device should publish a `status` message to update
all connected clients.

### `stop` (Client &rarr; Device)

Payload: none

Causes the device to stop running any currently running program.

Upon receipt of the message, the device should publish a `status` message to update
all connected clients.

### `status` (Device &rarr; Client)

Payload: running status, as a `bool`

### `display` (Device &rarr; Client), `input` (Client &rarr; Device)

#### Payload

| Name | Type |
| - | - |
| Message type | `u16` |
| Message data type | `u16` |
| Message data | Various |

#### Message types

For `display`:

| Type | Value |
| - | - |
| Standard output | 0 |
| Standard error | 1 |
| Program result | 2 |
| Prompt | 3 |

For `input`:

| Type | Value |
| - | - |
| Prompt result | 4 |

#### Message data

These correspond to `sinter_type_t` in Sinter.

| Type | Type value | Payload |
| - | - | - |
| Undefined | 1 | None |
| Null | 2 | None |
| Boolean | 3 | `bool` |
| Integer | 4 | `i32` |
| Float | 5 | `f32` |
| String | 6 | `u32` string length excluding null terminator, followed by `str` string data |
| Array | 7 | Device implementation-defined stringification of array. Payload is same as string. |
| Function | 8 | None |
