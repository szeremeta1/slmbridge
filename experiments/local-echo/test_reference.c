#include "local_echo.h"
#include <assert.h>
#include <stdlib.h>

static void frame(int16_t *p, int16_t n) { for (int i=0;i<160;i++) p[i]=n; }
static void cycle(struct local_echo *s, int16_t *rx, int16_t *tx)
{
    assert(le_receive(s, rx, 320) == 0);
    assert(le_stage(s, tx) == 0);
    assert(le_commit(s) == 0);
}
int main(void)
{
    int16_t rx[160], tx[160];
    assert(le_config(NULL)==LE_OFF && le_config("off")==LE_OFF);
    assert(le_config("observe")==LE_OBSERVE && le_config("mix")==LE_MIX);
    assert(le_config("")==-1 && le_config("1")==-1 && le_config("Mix")==-1);
    struct local_echo s={.mode=LE_MIX};
    frame(rx, 123); frame(tx, 10000); cycle(&s,rx,tx);
    assert(rx[0]==123 && s.warmup==1 && s.mixed==0);
    frame(rx, 200); frame(tx, -20000); cycle(&s,rx,tx);
    assert(rx[0]==300 && s.mixed==1); /* Previous frame, not new TX. */
    frame(rx, 200); frame(tx, 0); cycle(&s,rx,tx);
    assert(rx[0]==0 && s.committed==3);
    /* Full input range: signed rounding, saturation and odd byte alignment. */
    unsigned long clips=0;
    for (int ref=-32768;ref<=32767;ref++) {
        s=(struct local_echo){.mode=LE_MIX};
        frame(rx,0); frame(tx,(int16_t)ref); cycle(&s,rx,tx);
        uint8_t unaligned[321];
        int16_t value = ref<0 ? INT16_MIN : INT16_MAX;
        for (int i=0;i<160;i++) memcpy(unaligned+1+i*2,&value,2);
        assert(le_receive(&s,unaligned+1,320)==0);
        int addition = (int)((abs(ref)+50)/100) * (ref<0 ? -1 : 1);
        int expected=value+addition;
        if(expected>32767) expected=32767;
        if(expected< -32768) expected= -32768;
        int16_t actual; memcpy(&actual,unaligned+1,2);
        assert(actual==expected);
        assert(s.clipped==(addition ? 160u:0u)); clips += s.clipped;
    }
    assert(clips>0);
    /* Observe validates reference ownership but preserves bytes. */
    s=(struct local_echo){.mode=LE_OBSERVE};
    frame(rx,123); frame(tx,32767); cycle(&s,rx,tx); cycle(&s,rx,tx);
    assert(rx[0]==123 && s.mixed==0 && s.committed==2);
    /* Staging does not make a partly written frame available. */
    s=(struct local_echo){.mode=LE_MIX}; frame(rx,0);
    assert(le_receive(&s,rx,320)==0 && le_stage(&s,tx)==0);
    assert(s.reference[0]==0 && s.pending);
    assert(le_receive(&s,rx,320)==-1 && s.invalid);
    assert(le_commit(&s)==-1); /* An invalid epoch cannot quietly recover. */
    s=(struct local_echo){.mode=LE_MIX};
    assert(le_stage(&s,tx)==-1); /* Fallback before first input. */
    s=(struct local_echo){.mode=LE_MIX}; cycle(&s,rx,tx);
    assert(le_stage(&s,tx)==-1); /* Extra fallback after a valid frame. */
    s=(struct local_echo){.mode=LE_MIX};
    assert(le_commit(&s)==-1);
    for(size_t n=0;n<=8192;n++) if(n!=320) {
        s=(struct local_echo){.mode=LE_MIX};
        assert(le_receive(&s,NULL,n)==-1);
    }
    s=(struct local_echo){.mode=LE_MIX,.received=UINT64_MAX,.committed=UINT64_MAX};
    assert(le_receive(&s,rx,320)==-1);
    /* Disabled really is a no-op, including unusual legacy frame contracts. */
    s=(struct local_echo){.mode=LE_OFF};
    assert(le_receive(&s,NULL,8192)==0 && le_stage(&s,NULL)==0 && le_commit(&s)==0);
    assert(!s.received && !s.committed && !s.invalid);
    le_report(&s);
    puts("PASS: causal commit, one-frame reference, all signed values, saturation, invalid epochs and disabled preservation");
    return 0;
}
