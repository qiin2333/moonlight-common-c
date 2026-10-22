#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define MAT_DUMP_CODEC 4
#define MAT_DUMP_NAME "mat-dump-v1"
typedef struct { char sha256[65]; uint32_t cycles; } LI_MAT_DUMP_PROFILE;
/* Strict bounded SDP attributes: exact name, no duplicates or truncation. */
static inline int MatSdpValue(const char* sdp,const char* name,char* out,size_t cap) {
    int found=0;size_t key=strlen(name);out[0]=0;
    for(const char* p=sdp;p && *p;) {
        const char* end=strchr(p,'\n');if(!end)end=p+strlen(p);
        const char* trimmed=end;if(trimmed>p && trimmed[-1]=='\r')--trimmed;
        if((size_t)(trimmed-p)>=key+3 && !memcmp(p,"a=",2) && !memcmp(p+2,name,key) && p[key+2]==':') {
            size_t n=(size_t)(trimmed-p)-key-3;if(found || n>=cap)return -1;
            memcpy(out,p+key+3,n);out[n]=0;found=1;
        }
        p=*end?end+1:end;
    }return found;
}
static inline int MatProfileSet(LI_MAT_DUMP_PROFILE* out,const char* hash,const char* count) {
    memset(out,0,sizeof(*out));
    if(!hash || !count || strlen(hash)!=64 || !*count)return 0;
    for(unsigned i=0;i<64;i++)if(!((hash[i]>='0'&&hash[i]<='9') || (hash[i]>='a'&&hash[i]<='f')))return 0;
    unsigned n=0;for(const char* p=count;*p;p++){if(*p<'0'||*p>'9'||n>1000)return 0;n=n*10+(unsigned)(*p-'0');}
    if(!n || n>1000)return 0;
    memcpy(out->sha256,hash,65);out->cycles=n;return 1;
}
static inline int MatProfileParse(LI_MAT_DUMP_PROFILE* out,const char* sdp) {
    char version[8],hash[65],count[16];memset(out,0,sizeof(*out));
    return MatSdpValue(sdp,"x-ss-audio.mat-dump",version,sizeof(version))==1 && !strcmp(version,"1") &&
        MatSdpValue(sdp,"x-ss-audio.mat-sha256",hash,sizeof(hash))==1 &&
        MatSdpValue(sdp,"x-ss-audio.mat-cycles",count,sizeof(count))==1 && MatProfileSet(out,hash,count);
}
static inline int MatProfileConfirm(const LI_MAT_DUMP_PROFILE* expected,const char* codec,const char* hash,const char* count) {
    LI_MAT_DUMP_PROFILE got;return codec && !strcmp(codec,MAT_DUMP_NAME) && MatProfileSet(&got,hash,count) &&
        got.cycles==expected->cycles && !strcmp(got.sha256,expected->sha256);
}
