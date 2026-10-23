# MAT transport boundary — 2026-10-23

This PR targets **mic**. It contains network negotiation, authenticated live
transport, bounded depacketization and synchronous cycle/EOS callbacks. It does
not own files, offline recording input, native capture, encoding or hardware.

- Codec 7 / `mat-live-v1` uses HKDF-SHA256, AES-128-GCM with authenticated headers
  and a 64-bit packet counter. Network failure is terminal; it cannot synthesize
  a successful EOS. Normal MAT preference selects live7 or Opus0, never replay.
- `MatProfile.h` contains shared SDP parsing and carrier/clock constants; the
  live profile no longer depends on finite dump/playback profile headers.
- Explicit finite codec4/5 profiles remain protocol features. Both validate
  count/SHA and deliver complete cycles/EOS through `LiSetMatPlaybackCallbacks`.
  Codec4 is diagnostic data, never device playback. Consumers own any storage
  policy, publication or bounded hardware drain; errors terminate the stream.
- `MatDumpFile.c/.h`, `STREAM_CONFIGURATION.matDumpPath` and
  `MatReceiverPushRecordingCycle`/`MatReceiverEndRecording` are removed. The
  Sunshine parent contains the migrated diagnostic writer, independent raw-cycle
  adapter and mock replay tool under `spatial-object-spike/mat-diagnostics`.
- Rebuild consumers: the unpublished configuration ABI changed, and diagnostic
  codec4 now requires registered callbacks. The historical Android PR stays on
  its old shared revision; new acceptance uses mock frontends against this PR.

Static CTest has 14 tests, including production AudioStream codec4/5 callback
delivery, SHA/count rejection and consumer failure. Parent software acceptance
also covers the independent crypto oracle, RTSP negotiation, UDP/FEC, RTP wrap,
native-source contracts and migrated diagnostic failure cases. Generated test
bytes do not establish native capture or hardware/Atmos playback acceptance.

Review fixes: both live and explicit finite input have a two-second inactivity
deadline, stopped only after verified END before the consumer's bounded drain.
The receive thread loads the last accepted timestamp before sampling its clock.
MSVC static RTP tests explicitly enable C11 atomics and now run in Windows CI;
finite digest contexts use the OpenSSL 1.0.2-compatible create/destroy names.
Local MinGW and MSVC static CTest suites pass; this does not claim a runtime test
against an installed OpenSSL 1.0.2 SDK. Parent diagnostics exercise real receive
threads with generated UDP data and missing END/final-fragment failures.

## Historical finite handoff — 2026-10-22

The following records the earlier API and acceptance only; it does not describe
the current file ownership, recording input or live transport boundary above.

This branch targets **mic**, the branch consumed by both parent repositories,
not default master. Baseline: b91c7cd72764320edd6140a16f8614aeb63dc42a.
Keep Sunshine and moonlight-vplus pinned to the same resulting commit; rebuild
all consumers because STREAM_CONFIGURATION and the JNI entry point changed.

Upstream integration on 2026-10-22 includes mic commit
`2aa14ffeb877f9093993d9c9cf3c3440e2d2220d`.
The upstream line-based RTSP parser, whitespace/binary-payload support,
case-insensitive last-value-wins lookup and duplicate-node free are retained.
MAT marks the incoming replacement node before upstream insertion; do not
restore the old caller-side free or set flags only on the overwritten node.
`tests/RtspParserMatTest.c` covers unique, identical/case-folded/triple duplicate
headers plus whitespace and embedded-NUL payload handling. With this regression
the parent's rebuilt static-library CTest suites each contain ten tests.
The parent handoff records fresh end-to-end no-device acceptance separately
from the original historical C proof; no Android hardware claim is added.

## Contract

- Codec 4 / `mat-dump-v1`: bounded, app-owned file publication after count/SHA/EOS
  validation. Never decode or play this codec.
- Codec 5 / `mat-iec61937-v1`: explicit finite replay, fileless `MatPlayback` ->
  full-cycle callbacks. No Opus, AC3 or PCM fallback. Explicit matching RTSP
  capability and unique ANNOUNCE confirmation are required.
- Carrier `iec61937-192000-8-s16le`: 61,440 bytes per 20 ms cycle; SMAT logical
  clock 48,000 Hz / 960 ticks per cycle. RTP/FEC fragment ticks are not media PTS.
- `MatDepacketizer` -> `MatReceiver` shared by the dump and playback consumers.
  Preserve cycle boundaries, epoch/domain/index/PTS; fail rather than conceal
  loss or overflow. `MatPlayback` checks count and incremental SHA at EOS before
  requesting the consumer's bounded drain; no successful EOS after failed drain.
- Register playback callbacks during audio init, clear only after audio threads
  join. The callback owns no borrowed data after return; renderers must copy.
- Duplicate parser flag preserves otherwise-coalesced duplicate header evidence;
  strict case-insensitive uniqueness applies to the new playback confirmation.
  Existing generic lookup retains legacy last-value-wins semantics.

## Evidence / limits

The parent Sunshine branch contains tests under `spatial-object-spike/` and the
cross-repository `docs/ATMOS-MAT-HANDOFF.md`. Recorded acceptance: plain, AES audio
and encrypted RTSP + AES sessions, 178 cycles / 10,936,320 bytes, SHA256
`79cd69e33adcd1bcffe95673564b6fc727a7aaed7370745d43ec269925a492f6`.
Actual JNI and Java player were exercised with **fake final hardware**, not an
Android HAL or soundbar. Original dump regression, 15 negative negotiation
cases, playback lifecycle cases and existing nine CTests passed.

This is not infinite/live capture or authenticated audio transport. FNV and
legacy AES-CBC are not a MAC. No claim of Android MAT support, Atmos recognition,
certification or commercial encoding rights is made. Keep finite manifest bounds
and fail-closed behavior until a separate live-source contract is designed.
