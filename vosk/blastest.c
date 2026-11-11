/* blastest: cblas_sgemm of the linked OpenBLAS against a plain loop, odd sizes and all transpositions (checks the
   8x4 kernel of kernel/arm/sgemm_kernel_8x4_wp.c, PLAN.md §59.5). Host: clang blastest.c <libopenblas.a> -lm */
#include <cblas.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    int sizes[] = {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 33, 64, 127, 130, 257, 300};
    int ns = sizeof sizes / sizeof *sizes, bad = 0, runs = 0;
    srand(1);
    for (int a = 0; a < ns; a++) for (int b = 0; b < ns; b += 2) for (int c = 0; c < ns; c += 3)
    for (int ta = 0; ta < 2; ta++) for (int tb = 0; tb < 2; tb++) {
        int m = sizes[a], n = sizes[b], k = sizes[c];
        int lda = (ta ? k : m) + 1, ldb = (tb ? n : k) + 2, ldc = m + 3;
        float *A = malloc(sizeof(float) * lda * (ta ? m : k)), *B = malloc(sizeof(float) * ldb * (tb ? k : n));
        float *C = malloc(sizeof(float) * ldc * n), *R = malloc(sizeof(float) * ldc * n);
        for (int i = 0; i < lda * (ta ? m : k); i++) A[i] = rand() / (float)RAND_MAX - 0.5f;
        for (int i = 0; i < ldb * (tb ? k : n); i++) B[i] = rand() / (float)RAND_MAX - 0.5f;
        for (int i = 0; i < ldc * n; i++) C[i] = R[i] = rand() / (float)RAND_MAX - 0.5f;
        float alpha = 0.7f, beta = 0.3f;
        cblas_sgemm(CblasColMajor, ta ? CblasTrans : CblasNoTrans, tb ? CblasTrans : CblasNoTrans, m, n, k, alpha, A,
                    lda, B, ldb, beta, C, ldc);
        double worst = 0;
        for (int j = 0; j < n; j++) for (int i = 0; i < m; i++) {
            double s = 0;
            for (int l = 0; l < k; l++)
                s += (double)(ta ? A[l + i * lda] : A[i + l * lda]) * (tb ? B[j + l * ldb] : B[l + j * ldb]);
            double r = alpha * s + beta * R[i + j * ldc];
            double e = fabs(r - C[i + j * ldc]) / (1 + fabs(r));
            if (e > worst) worst = e;
        }
        for (int j = 0; j < n; j++) for (int i = m; i < ldc; i++)  /* padding rows untouched */
            if (C[i + j * ldc] != R[i + j * ldc]) worst = 1;
        runs++;
        if (worst > 1e-4) { if (bad++ < 10) printf("BAD m=%d n=%d k=%d ta=%d tb=%d err=%g\n", m, n, k, ta, tb, worst); }
        free(A); free(B); free(C); free(R);
    }
    printf("%d/%d sgemm ok\n", runs - bad, runs);
    return bad != 0;
}
