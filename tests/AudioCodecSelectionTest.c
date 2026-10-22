/* Exercise the real audio startup with a sink that stops before hardware/threads. */
#include "../src/AudioStream.c"
#include <stdio.h>
static int failures,expectedCodec;
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"line %d: %s\n",__LINE__,#c);failures++;} } while(0)
static int stopBeforeHardware(int configuration,const POPUS_MULTISTREAM_CONFIGURATION format,void* context,int flags) {
    (void)configuration;(void)context;(void)flags;
    int mat=AUDIO_CODEC_IS_MAT(expectedCodec);
    CHECK(packetQueue.sizeBound==(mat?512:30));
    CHECK(lastSeq==(mat?3:0));
    CHECK(format->sampleRate==48000);
    CHECK(format->samplesPerFrame==(mat?960:240));
    CHECK(matDump==NULL && matPlayback==NULL);
    return -123;
}
int main(void) {
    CHECK(initializePlatform()==0);
    const int requests[]={AUDIO_CODEC_MAT_AUTO,AUDIO_CODEC_MAT_AUTO,AUDIO_CODEC_MAT_IEC61937,AUDIO_CODEC_OPUS,AUDIO_CODEC_MAT_LIVE};
    const int selections[]={AUDIO_CODEC_MAT_LIVE,AUDIO_CODEC_OPUS,AUDIO_CODEC_MAT_IEC61937,AUDIO_CODEC_OPUS,AUDIO_CODEC_MAT_LIVE};
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
    puts("PASS actual audio startup uses negotiated codec for queue/sequence/frame duration; auto MAT, auto Opus, strict MAT and normal Opus");
    return 0;
}
