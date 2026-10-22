#include "MatReceiver.h"
#include <string.h>
static int fail(MAT_RECEIVER* s) {
    s->failed = 1; s->depacketizer.failed = 1; return -1;
}
int MatCarrierIsValid(const uint8_t* p, size_t n) {
    if (!p || n != 61440 || memcmp(p, "\x72\xf8\x1f\x4e", 4) ||
        (p[4] & 31) != 0x16 || (p[4] & 0x80)) return 0;
    unsigned pd = (unsigned)p[6] | (unsigned)p[7] << 8;
    return pd >= 2 && pd <= 61432 && !(pd & 1);
}
static int consume(void* context, const uint8_t* p, size_t n) {
    MAT_RECEIVER* s = context;
    MAT_DEPACKETIZER* d = &s->depacketizer;
    MAT_CYCLE_INFO info = {d->epoch, d->domain, d->next_frame, d->next_frame * 960};
    return s->consume && s->consume(s->context, &info, p, n);
}
void MatReceiverInit(MAT_RECEIVER* s, MAT_CONSUME_CYCLE callback,
                     MAT_COMPLETE_STREAM complete, void* context) {
    memset(s, 0, sizeof(*s));
    s->consume = callback; s->complete = complete; s->context = context;
    MatDepacketizerInit(&s->depacketizer, consume, s);
}
static int complete(MAT_RECEIVER* s) {
    if (!s->completed) {
        if (s->complete && !s->complete(s->context, s->depacketizer.next_frame)) return fail(s);
        s->completed = 1;
    }
    return 2;
}
int MatReceiverPush(MAT_RECEIVER* s, const uint8_t* p, size_t n) {
    if (s->failed || s->mode == 2) return fail(s);
    s->mode = 1;
    int result = MatDepacketizerPush(&s->depacketizer, p, n);
    if (result < 0) return fail(s);
    return result == 2 ? complete(s) : result;
}
int MatReceiverPushRecordingCycle(MAT_RECEIVER* s, const MAT_CYCLE_INFO* i,
                                 const uint8_t* p, size_t n) {
    MAT_DEPACKETIZER* d = &s->depacketizer;
    if (s->failed || s->completed || s->mode == 1 || !i || !i->epoch || !i->domain ||
        i->index != d->next_frame || i->index >= UINT64_MAX / 960 ||
        i->pts48k != i->index * 960 || !MatCarrierIsValid(p, n)) return fail(s);
    if (d->started && (i->epoch != d->epoch || i->domain != d->domain)) return fail(s);
    s->mode = 2;
    d->started = 1; d->epoch = i->epoch; d->domain = i->domain;
    if (!s->consume || !s->consume(s->context, i, p, n)) return fail(s);
    ++d->next_frame;
    return 1;
}
int MatReceiverEndRecording(MAT_RECEIVER* s, uint64_t expectedCycles) {
    if (s->failed || s->mode != 2 || !expectedCycles ||
        expectedCycles != s->depacketizer.next_frame) return fail(s);
    s->depacketizer.ended = 1;
    return complete(s);
}
int MatReceiverFinish(const MAT_RECEIVER* s) {
    return s && !s->failed && s->completed && MatDepacketizerFinish(&s->depacketizer);
}
void MatReceiverAbort(MAT_RECEIVER* s) { if (s) fail(s); }
