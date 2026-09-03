/* Does the 6:5 resampler preserve the band a V.34 demodulator makes bit
 * decisions on?
 *
 * This tests MY code, not the DSP. It cannot tell you whether V.34 trains --
 * only whether the conversion between the DSP's 9600 Hz and AudioSocket's
 * 8000 Hz is transparent where the signal actually lives.
 *
 * THE BAND THIS ASSERTS OVER IS 300-3900 Hz, AND THAT IS THE POINT.
 *
 * The previous version of this file asserted 300-3400 and passed, while the
 * filter it was testing was 1.9 dB down at 3674 Hz and 7.1 dB down at 3888 --
 * the top of the 3429-baud V.34 constellation that carries 31200 and 33600.
 * A test whose pass band stops below the signal band cannot see the defect it
 * exists to catch, and this one did not, for the entire time V.34 was being
 * debugged one layer above it. If a future rate needs more than 3900 Hz, move
 * this number FIRST and let it fail.
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#define RS_SELFTEST
#include "slmbridge.c"

static double rms(const double *x, int n)
{ double s = 0; for (int i = 0; i < n; i++) s += x[i]*x[i]; return sqrt(s/n); }

/* Single-bin DFT magnitude, Hann-windowed, for the alias probe. */
static double bin_mag(const int16_t *x, int n, double f, double fs)
{
    double re = 0, im = 0, wsum = 0;
    for (int i = 0; i < n; i++) {
        double w = 0.5 - 0.5*cos(2.0*M_PI*i/(n-1));
        double a = 2.0*M_PI*f*i/fs;
        re += w*x[i]*cos(a); im -= w*x[i]*sin(a); wsum += w;
    }
    return sqrt(re*re + im*im) / (wsum/2.0);
}

static int sweep(const char *label, double tol)
{
    const int N = 9600 * 4;
    static int16_t in[38400], mid[38400], out[38400];
    int fails = 0;
    printf("\n%s -- round trip 9600->8000->9600\n", label);
    printf("  freq     level dB   (pass = within %.2f dB across 300-3900)\n", tol);
    for (int f = 200; f <= 4000; f += 200) {
        resamp_t down, up;
        rs_init_pair(&up, &down, 9600);
        for (int i = 0; i < N; i++)
            in[i] = (int16_t)(12000.0 * sin(2.0*M_PI*f*i/9600.0));

        int nm = rs_process(&down, in, N, mid, (int)(sizeof mid/2));
        int no = rs_process(&up, mid, nm, out, (int)(sizeof out/2));

        int skip = 600;
        if (no <= skip + 1000) { printf("  %4d Hz  TOO SHORT\n", f); fails++; continue; }
        double a[8000], b[8000]; int n = 4000;
        for (int i = 0; i < n; i++) { a[i] = in[skip+i]; b[i] = out[skip+i]; }
        double db = 20.0 * log10(rms(b, n) / rms(a, n));

        const char *verdict;
        if (f >= 300 && f <= 3900) { verdict = (fabs(db) <= tol) ? "ok" : "FAIL"; if (fabs(db) > tol) fails++; }
        else verdict = "(outside asserted band)";
        printf("  %4d Hz  %+7.2f    %s\n", f, db, verdict);
    }
    return fails;
}

/* Aliasing: an 8 kHz input tone at f upsampled to 9600 puts an image at
 * 8000-f. For f < 3200 that image lands above 4800 and folds back to 1600+f,
 * i.e. straight into the band, unless the filter's stopband kills it. This is
 * the failure the cutoff move could have caused and did not. */
static int alias_up(double tol_db)
{
    const int N = 8000 * 4;
    static int16_t in[32000], out[40000];
    int fails = 0;
    printf("\nalias rejection -- 8000->9600 up-resample, image of f folded to 1600+f\n");
    for (int f = 800; f <= 3000; f += 400) {
        resamp_t up, down;
        rs_init_pair(&up, &down, 9600);
        for (int i = 0; i < N; i++)
            in[i] = (int16_t)(12000.0 * sin(2.0*M_PI*f*i/8000.0));
        int no = rs_process(&up, in, N, out, (int)(sizeof out/2));
        int skip = 600, n = no - skip - 100; if (n > 16000) n = 16000;
        double want = bin_mag(out+skip, n, f, 9600.0);
        double ghost = bin_mag(out+skip, n, 1600.0+f, 9600.0);
        double db = 20.0*log10((ghost + 1e-9) / (want + 1e-9));
        int bad = db > tol_db;
        if (bad) fails++;
        printf("  tone %4d Hz -> image at %4.0f Hz  %+7.1f dBc   %s\n",
               f, 1600.0+f, db, bad ? "FAIL" : "ok");
    }
    return fails;
}

int main(void)
{
    int fails = 0;
    const int N = 9600 * 4;
    static int16_t in[38400], mid[38400], out[38400];

    /* Report the legacy design too, so the regression this replaced stays
       visible in the test output rather than only in a commit message. */
    setenv("SLMBRIDGE_RS_PROFILE", "legacy", 1);
    sweep("LEGACY profile (32 tap, fc 4000, Blackman) -- reference only", 99.0);
    unsetenv("SLMBRIDGE_RS_PROFILE");

    fails += sweep("DEFAULT profile (96/112 tap, fc 4000, Kaiser b=5)", 0.50);
    fails += alias_up(-60.0);

    /* NARROW profile: deliberately NOT transparent at the top of the band.
       It band-limits the RECEIVE path to SLMBRIDGE_RS_FC (default 3400) because
       the caller's ATA is already 10.7 dB down at 3750 Hz, so reconstructing
       that region flat mostly amplifies noise. It is reported, not asserted --
       failing the 300-3900 transparency check is the WHOLE POINT of it, and
       asserting transparency here would make the test contradict the design. */
    setenv("SLMBRIDGE_RS_PROFILE", "narrow", 1);
    sweep("NARROW profile (96/112 tap, rx fc 3800) -- reference, rolloff intended", 99.0);
    unsetenv("SLMBRIDGE_RS_PROFILE");

    /* DC gain: a resampler that is not unity at DC quietly changes level. */
    resamp_t d2, u2; rs_init_pair(&u2, &d2, 9600);
    for (int i = 0; i < N; i++) in[i] = 8000;
    int nm = rs_process(&d2, in, N, mid, (int)(sizeof mid/2));
    int no = rs_process(&u2, mid, nm, out, (int)(sizeof out/2));
    long sum = 0; int c = 0;
    for (int i = 1000; i < no - 100; i++) { sum += out[i]; c++; }
    double dcerr = c ? (double)sum/c / 8000.0 : 0;
    printf("\n  DC gain %.4f  %s\n", dcerr, fabs(dcerr-1.0) < 0.01 ? "ok" : "FAIL");
    if (fabs(dcerr-1.0) >= 0.01) fails++;

    printf("\n%s\n", fails ? "RESAMPLER FAILED"
        : "resampler ok: 300-3900 Hz transparent and aliases rejected");
    return fails ? 1 : 0;
}
