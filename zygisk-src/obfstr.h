#pragma once

// 編譯期 XOR 字串：避免 .rodata 出現可 grep 的常數（模組路徑、package 名、
// hook 目標類名）。模組 .so 會在未排除 app 的 /proc/self/maps 裡被宿主 app
// 直接讀取，明文常數等於自白書。
//
// 編碼在 build-time 由 gen_obf_strings.py 完成（輸出 gen/obf_strings.h）；
// 曾試過 constexpr constructor 在編譯期編碼，但 clang 仍把原始 string
// literal 以 mergeable-string 片段形式 emit 進 .rodata，明文照樣外洩，
// 因此改為完全不讓明文進入 translation unit。
//
// runtime 只解到呼叫端的 stack buffer、用完以 secureClear 抹除——刻意不
// 就地解碼：解開的明文會長駐 .data/.bss，常駐進程的記憶體同樣可被宿主讀走。

#include <cstddef>
#include <cstring>

namespace obf {

// 必須與 gen_obf_strings.py 的 kkey() 保持一致。
constexpr char kKey(size_t i) {
    return static_cast<char>(0xA5u ^ ((i * 31u + 7u) & 0x7Fu));
}

// 把 gen/obf_strings.h 的編碼陣列（含編碼過的 NUL）解到 out；
// out 至少配置 N bytes（即 kObf<name>Len + 1）。
// 陣列宣告為 const volatile 是刻意的：阻止 optimizer 把 XOR 結果在編譯期
// 常數折疊回明文常數池（fold 一出現，明文照樣進 .rodata，編碼白做）。
template <size_t N>
inline void decodeStr(const volatile unsigned char (&enc)[N], char *out) {
    for (size_t i = 0; i < N; ++i)
        out[i] = static_cast<char>(enc[i] ^ static_cast<unsigned char>(kKey(i)));
}

template <size_t N>
inline void secureClear(char (&buf)[N]) {
    volatile char *p = buf;
    for (size_t i = 0; i < N; ++i) p[i] = 0;
}

}  // namespace obf
