#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define PLUTO_MIN_HZ 70000000LL
#define PLUTO_MAX_HZ 6000000000LL
#define IQ_RATE 2400000
#define AUDIO_RATE 24000
#define DECIM (IQ_RATE / AUDIO_RATE)
#define PCM_CHUNK 4800
#define IIO_BUF_SAMPLES 24000
#define NOAA_RF_BW 200000
#define NFM_DEV_HZ 5000.0
#define AUDIO_GAIN 15000.0
#define DEFAULT_RF_GAIN_DB 40.0
#define PI_D 3.14159265358979323846264338327950288

static volatile sig_atomic_t g_run = 1;
struct state { double ai, aq, pi, pq, deemp, hp_in, hp_out; int n, have; };
struct profile { int noaa, stream; long long freq, lo; int rf_bw; double rf_gain; };

static void sig_handler(int s){ (void)s; g_run = 0; }
static int is_noaa(long long f){ long long a[]={162400000LL,162425000LL,162450000LL,162475000LL,162500000LL,162525000LL,162550000LL}; for(size_t i=0;i<sizeof(a)/sizeof(a[0]);i++) if(f==a[i]) return 1; return 0; }
static int parse_ll(const char *t,long long *o){ char *e=NULL; errno=0; long long v=strtoll(t,&e,10); if(errno||!e||*e) return 0; *o=v; return 1; }
static int parse_i(const char *t,int *o){ char *e=NULL; errno=0; long v=strtol(t,&e,10); if(errno||!e||*e||v<0||v>16) return 0; *o=(int)v; return 1; }
static int parse_d(const char *t,double *o){ char *e=NULL; errno=0; double v=strtod(t,&e); if(errno||!e||*e) return 0; *o=v; return 1; }
static int cmd_ok(const char *c){ int r=system(c); return !(r==-1||!WIFEXITED(r)||WEXITSTATUS(r)!=0); }
static void best(const char *label,const char *cmd){ fprintf(stderr,"PLUTO_HELPER_CFG %s %s\n", cmd_ok(cmd)?"PASS":"WARN", label); }

static int set_lo(long long hz){
    char c[256];
    snprintf(c,sizeof(c),"/usr/bin/iio_attr -u local: -c ad9361-phy altvoltage0 frequency %lld >/dev/null 2>&1",hz);
    if(cmd_ok(c)){ fprintf(stderr,"PLUTO_HELPER_CFG PASS rx_lo=%lld\n",hz); return 1; }
    const char *p[]={"/sys/bus/iio/devices/iio:device1/out_altvoltage0_RX_LO_frequency","/sys/bus/iio/devices/iio:device0/out_altvoltage0_RX_LO_frequency","/sys/bus/iio/devices/iio:device2/out_altvoltage0_RX_LO_frequency","/sys/kernel/debug/iio/iio:device1/out_altvoltage0_RX_LO_frequency","/sys/kernel/debug/iio/iio:device0/out_altvoltage0_RX_LO_frequency"};
    for(size_t i=0;i<sizeof(p)/sizeof(p[0]);i++){ FILE *f=fopen(p[i],"wb"); if(!f) continue; fprintf(f,"%lld\n",hz); if(fclose(f)==0){ fprintf(stderr,"PLUTO_HELPER_CFG PASS rx_lo_sysfs=%lld path=%s\n",hz,p[i]); return 1; } }
    fprintf(stderr,"PLUTO_HELPER_CFG FAIL rx_lo=%lld\n",hz); return 0;
}

static int config_rx(struct profile *p){
    char c[256];
    snprintf(c,sizeof(c),"/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 sampling_frequency %d >/dev/null 2>&1",IQ_RATE); best("voltage0_sampling_frequency",c);
    snprintf(c,sizeof(c),"/usr/bin/iio_attr -u local: -c ad9361-phy voltage1 sampling_frequency %d >/dev/null 2>&1",IQ_RATE); best("voltage1_sampling_frequency",c);
    snprintf(c,sizeof(c),"/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 rf_bandwidth %d >/dev/null 2>&1",p->rf_bw); best("voltage0_rf_bandwidth",c);
    snprintf(c,sizeof(c),"/usr/bin/iio_attr -u local: -c ad9361-phy voltage1 rf_bandwidth %d >/dev/null 2>&1",p->rf_bw); best("voltage1_rf_bandwidth",c);
    best("voltage0_gain_control_manual","/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 gain_control_mode manual >/dev/null 2>&1");
    best("voltage1_gain_control_manual","/usr/bin/iio_attr -u local: -c ad9361-phy voltage1 gain_control_mode manual >/dev/null 2>&1");
    snprintf(c,sizeof(c),"/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 hardwaregain %.1f >/dev/null 2>&1",p->rf_gain); best("voltage0_hardwaregain",c);
    snprintf(c,sizeof(c),"/usr/bin/iio_attr -u local: -c ad9361-phy voltage1 hardwaregain %.1f >/dev/null 2>&1",p->rf_gain); best("voltage1_hardwaregain",c);
    return set_lo(p->lo);
}

static FILE *start_iio(pid_t *child){
    int fd[2]; if(pipe(fd)!=0) return NULL; pid_t pid=fork();
    if(pid<0){ close(fd[0]); close(fd[1]); return NULL; }
    if(pid==0){ char b[32]; snprintf(b,sizeof(b),"%d",IIO_BUF_SAMPLES); dup2(fd[1],STDOUT_FILENO); close(fd[0]); close(fd[1]); execl("/usr/bin/iio_readdev","iio_readdev","-u","local:","-b",b,"-s","0","cf-ad9361-lpc",(char*)NULL); _exit(127); }
    close(fd[1]); *child=pid; return fdopen(fd[0],"rb");
}

static int16_t pcm16(double x){ double s=x*AUDIO_GAIN; if(s>32767.0) s=32767.0; if(s<-32768.0) s=-32768.0; return (int16_t)lrint(s); }

static int process(double i,double q,FILE *out,struct state *st,short *pcm,size_t *pcmn,size_t *flush){
    if(st->have){
        double cross = q*st->pi - i*st->pq; /* Pi NOAA discriminator sign */
        double dot = i*st->pi + q*st->pq;
        double phase = atan2(cross,dot);
        double demod = phase / (2.0*PI_D*NFM_DEV_HZ/(double)AUDIO_RATE);
        double a = 1.0/(1.0 + 75.0e-6*(double)AUDIO_RATE);
        double hp;
        st->deemp += a*(demod-st->deemp);
        hp = st->deemp - st->hp_in + 0.9922*st->hp_out;
        st->hp_in=st->deemp; st->hp_out=hp;
        pcm[(*pcmn)++] = pcm16(hp);
        if(*pcmn >= PCM_CHUNK){ if(fwrite(pcm,sizeof(short),*pcmn,out)!=*pcmn) return -1; *flush += *pcmn; if(*flush>=12000){ fflush(out); *flush=0; } *pcmn=0; }
    }
    st->pi=i; st->pq=q; st->have=1; return 0;
}

static int stream(FILE *in, FILE *out, struct profile *p){
    unsigned char buf[IIO_BUF_SAMPLES*8]; short pcm[PCM_CHUNK]; size_t pcmn=0, flush=0, bytes=0, frames_total=0; struct state st; int layout=-1; memset(&st,0,sizeof(st));
    while(g_run){
        size_t got=fread(buf,1,sizeof(buf),in); if(!got){ if(feof(in)) break; if(ferror(in)) return -1; continue; }
        bytes += got; size_t stride = (got>=8 && got%8==0) ? 8 : 4; if(layout!=(int)stride){ fprintf(stderr,"PLUTO_HELPER_LAYOUT %s stride=%lu stream_index=%d\n", stride==8?"dual_rx_i0q0i1q1":"single_rx_iq", (unsigned long)stride, p->stream); layout=(int)stride; }
        size_t frames=got/stride; frames_total += frames;
        for(size_t n=0;n<frames;n++){
            size_t off=n*stride; if(stride==8 && p->stream>0) off += 4; if(off+3>=got) break;
            int16_t ri=(int16_t)((uint16_t)buf[off] | ((uint16_t)buf[off+1]<<8));
            int16_t rq=(int16_t)((uint16_t)buf[off+2] | ((uint16_t)buf[off+3]<<8));
            st.ai += (double)ri; st.aq += (double)rq; st.n++;
            if(st.n>=DECIM){ double ci=st.ai/(double)DECIM, cq=st.aq/(double)DECIM; st.ai=st.aq=0.0; st.n=0; if(process(ci,cq,out,&st,pcm,&pcmn,&flush)!=0) return -1; }
        }
    }
    if(pcmn>0){ if(fwrite(pcm,sizeof(short),pcmn,out)!=pcmn) return -1; flush += pcmn; }
    if(flush>0) fflush(out);
    fprintf(stderr,"PLUTO_HELPER_DONE bytes=%lu frames=%lu profile=%s station=%lld lo=%lld stream_index=%d rf_gain=%.1f\n",(unsigned long)bytes,(unsigned long)frames_total,p->noaa?"NOAA_CENTER_NFM":"GENERIC_CENTER",p->freq,p->lo,p->stream,p->rf_gain);
    return 0;
}

int main(int argc,char **argv){
    long long freq=0; const char *out_path=NULL; int stream_idx=0; double gain=DEFAULT_RF_GAIN_DB; FILE *out=NULL,*in=NULL; pid_t child=-1; struct profile p; int ai;
    const char *e=getenv("PLUTO_AUDIO_STREAM_INDEX"); if(e&&*e) parse_i(e,&stream_idx);
    e=getenv("PLUTO_NOAA_GAIN_DB"); if(e&&*e) parse_d(e,&gain);
    for(ai=1;ai<argc;ai++){
        if(strcmp(argv[ai],"--freq-hz")==0 && ai+1<argc){ if(!parse_ll(argv[++ai],&freq)) return 2; }
        else if(strcmp(argv[ai],"--output")==0 && ai+1<argc){ out_path=argv[++ai]; }
        else if(strcmp(argv[ai],"--stream-index")==0 && ai+1<argc){ if(!parse_i(argv[++ai],&stream_idx)) return 2; }
        else if(strcmp(argv[ai],"--gain-db")==0 && ai+1<argc){ if(!parse_d(argv[++ai],&gain)) return 2; }
        else { fprintf(stderr,"Usage: %s --freq-hz <hz> --output <path> [--stream-index 0|1] [--gain-db <db>]\n",argv[0]); return 2; }
    }
    if(freq<PLUTO_MIN_HZ || freq>PLUTO_MAX_HZ || !out_path || !*out_path) return 2; if(stream_idx<0||stream_idx>1) stream_idx=0;
    memset(&p,0,sizeof(p)); p.freq=freq; p.lo=freq; p.stream=stream_idx; p.rf_gain=gain; p.rf_bw=NOAA_RF_BW; p.noaa=is_noaa(freq);
    signal(SIGINT,sig_handler); signal(SIGTERM,sig_handler);
    fprintf(stderr,"PLUTO_NOAA_CENTER_NFM_DUAL_RX_V2 profile=%s station=%lld lo=%lld rf_bw=%d stream_index=%d rf_gain=%.1f audio_rate=%d iq_rate=%d decim=%d\n",p.noaa?"NOAA_CENTER_NFM":"GENERIC_CENTER",p.freq,p.lo,p.rf_bw,p.stream,p.rf_gain,AUDIO_RATE,IQ_RATE,DECIM);
    if(!config_rx(&p)){ fprintf(stderr,"Unable to configure Pluto RX path.\n"); return 1; }
    out=fopen(out_path,"wb"); if(!out){ perror("fopen"); return 1; } setvbuf(out,NULL,_IOFBF,65536);
    in=start_iio(&child); if(!in){ fprintf(stderr,"Unable to start iio_readdev stream.\n"); fclose(out); return 1; }
    (void)stream(in,out,&p); fclose(in); fclose(out); if(child>0){ kill(child,SIGTERM); waitpid(child,NULL,0); }
    return 0;
}
