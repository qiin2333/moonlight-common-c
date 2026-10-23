/* Exercise the real audio startup with a sink that stops before hardware/threads. */
#include "../src/AudioStream.c"
#include "MatWire.h"
#include "MatReceiver.h"
#include <stdio.h>
static int failures,expectedCodec;
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"line %d: %s\n",__LINE__,#c);failures++;} } while(0)
static int received,ended,terminated,rejectCycle,rejectEnd;
static int consumeCycle(void* context,const unsigned char* bytes,int n,uint64_t epoch,uint64_t domain,uint64_t index,uint64_t pts) {
    (void)context;CHECK(MatCarrierIsValid(bytes,(size_t)n));CHECK(epoch==1&&domain==1&&index==0&&pts==0);
    ++received;return !rejectCycle;
}
static int consumeEnd(void* context,uint64_t count){(void)context;CHECK(count==1);++ended;return !rejectEnd;}
static void terminate(int error){(void)error;++terminated;}
static void finiteCallbacks(void) {
    unsigned char carrier[MAT_WIRE_CYCLE]={0},packet[MAT_WIRE_PACKET];
    memcpy(carrier,"\x72\xf8\x1f\x4e\x16\x00\x02\x00",8);
    const char* hash="b51894f456731ba3171663abe5305090f366ee778eb56cece1009e90005f4898";
    LI_MAT_PLAYBACK_CALLBACKS callbacks={consumeCycle,consumeEnd,NULL};
    LiSetMatPlaybackCallbacks(&callbacks);ListenerCallbacks.connectionTerminated=terminate;
    for(int codec=AUDIO_CODEC_MAT_DUMP;codec<=AUDIO_CODEC_MAT_IEC61937;codec++)for(int test=0;test<5;test++) {
        received=ended=terminated=0;rejectCycle=test==1;rejectEnd=test==2;
        matPlaybackFailedReported=matPlaybackCompleteReported=false;NegotiatedAudioCodec=codec;
        CHECK(MatProfileSet(&MatDumpProfile,hash,test==4?"2":"1"));
        if(test==3)MatDumpProfile.sha256[0]='0';
        matPlayback=MatPlaybackOpen(&MatDumpProfile,&matCallbacks);CHECK(matPlayback!=NULL);
        for(unsigned fragment=0;fragment<MAT_WIRE_FRAGMENTS;fragment++) {
            mat_wire_make(packet,MAT_WIRE_DATA,1,1,0,fragment,mat_hash(carrier,sizeof(carrier)),carrier);
            consumeAudio((char*)packet,sizeof(packet));
        }
        mat_wire_make(packet,MAT_WIRE_END,1,1,1,0,mat_hash(carrier,sizeof(carrier)),NULL);
        consumeAudio((char*)packet,sizeof(packet));
        CHECK(received==1);CHECK(ended==(test==0||test==2));CHECK(terminated==(test!=0));
        MatPlaybackClose(matPlayback);matPlayback=NULL;
    }
    LiSetMatPlaybackCallbacks(NULL);
    CHECK(MatPlaybackOpen(&MatDumpProfile,&matCallbacks)==NULL);
}
static int stopBeforeHardware(int configuration,const POPUS_MULTISTREAM_CONFIGURATION format,void* context,int flags) {
    (void)configuration;(void)context;(void)flags;
    int mat=AUDIO_CODEC_IS_MAT(expectedCodec);
    CHECK(packetQueue.sizeBound==(mat?512:30));
    CHECK(lastSeq==(mat?3:0));
    CHECK(format->sampleRate==48000);
    CHECK(format->samplesPerFrame==(mat?960:240));
    CHECK(matLive==NULL && matPlayback==NULL);
    return -123;
}
int main(void) {
    CHECK(initializePlatform()==0);
    finiteCallbacks();
    const int requests[]={AUDIO_CODEC_MAT_AUTO,AUDIO_CODEC_MAT_AUTO,AUDIO_CODEC_MAT_IEC61937,AUDIO_CODEC_OPUS,AUDIO_CODEC_MAT_LIVE,AUDIO_CODEC_MAT_DUMP};
    const int selections[]={AUDIO_CODEC_MAT_LIVE,AUDIO_CODEC_OPUS,AUDIO_CODEC_MAT_IEC61937,AUDIO_CODEC_OPUS,AUDIO_CODEC_MAT_LIVE,AUDIO_CODEC_MAT_DUMP};
    for(unsigned i=0;i<sizeof(requests)/sizeof(requests[0]);i++) {
        StreamConfig.audioCodec=requests[i];
        StreamConfig.audioConfiguration=AUDIO_CONFIGURATION_STEREO;
        NegotiatedAudioCodec=selections[i];expectedCodec=selections[i];
        NormalQualityOpusConfig=(OPUS_MULTISTREAM_CONFIGURATION){0};
        NormalQualityOpusConfig.sampleRate=48000;NormalQualityOpusConfig.channelCount=2;NormalQualityOpusConfig.streams=1;
        HighQualitySurroundEnabled=false;AudioPacketDuration=5;AudioCallbacks.init=stopBeforeHardware;
        CHECK(initializeAudioStream()==0);
        CHECK(packetQueue.sizeBound==30 && lastSeq==0);
        CHECK(startAudioStream(NULL,0)==-123);
        CHECK(!pingThreadStarted);
        destroyAudioStream();
    }
    cleanupPlatform();
    if(failures)return 1;
    puts("PASS negotiated audio startup; codec4/5 deliver verified cycles/EOS through callbacks, reject missing/failing consumers and wrong SHA/count; no file path or offline input API");
    return 0;
}
