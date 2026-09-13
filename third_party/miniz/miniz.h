/* miniz.h - Single-header deflate/inflate library interface */
#ifndef MINIZ_H
#define MINIZ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MZ_OK                       0
#define MZ_STREAM_END               1
#define MZ_NEED_DICT                2
#define MZ_ERRNO                   -1
#define MZ_STREAM_ERROR            -2
#define MZ_DATA_ERROR              -3
#define MZ_MEM_ERROR               -4
#define MZ_BUF_ERROR               -5
#define MZ_VERSION_ERROR           -6

#define MZ_NO_COMPRESSION           0
#define MZ_BEST_SPEED               1
#define MZ_BEST_COMPRESSION         9
#define MZ_DEFAULT_COMPRESSION      6

#define MZ_DEFAULT_STRATEGY         0

typedef unsigned char mz_uint8;
typedef unsigned short mz_uint16;
typedef unsigned int mz_uint32;
typedef unsigned long long mz_uint64;
typedef size_t mz_ulong;

/* Simple in-memory compression/decompression functions */
int mz_compress(unsigned char *pDest, mz_ulong *pDest_len, const unsigned char *pSource, mz_ulong source_len);
int mz_compress2(unsigned char *pDest, mz_ulong *pDest_len, const unsigned char *pSource, mz_ulong source_len, int level);
int mz_uncompress(unsigned char *pDest, mz_ulong *pDest_len, const unsigned char *pSource, mz_ulong source_len);
mz_ulong mz_compressBound(mz_ulong source_len);

/* CRC32 */
mz_uint32 mz_crc32(mz_uint32 crc, const mz_uint8 *ptr, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* MINIZ_H */
