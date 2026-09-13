/* Exercise the real transformed helper over local socket pairs. The inherited
 * fixture provides bounded reads and injected short/EINTR/EAGAIN writes. */
#define main legacy_integration_main
#include "../stream-io/test_integration.c"
#undef main

static void echo_fixture_defaults(void)
{
    setenv("SLMBRIDGE_RS_DUMP", "", 1); setenv("SLMBRIDGE_RS_PROFILE", "", 1);
    setenv("SLMBRIDGE_RS_FC", "3800", 1); setenv("SLMBRIDGE_ELASTIC", "0", 1);
    setenv("SLMBRIDGE_RING_FRAMES", "", 1); setenv("SLMBRIDGE_RXGAP_LOG_MS", "0", 1);
}

static void echo_helper_check(int rate, const char *mode, int fragmented, int fallback, int prefill)
{
    int a[2], p[2], logs[2];
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,a));
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,p)); assert(!pipe(logs));
    echo_fixture_defaults(); setenv("SLMBRIDGE_DSP_RATE",rate==9600?"9600":"8000",1);
    setenv("SLMBRIDGE_TX_CLOCK","rx",1); setenv("SLMBRIDGE_RX_PREFILL",prefill?"2":"0",1);
    setenv("SLMBRIDGE_STREAM_IO","1",1); setenv("SLMBRIDGE_LOCAL_ECHO",mode,1);
    send_fragment=fragmented?7:0; faults=fragmented;
    pid_t child=fork(); assert(child>=0);
    if(!child) {
        close(a[0]);close(p[0]);close(logs[0]);
        assert(dup2(logs[1],2)>=0);close(logs[1]);alarm(15);
        int r=helper_main(p[1],a[1]);_exit(r);
    }
    close(a[1]);close(p[1]);close(logs[1]);
    resamp_t up_ref, down_ref;
    if(rate==9600)rs_init_pair(&up_ref,&down_ref,rate);
    int16_t previous[160]={0}, output_history[16][160]={{0}}, zeros[160]={0};
    int frames=fallback?2:16;
    for(int frame=0;frame<frames;frame++) {
        int16_t input[160],expected[160],reply[192],up_expected[192],down_expected[160];
        int ns=rate==9600?192:160;
        for(int i=0;i<160;i++) {
            input[i]=(int16_t)(frame*101+i*97-12000);
            double addition=(double)previous[i]/100.0;
            int adjustment=addition>=0?(int)floor(addition+0.5):(int)ceil(addition-0.5);
            expected[i]=(int16_t)(input[i]+((frame && !fallback && !strcmp(mode,"mix"))?adjustment:0));
        }
        for(int i=0;i<ns;i++)reply[i]=(int16_t)(frame*431+i*43-10000);
        if(rate==9600) {
            assert(rs_process(&up_ref,expected,160,up_expected,192)==192);
            assert(rs_process(&down_ref,reply,192,down_expected,160)==160);
        } else {memcpy(up_expected,expected,320);memcpy(down_expected,reply,320);}
        memcpy(output_history[frame],down_expected,320);
        const int16_t *emitted=frame<prefill?zeros:output_history[frame-prefill];
        put_all(p[0],reply,(size_t)ns*2,(size_t)ns*2);
        uint8_t in[323]={0x10,1,64},out[323],pcm[384];
        memcpy(in+3,input,320);put_all(a[0],in,sizeof in,fragmented?1:323);
        get_all(p[0],pcm,(size_t)ns*2);assert(!memcmp(pcm,up_expected,(size_t)ns*2));
        get_all(a[0],out,sizeof out);assert(!memcmp(out+3,emitted,320));
        memcpy(previous,out+3,320);
        if(fallback && !frame) {
            /* Preserve the real fallback emission, then continue unchanged
             * audio with a permanently invalid reference epoch. */
            get_all(a[0],out,sizeof out);
            assert(out[0]==0x10 && out[1]==1 && out[2]==64);
        }
    }
    uint8_t hangup[3]={0};put_all(a[0],hangup,3,3);child_ok(child);
    char report[4096];ssize_t n=read(logs[0],report,sizeof report-1);assert(n>0);report[n]=0;
    assert(strstr(report,"error=0") && strstr(report,"to_first_pending=0 to_second_pending=0"));
    if(fallback) assert(strstr(report,"invalid=1 reason=unpaired_output received=1 committed=1 warmup=1 mixed=0 clipped=0 pending=0"));
    else {
        assert(strstr(report,"invalid=0 reason=none received=16 committed=16 warmup=1"));
        assert(strstr(report,!strcmp(mode,"mix")?"mixed=15 clipped=0 pending=0":"mixed=0 clipped=0 pending=0"));
    }
    close(logs[0]);close(a[0]);close(p[0]);
    printf("PASS real helper %dHz %s fragment=%d fallback=%d prefill=%d exact RX/TX and reference lifecycle\n",rate,mode,fragmented,fallback,prefill);
}

int main(void)
{
    for(int rate=8000;rate<=9600;rate+=1600)
        for(int fragments=0;fragments<2;fragments++) {
            echo_helper_check(rate,"observe",fragments,0,0);
            echo_helper_check(rate,"mix",fragments,0,0);
        }
    echo_helper_check(8000,"mix",0,1,0);
    echo_helper_check(9600,"mix",0,1,0);
    for(int rate=8000;rate<=9600;rate+=1600) {
        echo_helper_check(rate,"observe",1,0,2);
        echo_helper_check(rate,"mix",1,0,2);
    }
    setenv("SLMBRIDGE_LOCAL_ECHO","off",1);
    return legacy_integration_main();
}
