# Experimental finite MAT transport handoff — 2026-10-22

This branch targets **mic**, the branch consumed by both parent repositories,
not default master. Baseline: b91c7cd72764320edd6140a16f8614aeb63dc42a.
Keep Sunshine and moonlight-vplus pinned to the same resulting commit; rebuild
all consumers because STREAM_CONFIGURATION and the JNI entry point changed.

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
