/* Does the 6:5 resampler preserve a telephone band well enough for a modem?
 *
 * This tests MY code, not the DSP. It cannot tell you whether V.34 trains --
 * only whether the conversion between the DSP's 9600 Hz and AudioSocket's
 * 8000 Hz is transparent in 300-3400 Hz, which is the part a demodulator makes
 * bit decisions on.
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

int main(void)
{
    const int N = 9600 * 4;
    static int16_t in[38400], mid[38400], out[38400];
    int fails = 0;

    printf("  freq     level dB   (pass = within 1.0 dB across 300-3400)\n");
    for (int f = 200; f <= 3800; f += 200) {
        resamp_t down, up;
        rs_init(&down, 5, 6);        /* 9600 -> 8000 */
        rs_init(&up,   6, 5);        /* 8000 -> 9600 */
        for (int i = 0; i < N; i++)
            in[i] = (int16_t)(12000.0 * sin(2.0*M_PI*f*i/9600.0));

        int nm = rs_process(&down, in, N, mid, (int)(sizeof mid/2));
        int no = rs_process(&up, mid, nm, out, (int)(sizeof out/2));

        /* Skip the group delay of both filters before measuring. */
        int skip = 400;
        if (no <= skip + 1000) { printf("  %4d Hz  TOO SHORT\n", f); fails++; continue; }
        double a[8000], b[8000]; int n = 4000;
        for (int i = 0; i < n; i++) { a[i] = in[skip+i]; b[i] = out[skip+i]; }
        double db = 20.0 * log10(rms(b, n) / rms(a, n));

        const char *verdict;
        if (f >= 300 && f <= 3400) { verdict = (fabs(db) <= 1.0) ? "ok" : "FAIL"; if (fabs(db) > 1.0) fails++; }
        else verdict = "(outside band)";
        printf("  %4d Hz  %+7.2f    %s\n", f, db, verdict);
    }

    /* DC gain: a resampler that is not unity at DC quietly changes level. */
    resamp_t d2, u2; rs_init(&d2, 5, 6); rs_init(&u2, 6, 5);
    for (int i = 0; i < N; i++) in[i] = 8000;
    int nm = rs_process(&d2, in, N, mid, (int)(sizeof mid/2));
    int no = rs_process(&u2, mid, nm, out, (int)(sizeof out/2));
    long sum = 0; int c = 0;
    for (int i = 1000; i < no - 100; i++) { sum += out[i]; c++; }
    double dcerr = c ? (double)sum/c / 8000.0 : 0;
    printf("\n  DC gain %.4f  %s\n", dcerr, fabs(dcerr-1.0) < 0.01 ? "ok" : "FAIL");
    if (fabs(dcerr-1.0) >= 0.01) fails++;

    printf("\n%s\n", fails ? "RESAMPLER FAILED" : "resampler ok: telephone band is transparent through 9600->8000->9600");
    return fails ? 1 : 0;
}
