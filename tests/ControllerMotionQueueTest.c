// Exercise the production sender, queue and stop handling without a network peer.
#undef NDEBUG
#include "../src/InputStream.c"
#include <assert.h>
#include <math.h>

static PPACKET_HOLDER popMotion(bool expectedCurrent) {
    PPACKET_HOLDER holder = NULL;
    assert(LbqPollQueueElement(&packetQueue, (void**)&holder) == LBQ_SUCCESS);
    assert(holder->packet.header.magic == LE32(SS_CONTROLLER_MOTION_MAGIC));
    assert(motionPacketIsCurrent(holder, false) == expectedCurrent);
    return holder;
}

static float value(netfloat data) {
    uint32_t bits;
    float result;
    memcpy(&bits, data, sizeof(bits));
    bits = LE32(bits);
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static void enable(uint8_t slot) {
    assert(LiSetControllerMotionEventState(slot, LI_MOTION_TYPE_GYRO, 100) == 0);
    assert(LiSetControllerMotionEventState(slot, LI_MOTION_TYPE_ACCEL, 100) == 0);
}

static void emptyMotion(void) {
    while (LbqGetItemCount(&packetQueue)) freePacketHolder(popMotion(false));
    assert(queuedMotionPackets == 0);
}

int main(void) {
    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 500;
    AppVersionQuad[3] = -1;
    SunshineFeatureFlags = LI_FF_CONTROLLER_TOUCH_EVENTS;
    assert(initializeInputStream() == 0);
    // Host demand can arrive while the control stream starts, before the input thread.
    enable(0);
    initialized = true;

    assert(LiSendControllerMotionEvent(0, 2, 1, 2, 3) == 0);
    assert(LiSendControllerMotionEvent(0, 1, 10, 20, 30) == 0);
    assert(LiSendControllerMotionEvent(0, 2, 4, 5, 6) == 0);
    const float expected[] = {1, 10, 4};
    for (int i = 0; i < 3; i++) {
        PPACKET_HOLDER holder = popMotion(true);
        assert(value(holder->packet.controllerMotion.x) == expected[i]);
        freePacketHolder(holder);
    }

    // Paired reports and zeros are unchanged even around high frequency zero crossings.
    double integral = 0;
    for (int batch = 0; batch < 125; batch++) {
        for (int sample = 0; sample < 10; sample++) {
            float x = sample % 5 == 0 ? 0 : sample % 5 < 3 ? 1 : -1;
            assert(LiSendControllerMotionFrame(0, x, x / 2, -x, 0, 9.80665f, 1) == 0);
        }
        for (int sample = 0; sample < 10; sample++) {
            float x = sample % 5 == 0 ? 0 : sample % 5 < 3 ? 1 : -1;
            PPACKET_HOLDER gyro = popMotion(true), accel = popMotion(true);
            assert(gyro->packet.controllerMotion.motionType == 2);
            assert(accel->packet.controllerMotion.motionType == 1);
            assert(value(gyro->packet.controllerMotion.x) == x);
            assert(value(gyro->packet.controllerMotion.y) == x / 2);
            assert(value(gyro->packet.controllerMotion.z) == -x);
            assert(value(accel->packet.controllerMotion.y) == 9.80665f);
            integral += value(gyro->packet.controllerMotion.x) * .004;
            freePacketHolder(gyro); freePacketHolder(accel);
        }
    }
    assert(fabs(integral) < 1e-9 && LbqGetItemCount(&packetQueue) == 0);

    // One free entry in the shared queue cannot accept half a frame. A final
    // zero still uses the fixed stop slot when all shared queue entries are full.
    for (int i = 0; i < MAX_QUEUED_INPUT_PACKETS - 1; i++) assert(LiSendKeyboardEvent(0x41, KEY_ACTION_DOWN, 0) == 0);
    assert(LiSendControllerMotionFrame(0, 3, 0, 0, 1, 2, 3) == LBQ_BOUND_EXCEEDED);
    assert(LbqGetItemCount(&packetQueue) == MAX_QUEUED_INPUT_PACKETS - 1 && queuedMotionPackets == 0);
    assert(LiSendKeyboardEvent(0x41, KEY_ACTION_UP, 0) == 0);
    assert(LiSendControllerMotionFrame(0, 0, 0, 0, 1, 2, 3) == LBQ_BOUND_EXCEEDED);
    PACKET_HOLDER fullQueueStop;
    assert(takePendingMotionStop(&fullQueueStop) && motionPacketIsCurrent(&fullQueueStop, true));
    finishMotionStop(&fullQueueStop);
    PPACKET_HOLDER queued;
    while (LbqPollQueueElement(&packetQueue, (void**)&queued) == LBQ_SUCCESS) freePacketHolder(queued);

    // A paired frame must not partly enter a nearly full motion budget.
    // Failed USB input does not claim the phone source, and buttons retain capacity.
    assert(LiSendControllerMotionEvent(0, 2, 1, 0, 0) == 0);
    for (int i = 1; i < MAX_QUEUED_MOTION_PACKETS; i++) {
        assert(LiSendControllerMotionEvent(0, 1, i, 0, 0) == 0);
    }
    assert(LiSendControllerMotionFrame(0, 9, 0, 0, 1, 2, 3) != 0);
    assert(LiStopControllerMotionEvent(0, true) == 0);
    assert(pendingGyroStops == 0);
    assert(LbqGetItemCount(&packetQueue) == MAX_QUEUED_MOTION_PACKETS);
    assert(LiSendKeyboardEvent(0x41, KEY_ACTION_DOWN, 0) == 0);
    for (int i = 0; i < MAX_QUEUED_MOTION_PACKETS; i++) freePacketHolder(popMotion(true));
    PPACKET_HOLDER keyboard = NULL;
    assert(LbqPollQueueElement(&packetQueue, (void**)&keyboard) == LBQ_SUCCESS);
    assert(keyboard->packet.header.magic == LE32(KEY_DOWN_EVENT_MAGIC));
    freePacketHolder(keyboard);

    // Stops need no queue capacity and invalidate preceding movement. A dequeued
    // stop cannot survive a source replacement or a newly accepted gyro sample.
    assert(LiSendControllerMotionFrame(0, 7, 0, 0, 1, 2, 3) == 0);
    assert(LiStopControllerMotionEvent(0, true) == 0);
    PACKET_HOLDER oldStop, replacementStop;
    assert(takePendingMotionStop(&oldStop));
    assert(LiResetControllerMotionEvents(0, false) == 0);
    assert(!motionPacketIsCurrent(&oldStop, true));
    assert(takePendingMotionStop(&replacementStop));
    assert(LiSendControllerMotionEvent(0, 2, 8, 0, 0) == 0);
    assert(!motionPacketIsCurrent(&replacementStop, true));
    freePacketHolder(popMotion(false)); freePacketHolder(popMotion(false));
    PPACKET_HOLDER fresh = popMotion(true);
    assert(value(fresh->packet.controllerMotion.x) == 8);
    freePacketHolder(fresh);
    assert(!takePendingMotionStop(&oldStop));

    // Rejected final zero still clears rotation when the queue is saturated.
    for (int i = 0; i < MAX_QUEUED_MOTION_PACKETS; i++) assert(LiSendControllerMotionEvent(0, 2, 9, 0, 0) == 0);
    assert(LiSendControllerMotionEvent(0, 2, 0, 0, 0) != 0);
    assert(LiStopControllerMotionEvent(0, false) == 0); // No extra pending stop.
    assert(takePendingMotionStop(&oldStop));
    assert(motionPacketIsCurrent(&oldStop, true));
    assert(value(oldStop.packet.controllerMotion.x) == 0 && oldStop.enetPacketFlags == ENET_PACKET_FLAG_RELIABLE);
    finishMotionStop(&oldStop);
    assert(!takePendingMotionStop(&oldStop));
    emptyMotion();

    // Demand and generations are per sensor and per slot. A disabled gyro still
    // owes its stop, whereas removing its host slot cancels all remaining work.
    enable(1);
    assert(LiSendControllerMotionEvent(0, 2, 10, 0, 0) == 0);
    assert(LiSetControllerMotionEventState(0, 2, 0) == 0);
    assert(takePendingMotionStop(&oldStop) && motionPacketIsCurrent(&oldStop, true));
    assert(LiSendControllerMotionEvent(0, 2, 99, 0, 0) == 0);
    assert(LiSendControllerMotionEvent(1, 2, 11, 0, 0) == 0);
    assert(LiResetControllerMotionEvents(0, true) == 0);
    assert(!motionPacketIsCurrent(&oldStop, true));
    freePacketHolder(popMotion(false));
    fresh = popMotion(true);
    assert(fresh->packet.controllerMotion.controllerNumber == 1);
    freePacketHolder(fresh);
    assert(LiSetControllerMotionEventState(256, 2, 100) == -3);
    assert(LiSetControllerMotionEventState(0, 3, 100) == -3);
    assert(LiSetControllerMotionEventState(0, 0, 100) == -3);
    assert(LiSendControllerMotionEvent(0, 0, 0, 0, 0) == -3);
    initialized = false;
    assert(LiSendControllerMotionFrame(0, 0, 0, 0, 0, 0, 0) == -2);
    LbqSignalQueueShutdown(&packetQueue);
    LbqSignalQueueShutdown(&packetHolderFreeList);
    destroyInputStream();
    assert(LiSetControllerMotionEventState(0, 2, 100) == -2);
    puts("Shared motion queue, frame, pressure and stop lifecycle checks passed");
    return 0;
}
