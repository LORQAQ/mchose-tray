/*
 * strings.exe — 简易 strings 实现（用于对大型二进制做快速字符串提取）
 * ---------------------------------------------------------------------------
 * 为什么要自己写：PowerShell 逐字节处理 180MB 的 Electron 主程序会极慢，
 * 而本机没有 Sysinternals strings。这个实现按块 fread，速度接近原版。
 *
 * 编译（MinGW-w64）：
 *   gcc -O2 -o strings.exe strings.c
 *
 * 用法：
 *   strings.exe <文件> [最小长度=5]
 *   strings.exe "D:\...\index.jsc" 5 > out.txt
 */

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: strings <file> [minLen=5]\n");
        return 2;
    }
    int min = (argc > 2) ? atoi(argv[2]) : 5;
    if (min < 1) min = 1;

    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) { perror("open"); return 1; }

    const size_t BLOCK = 1 << 20;
    unsigned char *in = (unsigned char *)malloc(BLOCK);
    /* 当前字符串缓冲：最坏情况整块都是可打印字符 */
    size_t cap = BLOCK + 1;
    char *cur = (char *)malloc(cap);
    size_t len = 0;

    if (in == NULL || cur == NULL) { fprintf(stderr, "oom\n"); return 1; }

    size_t got;
    while ((got = fread(in, 1, BLOCK, f)) > 0) {
        for (size_t i = 0; i < got; i++) {
            unsigned char c = in[i];
            if (c >= 32 && c < 127) {
                if (len + 1 >= cap) {
                    cap *= 2;
                    cur = (char *)realloc(cur, cap);
                    if (cur == NULL) { fprintf(stderr, "oom\n"); return 1; }
                }
                cur[len++] = (char)c;
            } else {
                if ((int)len >= min) { fwrite(cur, 1, len, stdout); fputc('\n', stdout); }
                len = 0;
            }
        }
    }
    if ((int)len >= min) { fwrite(cur, 1, len, stdout); fputc('\n', stdout); }

    free(in);
    free(cur);
    fclose(f);
    return 0;
}
