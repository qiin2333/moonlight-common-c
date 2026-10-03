#pragma once

#include "VideoNetwork.h"
#include "TransportFeedbackWire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VF_WIRE_OVERHEAD_BOUND 96u
#define VF_FEEDBACK_BURST_BYTES (8u * (TF_MAX_REPORT_BYTES + VF_WIRE_OVERHEAD_BOUND))
#define VF_READY_TIMEOUT_US 1000000u

typedef struct _VIDEO_PACKET_FEEDBACK {
    TF_READY ready;
    uint64_t expectedConnectionEpoch;
    uint64_t lastReadyUs;
    uint64_t nextReportSequence;
    uint64_t lastCommittedReportSequence;
    uint64_t nextCycleUs;
    uint64_t tokenTimeUs;
    uint32_t tokens;
    unsigned reportsInCycle;
    bool active;
    bool exhausted;
} VIDEO_PACKET_FEEDBACK;

// Single owner or externally synchronized. No heap storage, no playback wait.
void VfInitialize(VIDEO_PACKET_FEEDBACK* state, uint64_t expectedEpoch);
bool VfAcceptReady(VIDEO_PACKET_FEEDBACK* state, VIDEO_NETWORK_OBSERVER* observer,
                   const TF_READY* ready, uint64_t nowUs);
bool VfObserveAuthenticated(VIDEO_PACKET_FEEDBACK* state, VIDEO_NETWORK_OBSERVER* observer,
                            uint64_t epoch, uint64_t sequence, uint32_t udpBytes, uint64_t arrivalUs);
// Reserves report sequence and conservative wire tokens. The report remains
// dirty until CommitQueued validates that the observed state hasn't changed.
bool VfPrepareReport(VIDEO_PACKET_FEEDBACK* state, VIDEO_NETWORK_OBSERVER* observer,
                     uint64_t nowUs, TF_PACKET_REPORT* report);
void VfCommitQueued(VIDEO_PACKET_FEEDBACK* state, VIDEO_NETWORK_OBSERVER* observer,
                    const TF_PACKET_REPORT* report);

#ifdef __cplusplus
}
#endif
