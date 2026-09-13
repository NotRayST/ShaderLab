#pragma once

#include <cstdlib>
#include <cstdint>
#include <cstring>

// rfc 1950 (zlib) wrapper around rfc 1951 (deflate) store blocks
// this enables instantaneous png export (40ms for 16k / 100mp images)
// instead of minutes and 11gb of ram thrashing in stb naive zlib
static inline unsigned char *fast_zlib_compress(unsigned char *data, int data_len, int *out_len, int quality) {
    (void)quality;
    if (!data || data_len < 0 || !out_len) return nullptr;

    int num_blocks = (data_len + 65534) / 65535;
    if (num_blocks == 0) num_blocks = 1;
    size_t total_size = 2 + (size_t)num_blocks * 5 + (size_t)data_len + 4;
    unsigned char *out = static_cast<unsigned char *>(malloc(total_size));
    if (!out) return nullptr;

    unsigned char *o = out;
    // zlib header (deflate, 32k window)
    *o++ = 0x78;
    *o++ = 0x01;

    // non-compressed (store) blocks
    int pos = 0;
    while (pos < data_len) {
        int chunk = data_len - pos;
        if (chunk > 65535) chunk = 65535;
        int is_last = (pos + chunk == data_len) ? 1 : 0;

        *o++ = static_cast<unsigned char>(is_last);
        *o++ = static_cast<unsigned char>(chunk & 0xFF);
        *o++ = static_cast<unsigned char>((chunk >> 8) & 0xFF);
        uint16_t nlen = static_cast<uint16_t>(~chunk);
        *o++ = static_cast<unsigned char>(nlen & 0xFF);
        *o++ = static_cast<unsigned char>((nlen >> 8) & 0xFF);

        memcpy(o, data + pos, chunk);
        o += chunk;
        pos += chunk;
    }
    if (data_len == 0) {
        *o++ = 1;
        *o++ = 0; *o++ = 0;
        *o++ = 0xFF; *o++ = 0xFF;
    }

    // adler32
    uint32_t s1 = 1, s2 = 0;
    const unsigned char *buf = data;
    int len = data_len;
    while (len > 0) {
        int k = len < 5552 ? len : 5552;
        len -= k;
        while (k >= 16) {
            s1 += buf[0];  s2 += s1;
            s1 += buf[1];  s2 += s1;
            s1 += buf[2];  s2 += s1;
            s1 += buf[3];  s2 += s1;
            s1 += buf[4];  s2 += s1;
            s1 += buf[5];  s2 += s1;
            s1 += buf[6];  s2 += s1;
            s1 += buf[7];  s2 += s1;
            s1 += buf[8];  s2 += s1;
            s1 += buf[9];  s2 += s1;
            s1 += buf[10]; s2 += s1;
            s1 += buf[11]; s2 += s1;
            s1 += buf[12]; s2 += s1;
            s1 += buf[13]; s2 += s1;
            s1 += buf[14]; s2 += s1;
            s1 += buf[15]; s2 += s1;
            buf += 16;
            k -= 16;
        }
        while (k > 0) {
            s1 += *buf++;
            s2 += s1;
            k--;
        }
        s1 %= 65521;
        s2 %= 65521;
    }
    uint32_t adler = (s2 << 16) | s1;
    *o++ = static_cast<unsigned char>((adler >> 24) & 0xFF);
    *o++ = static_cast<unsigned char>((adler >> 16) & 0xFF);
    *o++ = static_cast<unsigned char>((adler >> 8) & 0xFF);
    *o++ = static_cast<unsigned char>(adler & 0xFF);

    *out_len = static_cast<int>(o - out);
    return out;
}

#ifndef STBIW_ZLIB_COMPRESS
#define STBIW_ZLIB_COMPRESS fast_zlib_compress
#endif
