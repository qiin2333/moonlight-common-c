# Experimental packet feedback and policy notices

These capabilities are explicitly negotiated extensions for compatible hosts.
They preserve the existing fixed-FEC and unnegotiated paths. Enabling measurement
does not enable packet control; receiving a policy notice does not grant a
control lease.

The packet observer records original reception before RS reconstruction, a
bounded receive window and limited late corrections. Versioned reports carry
connection/clock/report identities and canonical packet statuses. The producer
has bounded history and feedback output; it does not equate missing original
packets with frame loss, decoding failure or rendering failure. Read observation
state through `LiGetVideoNetworkSnapshot()` while the original connection is
active. Stop/reinitialize clears that connection's state.

Reliable policy notices require explicit `policyStatusVersion:1` SDP/ANNOUNCE
negotiation and host `X-SS-Policy-Status` confirmation, packet feedback, video
profile 2, and video/control v2 encryption. Missing or future versions leave
the capability disabled. The control type is `0x550f`, payload version 1 and
exact payload length 72. Integers are big-endian:

| Offset | Width | Value |
| --- | --- | --- |
| 0 | 2 | Version 1 |
| 2 | 2 | Length 72 |
| 4 | 2 | Flags |
| 6 | 1 | Source: legacy/manual/GoogCC/local = 0/1/2/3 |
| 7 | 1 | Accepted-revision failure: none/unsupported/backend/superseded/stopped = 0/1/2/3/4 |
| 8 | 4 | Session ID |
| 12 | 4 | Reserved zero |
| 16 | 8 | Connection epoch |
| 24 | 8 | Notice sequence |
| 32 | 8 | Control epoch |
| 40 | 8 | Accepted revision |
| 48 | 8 | Historical SDK-applied revision |
| 56 | 8 | Historical first-sent revision |
| 64 | 8 | First-sent frame identity |

Flags are applied-known `1`, first-sent-known `2`, pending `4`, encoder-ready
`8`, stopped `16`, packet-control `32` and video-pacer `64`. Unknown flags,
nonzero reserved data, inconsistent optional fields, malformed progress or
identity are rejected atomically. Frame zero is valid. An absent optional
revision/frame is zero. Current encoder readiness may fall during a rebuild;
historical SDK/first-send progress cannot regress. Repeated/older notices and
identity changes do not overwrite the pinned connection. A stopped receiver
cannot revive without a new initialization, and sequences never wrap.

`LiGetTransportPolicyStatusNotice()` copies the last authenticated notice
under the observer lock. It returns false before a valid negotiated notice or
after stop. Applications reconcile full policy values, request identities and
permission through paired HTTPS and retain periodic query compensation.
Latest-state notices do not promise every intermediate event or a STOPPED
delivery after disconnect. A first-send receipt proves submission, not delivery.

The ENet dependency is pinned to the optional outbound admission implementation
in [qiin2333/enet PR #1](https://github.com/qiin2333/enet/pull/1). Rebuild the
library and its consumers together because the public ENetHost layout changes.

Configure with `-DBUILD_TESTING=ON` and run `ctest --test-dir build` to execute
wire, observer, lifecycle and negotiation regression targets, including policy
notice golden bytes and rejection/lifetime tests. These tests do not establish
application/device operation, resource cost or QoE performance.

The MbedTLS backend already uses PSA multipart AEAD. Configuration now checks
that the installed crypto library can link that API; headers alone are not
sufficient. CI builds the official MbedTLS 3.6.7 source archive with its pinned
SHA-256 instead of the incompatible Ubuntu 2.28 package. The crypto regression
executes an AES-128-GCM known-answer vector, rejects a modified authentication
tag, and checks context reuse with a new nonce and a non-block-aligned payload.
OpenSSL and MbedTLS run the same test without changing the production cipher.

MSVC shared builds export library symbols. Tests that access internal connection
globals use a private static library built from the same configured sources and
dependencies, while the production DLL is still built. This avoids adding
`dllimport` declarations to internal connection state. Run both Debug and Release
tests; Debug retains assertions and the queue's synthetic RS recovery validation.

Observer locking remains independent of stream lifetime, so snapshot reads can
race destruction without accessing a destroyed platform mutex. After 64 failed
atomic attempts a contender uses the existing platform sleep for a nominal 1 ms
pause and retries. This prevents an unbounded tight spin during report-history
scans; it does not guarantee lock fairness, a 1 ms wake-up, or packet latency.
Scheduler granularity and receive-delay cost still require device measurement.
