#include "Limelight.h"
#include "MatPlaybackProfile.h"
#include <stdio.h>
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"line %d: %s\n",__LINE__,#c);return 1;} } while(0)
static const char valid[] =
    "v=0\r\na=x-ss-audio.mat-playback:1\r\na=x-ss-audio.mat-carrier:" MAT_PLAYBACK_CARRIER
    "\r\na=x-ss-audio.mat-clock:48000\r\na=x-ss-audio.mat-sha256:"
    "79cd69e33adcd1bcffe95673564b6fc727a7aaed7370745d43ec269925a492f6"
    "\r\na=x-ss-audio.mat-cycles:178\r\n";
int main(void) {
    LI_MAT_DUMP_PROFILE p;
    CHECK(MatPlaybackProfileSelect(&p,valid,1)==AUDIO_CODEC_MAT_IEC61937);
    CHECK(p.cycles==178);
    CHECK(MatPlaybackProfileSelect(&p,"v=0\r\n",1)==AUDIO_CODEC_OPUS);
    CHECK(p.cycles==0 && p.sha256[0]==0);
    CHECK(MatPlaybackProfileSelect(&p,valid,0)==AUDIO_CODEC_MAT_IEC61937);
    CHECK(MatPlaybackProfileSelect(&p,"v=0\r\n",0)==-1);
    CHECK(p.cycles==0 && p.sha256[0]==0);
    CHECK(MatPlaybackProfileSelect(&p,NULL,1)==AUDIO_CODEC_OPUS);
    CHECK(MatPlaybackProfileSelect(&p,"a=x-ss-audio.mat-dump:1\r\n",1)==AUDIO_CODEC_OPUS);
    char duplicate[1024];snprintf(duplicate,sizeof(duplicate),"%sa=x-ss-audio.mat-playback:1\r\n",valid);
    CHECK(MatPlaybackProfileSelect(&p,duplicate,1)==AUDIO_CODEC_OPUS);
    CHECK(p.cycles==0 && p.sha256[0]==0);
    CHECK(!AUDIO_CODEC_IS_MAT(AUDIO_CODEC_MAT_AUTO));
    puts("PASS MAT preference selects only confirmed playback; absent/invalid/dump profiles fall back before ANNOUNCE; strict mode rejects");
    return 0;
}
