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

There should only be one device listening on a particular device ID.

All messages should be published with MQTT QoS of at least 1. (2 is preferred,
however AWS IoT does not support MQTT QoS 2.) Subscriptions should also be made
with QoS of at least 1.

## Data types

Data types are specified using Rust type names:

- `i32` represents a 32-bit signed integer
- `u32` represents a 32-bit unsigned integer
- `f32` represents a 32-bit IEEE 754 binary floating point
- `str` represents a UTF-8 null-terminated string
- `bool` represents a boolean, which is a `u8` representing `true` if nonzero,
  and `false` otherwise

## Message format

All messages have the following MQTT payload:

| Name | Type |
| - | - |
| Message number | `u32` |
| Type-specific payload | ... |

### Message numbers

#### Client &rarr; Device

The message number for Client &rarr; Device messages should be a random nonce.
Devices should use it to guard against duplicate deliveries of the same message.

#### Device &rarr; Client

The message number for Device &rarr; Client messages is a strictly increasing
number starting from 0.

- Message numbers should not be skipped, that is, consecutive messages should
  have consecutive message numbers.
    - This is to facilitate reconstructing `display` messages in the correct order.
- When the message number exceeds 4 000 000 000 (2^32 rounded), devices may wrap
  the number around to 0.

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

`display` sends output from running program(s) back to clients.

Clients should buffer display messages until a Flush display message is
received. When a Flush message is received, clients should sort the buffered
display messages, reconstruct the display message, and then display it as a
single display message as appropriate for the client's user interface.

The slightly awkward structure of this message is to reduce the amount of
formatting and buffering that the device has to do, and offload that onto the
client instead. For example, when printing an array, the device may send
displays like this

- `"["`
- `1`
- `", "`
- `2`
- `"]"`

to avoid having to stringify numbers on the device, which may have limited
memory and processing power.

#### Payload

| Name | Type |
| - | - |
| Display message type | `u16` |
| Sub-payload | Depends on display message type |

Payload for all display message types, except Flush:

| Name | Type |
| - | - |
| Message data type | `u16` |
| Message data | Various |

Payload for Flush:

| Name | Type |
| - | - |
| Starting message number | `u32` |
| Message count | `u32` |

#### Display message type

For `display`:

| Type | Value |
| - | - |
| Standard output | 0 |
| Standard error | 1 |
| Program result | 2 |
| Prompt | 3 |
| Flush | 100 |

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
