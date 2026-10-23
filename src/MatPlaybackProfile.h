#pragma once
#include "MatDumpProfile.h"
#define MAT_PLAYBACK_CODEC 5
#define MAT_PLAYBACK_NAME "mat-iec61937-v1"
#define MAT_PLAYBACK_CARRIER MAT_CARRIER
#define MAT_PLAYBACK_CLOCK MAT_CLOCK
/* Finite replay v1 only. No claim of an infinite/live capture source. */
static inline int MatPlaybackProfileParse(LI_MAT_DUMP_PROFILE* out,const char* sdp) {
    char version[8],carrier[48],clock[16],hash[65],count[16];
    memset(out,0,sizeof(*out));
    return MatSdpValue(sdp,"x-ss-audio.mat-playback",version,sizeof(version))==1 && !strcmp(version,"1") &&
        MatSdpValue(sdp,"x-ss-audio.mat-carrier",carrier,sizeof(carrier))==1 && !strcmp(carrier,MAT_PLAYBACK_CARRIER) &&
        MatSdpValue(sdp,"x-ss-audio.mat-clock",clock,sizeof(clock))==1 && !strcmp(clock,MAT_PLAYBACK_CLOCK) &&
        MatSdpValue(sdp,"x-ss-audio.mat-sha256",hash,sizeof(hash))==1 &&
        MatSdpValue(sdp,"x-ss-audio.mat-cycles",count,sizeof(count))==1 && MatProfileSet(out,hash,count);
}
static inline int MatPlaybackProfileConfirm(const LI_MAT_DUMP_PROFILE* expected,const char* codec,
        const char* hash,const char* count,const char* carrier,const char* clock) {
    LI_MAT_DUMP_PROFILE got;
    return codec && !strcmp(codec,MAT_PLAYBACK_NAME) && carrier && !strcmp(carrier,MAT_PLAYBACK_CARRIER) &&
        clock && !strcmp(clock,MAT_PLAYBACK_CLOCK) && MatProfileSet(&got,hash,count) &&
        got.cycles==expected->cycles && !strcmp(got.sha256,expected->sha256);
}
/* Fallback is legal only during DESCRIBE, before any codec is announced or media is consumed.
 * Strict diagnostic requests retain their failure behavior. Never accept a dump-only profile. */
static inline int MatPlaybackProfileSelect(LI_MAT_DUMP_PROFILE* out,const char* sdp,int allowFallback) {
    if(sdp && MatPlaybackProfileParse(out,sdp))return MAT_PLAYBACK_CODEC;
    memset(out,0,sizeof(*out));
    return allowFallback?0:-1;
}
