# DualSense authored haptics IR

This library owns the transport contract: capability advertisement, control
message dispatch, bounded decoding, validation, and synchronous callbacks.
The producer SDK owns PCM analysis. Sunshine owns source/controller association,
encoding, and sending. Clients own playback clocks, deduplication, expiry,
device calibration, and actuator allocation. No SDK, device model, effect preset,
analysis threshold, or output-rate policy is required by the protocol library.

## Integration and compatibility

V3 follows the existing v2 source API: initialize
`CONNECTION_LISTENER_CALLBACKS`, set `ds5HapticsIrV2` and/or `ds5HapticsIrV3`,
and pass it to `LiStartConnection()`. Register both to accept v3 on newer hosts
and v2 on older hosts. Registering neither keeps ordinary rumble. Authored raw
PCM and either IR callback are mutually exclusive.

Appending the optional v3 member leaves existing members and v2 wire semantics
unchanged. Callers and the library must be rebuilt together against the updated
header, as with the v2 callback integration. This is source and wire
compatibility, not binary compatibility for an old application replacing only
its shared library: the callback structure is larger, and initialization/copying
use its complete size. A same-header initializer defaults the new member to
NULL; callers should use `LiInitializeConnectionCallbacks()` before assignment.

For Sunshine, non-NULL callbacks advertise `ML_FF_DS5_HAPTICS_IR_V2 = 0x08`
and `ML_FF_DS5_HAPTICS_IR_V3 = 0x80` independently. These are client `ML_FF_*`
capabilities, separate from host `LI_FF_*` features. Version selection belongs
to the host. Old hosts can continue using v2, or ordinary rumble if they support
neither version. Non-Sunshine sessions advertise neither extension.

Both IR callbacks execute synchronously on the control receive thread. The
frame and its members are borrowed values valid only until the callback returns.
Copy before deferring work; do not block this thread. Callback C structures are
not packed wire structures and must not be serialized with `memcpy()`.

## V2

The existing encrypted control message is `0x550B`, version 2, with a fixed
72-byte payload. `Ds5HapticsIrStream.h` and its golden tests retain the existing
field layout and validation. V3 does not reinterpret a v2 field or message.

## V3 wire layout

The encrypted control message is `0x5511`, version 3. All integers and IEEE-754
binary32 floats are little-endian. Payload length is `64 + eventCount * 32`,
with 0-8 events and a maximum of 320 bytes. Profile 1 describes two authored
actuator lanes derived from 48 kHz stereo PCM; no PCM samples are in this message.

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u8 | Version, 3 |
| 1 | u8 | Flags: discontinuity 1, partial 2, stream end 4, silent 8 |
| 2 | u16 | Complete payload length |
| 4 | u16 | Client controller number |
| 6 | u16 | Profile, 1 |
| 8 | u32 | Stream epoch |
| 12 | u32 | IR frame sequence within the epoch |
| 16 | u64 | Source sampling window end, microseconds |
| 24 | u32 | Number of stereo source frames represented; positive except at END |
| 28 | u8 | Event count, 0-8 |
| 29 | u8 | Lane count, 2 |
| 30 | u16 | Reserved, zero |
| 32, 48 | 4 x f32 | Each lane: continuous low, continuous high, raw RMS, raw peak |
| 64 onward | 32-byte records | Identified source-time events |

`sourceFrameCount` is metadata, not a payload size or an allocation request.
As in v2, its positive u32 range is independent of the producer's processing
block size; the SDK currently accepts up to 240 frames per call, but that is
not a wire constraint. Zero is accepted only with END. Clients need not allocate
or reconstruct source samples from this count.

Each event contains `eventId:u32`, `reserved:u32=0`, `sourceTimeUs:u64`, then
four f32 values: `leftStrength`, `rightStrength`, `confidence`, and `sharpness`.
The decoder rejects unsupported versions/profiles, unknown flag bits, nonzero
reserved fields, incorrect sizes/counts, future event times, and nonfinite or
out-of-range normalized fields. No state is retained or filtered by the decoder.

Profile 1 continuous low/high values describe 50-180 Hz / 180-400 Hz bands as
linear full-scale levels in [0, 1]. They may retain release history and exceed
the current raw peak. Raw RMS and peak refer to unfiltered PCM, with
`0 <= rawRms <= rawPeak <= 1`; they are reference features, not an extra output
layer. Event strengths, confidence, and sharpness are in [0, 1]. Confidence is
an evidence score, not a calibrated probability. No field promises an actuator
amplitude, waveform, preset, or physically achievable playback density.

## Time and lifecycle semantics

Source times share a controller's relative sampling clock. They are not video
or audio PTS. An event time is its source onset and must not exceed the frame's
window end; analysis may publish it after a causal confirmation delay. Clients
map this clock to their local monotonic playback clock.

Continuous state is replaced by newer valid frames. Events are identified by
`(controllerNumber, epoch, eventId)` and may recur in several packets as transport
redundancy. An event's published time and features remain immutable. A client
can accept an unseen, still-useful event from a reordered frame without replacing
its newer continuous state. Frame and epoch ordering use unsigned 32-bit serial
arithmetic; zero is a legal value and rollover does not imply a source reset.

Source restarts/discontinuities establish a new epoch and retire old playback
state. END terminates its epoch and clears pending output; an END for an old
epoch must not stop a new one. SILENT describes current tactile-band input and
does not mean a continuous release tail is zero. Source/actuator queues must
remain bounded, with missed-event expiry and source-loss cleanup owned by the
client. This protocol does not prescribe queue size, playout delay, retransmit
history duration, phone effect cadence, or device-specific routing.
