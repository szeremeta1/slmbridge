/* Generated sample bytes over local AF_UNIX socketpairs only. No modem, SIP,
 * network listener, TTY, pppd or recorded waveform is opened or executed. */
#define _GNU_SOURCE
#include <assert.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <poll.h>
static size_t send_fragment, send_calls;
static int faults;
static ssize_t injected_send(int fd, const void *p, size_t n, int flags)
{
    send_calls++;
    if (faults == 2) return 0;
    if (faults == 3) { errno = EPIPE; return -1; }
    if (faults && send_calls % 17 == 0) { errno = EINTR; return -1; }
    if (faults && send_calls % 23 == 0) { errno = EAGAIN; return -1; }
    if (send_fragment && n > send_fragment) n = send_fragment;
    return send(fd, p, n, flags);
}
#define send injected_send
#define RS_SELFTEST
#ifndef BRIDGE_TEST_SOURCE
#define BRIDGE_TEST_SOURCE "bridge.c"
#endif
#include BRIDGE_TEST_SOURCE
#undef send
static void put_all(int fd, const void *v, size_t n, size_t frag)
{
    const uint8_t *p = v;
    while (n) {
        size_t chunk = n < frag ? n : frag;
        ssize_t r = write(fd, p, chunk);
        if (r < 0 && errno == EINTR) continue;
        assert(r > 0); p += r; n -= (size_t)r;
    }
}
static void get_all(int fd, void *v, size_t n)
{
    uint8_t *p = v;
    while (n) {
        struct pollfd f = {.fd=fd, .events=POLLIN};
        assert(poll(&f, 1, 3000) > 0);
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        assert(r > 0); p += r; n -= (size_t)r;
    }
}
static void child_ok(pid_t child)
{
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void helper_check(int rate, int enabled, int inject, size_t frag)
{
    int a[2], p[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,a));
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,p));
    setenv("SLMBRIDGE_DSP_RATE", rate == 9600 ? "9600" : "8000", 1);
    setenv("SLMBRIDGE_TX_CLOCK", "rx", 1); setenv("SLMBRIDGE_RX_PREFILL", "0", 1);
    setenv("SLMBRIDGE_STREAM_IO", enabled ? "1" : "0", 1);
    send_fragment = inject ? 7 : 0; faults = inject;
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        close(a[0]); close(p[0]); alarm(15);
        int r = helper_main(p[1], a[1]);
        close(a[1]); close(p[1]); _exit(r);
    }
    close(a[1]); close(p[1]);
    resamp_t up_ref, down_ref;
    if (rate == 9600) rs_init_pair(&up_ref, &down_ref, rate);
    for (int frame = 0; frame < 32; frame++) {
        int16_t input[160], reply[192], up_expected[192], down_expected[160];
        for (int i = 0; i < 160; i++) input[i] = (int16_t)(frame * 31 + i * 97 - 12000);
        int ns = rate == 9600 ? 192 : 160;
        for (int i = 0; i < ns; i++) reply[i] = (int16_t)(frame * 23 + i * 43 - 10000);
        if (rate == 9600) {
            assert(rs_process(&up_ref,input,160,up_expected,192) == 192);
            assert(rs_process(&down_ref,reply,192,down_expected,160) == 160);
        } else { memcpy(up_expected,input,320); memcpy(down_expected,reply,320); }
        /* PCM for this output is available before the RX-clock trigger. */
        put_all(p[0], reply, (size_t)ns * 2, (size_t)ns * 2);
        uint8_t in[323] = {0x10,1,64}, out[323], pcm[384];
        memcpy(in + 3, input, 320); put_all(a[0], in, sizeof in, frag);
        get_all(p[0], pcm, (size_t)ns * 2); assert(!memcmp(pcm,up_expected,(size_t)ns*2));
        get_all(a[0], out, sizeof out);
        assert(out[0] == 0x10 && out[1] == 1 && out[2] == 64);
        assert(!memcmp(out + 3, down_expected, 320));
    }
    uint8_t hangup[3] = {0,0,0}; put_all(a[0],hangup,3,3);
    child_ok(child); close(a[0]); close(p[0]);
    printf("PASS actual helper %dHz flag%d inject%d input_fragment%zu, 32 exact up/down frames\n",rate,enabled,inject,frag);
}
static void broker_check(void)
{
    enum { TOTAL = 128 * 1024 };
    uint8_t source[2][TOTAL], got[2][TOTAL];
    for (int k=0;k<2;k++) for(size_t i=0;i<TOTAL;i++) source[k][i]=(uint8_t)(i*97+i/251+k);
    int a[2],b[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,a));assert(!socketpair(AF_UNIX,SOCK_STREAM,0,b));
    int cap=4096;
    for(int k=0;k<2;k++){assert(!setsockopt(a[k],SOL_SOCKET,SO_SNDBUF,&cap,sizeof cap));assert(!setsockopt(b[k],SOL_SOCKET,SO_SNDBUF,&cap,sizeof cap));}
    setenv("SLMBRIDGE_STREAM_IO","1",1); send_fragment=7; faults=1;
    pid_t child=fork();assert(child>=0);
    if(!child){close(a[0]);close(b[0]);alarm(20);int t=-1;g_listen_fd=-1;int r=relay_and_watch(a[1],b[1],&t);close(a[1]);close(b[1]);_exit(r?1:0);}
    close(a[1]);close(b[1]);
    int fd[2]={a[0],b[0]},ended[2]={0},shut[2]={0};size_t written[2]={0},read_n[2]={0};
    for(int rounds=0;!ended[0]||!ended[1];rounds++){
        assert(rounds<1000000);
        struct pollfd f[2];
        for(int k=0;k<2;k++){f[k].fd=fd[k];f[k].events=(written[k]<TOTAL?POLLOUT:0)|POLLIN;}
        assert(poll(f,2,3000)>0);
        for(int k=0;k<2;k++){
            if(written[k]<TOTAL&&(f[k].revents&POLLOUT)){
                size_t chunk=TOTAL-written[k]; if(chunk>4096)chunk=4096;
                ssize_t n=send(fd[k],source[k]+written[k],chunk,MSG_DONTWAIT|MSG_NOSIGNAL);
                if(n>0)written[k]+=(size_t)n;else assert(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR);
            }
            if(written[k]==TOTAL&&!shut[k]){assert(!shutdown(fd[k],SHUT_WR));shut[k]=1;}
            if(!ended[k]&&(f[k].revents&(POLLIN|POLLHUP))){
                uint8_t extra;void *dst=read_n[k]<TOTAL?(void *)(got[k]+read_n[k]):&extra;
                size_t room=read_n[k]<TOTAL?TOTAL-read_n[k]:1;
                ssize_t n=recv(fd[k],dst,room,MSG_DONTWAIT);
                if(n>0){read_n[k]+=(size_t)n;assert(read_n[k]<=TOTAL);}else if(!n)ended[k]=1;else assert(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR);
            }
        }
    }
    assert(read_n[0]==TOTAL&&read_n[1]==TOTAL);
    assert(!memcmp(got[0],source[1],TOTAL)&&!memcmp(got[1],source[0],TOTAL));
    child_ok(child);close(a[0]);close(b[0]);
    puts("PASS actual broker 128KiB each direction, 7-byte sends, EINTR/EAGAIN, 4KiB socket buffers, duplex half-close, exact byte streams");
}
static void helper_blocked_check(int block_pcm)
{
    int a[2],p[2],logfd[2];
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,a));assert(!socketpair(AF_UNIX,SOCK_STREAM,0,p));assert(!pipe(logfd));
    int target=block_pcm?p[1]:a[1],cap=4096;
    assert(!setsockopt(target,SOL_SOCKET,SO_SNDBUF,&cap,sizeof cap));
    uint8_t filler[4096];memset(filler,0xa5,sizeof filler);size_t filled=0;
    for(;;){ssize_t n=send(target,filler,sizeof filler,MSG_DONTWAIT|MSG_NOSIGNAL);if(n>0){filled+=(size_t)n;assert(filled<1024*1024);}else{assert(errno==EAGAIN||errno==EWOULDBLOCK);break;}}
    setenv("SLMBRIDGE_DSP_RATE","8000",1);setenv("SLMBRIDGE_TX_CLOCK","rx",1);setenv("SLMBRIDGE_RX_PREFILL","0",1);setenv("SLMBRIDGE_STREAM_IO","1",1);
    faults=0;send_fragment=0;
    pid_t child=fork();assert(child>=0);
    if(!child){close(a[0]);close(p[0]);close(logfd[0]);assert(dup2(logfd[1],2)>=0);close(logfd[1]);alarm(10);int r=helper_main(p[1],a[1]);_exit(r);}
    close(a[1]);close(p[1]);close(logfd[1]);
    int16_t samples[160];for(int i=0;i<160;i++)samples[i]=(int16_t)(i*51-4000);
    put_all(p[0],samples,320,320);
    uint8_t frame[323]={0x10,1,64},got[323];memcpy(frame+3,samples,320);put_all(a[0],frame,323,323);
    if(block_pcm){get_all(a[0],got,323);assert(!memcmp(got,frame,323));struct pollfd f={.fd=a[0],.events=POLLIN};assert(poll(&f,1,350)==0);}
    else {get_all(p[0],got,320);assert(!memcmp(got,samples,320));assert(poll(NULL,0,350)==0);}
    size_t left=filled;while(left){size_t n=left<sizeof filler?left:sizeof filler;get_all(block_pcm?p[0]:a[0],filler,n);for(size_t i=0;i<n;i++)assert(filler[i]==0xa5);left-=n;}
    if(block_pcm){get_all(p[0],got,320);assert(!memcmp(got,samples,320));}
    if(!block_pcm){get_all(a[0],got,323);assert(!memcmp(got,frame,323));}
    uint8_t hangup[3]={0};put_all(a[0],hangup,3,3);child_ok(child);
    char logs[4096];ssize_t n=read(logfd[0],logs,sizeof logs-1);assert(n>0);logs[n]=0;
    assert(strstr(logs,"tx=1 rx=1 starved=0")&&strstr(logs,"error=0")&&strstr(logs,"to_first_pending=0 to_second_pending=0"));
    close(logfd[0]);close(a[0]);close(p[0]);
    printf("PASS helper actual %s backpressure350ms, exact pending suffix, one RX produces one TX without fallback insertion\n",block_pcm?"PCM":"AudioSocket");
}

static void actual_phase_check(void)
{
    int16_t input[4096]={0},output[8192];
    resamp_t up,down;rs_init_pair(&up,&down,9600);
    for(int phase=0;phase<6;phase++) for(int trial=0;trial<51;trial++){
        int n=trial<49?trial:trial==49?4095:4096;
        resamp_t r=down;r.phase=phase;
        size_t expected=bio_outputs((size_t)n,r.L,r.M,r.phase);
        for(size_t i=0;i<8192;i++)output[i]=12345;
        assert(rs_process(&r,input,n,output,8192)==(int)expected);
        assert(output[expected]==12345);
    }
    puts("PASS exact output-count formula against actual downsampler for all6 phases, every block0..48 plus4095/4096, canaries preserved");
}
static void helper_failure_check(int kind)
{
    int a[2],p[2],logfd[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,a));assert(!socketpair(AF_UNIX,SOCK_STREAM,0,p));assert(!pipe(logfd));
    setenv("SLMBRIDGE_DSP_RATE","8000",1);setenv("SLMBRIDGE_TX_CLOCK","rx",1);setenv("SLMBRIDGE_RX_PREFILL","0",1);setenv("SLMBRIDGE_STREAM_IO","1",1);
    faults=kind<=3?kind:0;send_fragment=0;
    pid_t child=fork();assert(child>=0);
    if(!child){close(a[0]);close(p[0]);close(logfd[0]);assert(dup2(logfd[1],2)>=0);close(logfd[1]);alarm(5);int r=helper_main(p[1],a[1]);_exit(r==1?0:1);}
    close(a[1]);close(p[1]);close(logfd[1]);
    if(kind==4){uint8_t odd=0x51;put_all(p[0],&odd,1,1);assert(!shutdown(p[0],SHUT_WR));}
    else {uint8_t frame[323]={0x10,1,64};if(kind==5)frame[2]=63;put_all(a[0],frame,kind==5?3:323,323);}
    child_ok(child);char logs[4096];ssize_t n=read(logfd[0],logs,sizeof logs-1);assert(n>0);logs[n]=0;
    assert(strstr(logs,"bridge_stream_io:")&&!strstr(logs,"error=0"));
    if(kind==4)assert(strstr(logs,"partial_input=1"));
    close(logfd[0]);close(a[0]);close(p[0]);
    printf("PASS actual helper visible failure kind%d (%s)\n",kind,kind==2?"zero send":kind==3?"EPIPE":kind==4?"odd PCM EOF":"odd AUDIO length");
}

int main(void)
{
#ifndef __linux__
    puts("SKIP full integration: Linux target AF_UNIX readiness semantics required"); return 0;
#endif
    alarm(60);
    setenv("SLMBRIDGE_RS_DUMP","",1);setenv("SLMBRIDGE_RS_PROFILE","",1);
    setenv("SLMBRIDGE_RS_FC","3800",1);setenv("SLMBRIDGE_ELASTIC","0",1);
    setenv("SLMBRIDGE_RING_FRAMES","",1);setenv("SLMBRIDGE_RXGAP_LOG_MS","0",1);
    helper_check(8000,0,0,323);helper_check(8000,1,0,323);helper_check(8000,1,1,1);
    helper_check(9600,0,0,323);helper_check(9600,1,0,323);helper_check(9600,1,1,1);
    setenv("SLMBRIDGE_RS_PROFILE","legacy",1);
    helper_check(9600,0,0,323);helper_check(9600,1,1,1);
    setenv("SLMBRIDGE_RS_PROFILE","narrow",1);
    helper_check(9600,0,0,323);helper_check(9600,1,1,1);
    broker_check();
    helper_blocked_check(0);helper_blocked_check(1);
    actual_phase_check();
    helper_failure_check(2);helper_failure_check(3);helper_failure_check(4);helper_failure_check(5);
    return 0;
}
