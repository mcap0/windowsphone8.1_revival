// DTW cost between two feature sequences (row-major doubles), steps (1,0) (0,1) (1,1), Sakoe-Chiba band.
// Returns the summed Euclidean distance along the best path divided by (n + m). Build: gcc -O2 -shared -fPIC -o dtw.so dtw.c -lm
#include <math.h>
#include <stdlib.h>
double dtw_cost(const double* A, int n, const double* B, int m, int d, double band) {
    int w = (int)(band * (n > m ? n : m));
    if (w < abs(n - m) + 5) w = abs(n - m) + 5;
    double* prev = malloc(sizeof(double) * (m + 1));
    double* cur = malloc(sizeof(double) * (m + 1));
    const double INF = 1e300;
    for (int j = 0; j <= m; ++j) prev[j] = INF;
    prev[0] = 0;
    for (int i = 1; i <= n; ++i) {
        for (int j = 0; j <= m; ++j) cur[j] = INF;
        int jc = (int)((long long)i * m / n), lo = jc - w < 1 ? 1 : jc - w, hi = jc + w > m ? m : jc + w;
        for (int j = lo; j <= hi; ++j) {
            const double* a = A + (size_t)(i - 1) * d;
            const double* b = B + (size_t)(j - 1) * d;
            double s = 0;
            for (int k = 0; k < d; ++k) { double t = a[k] - b[k]; s += t * t; }
            double best = prev[j - 1];
            if (prev[j] < best) best = prev[j];
            if (cur[j - 1] < best) best = cur[j - 1];
            cur[j] = sqrt(s) + best;
        }
        double* t = prev; prev = cur; cur = t;
    }
    double r = prev[m] / (n + m);
    free(prev); free(cur);
    return r;
}
