/*
 * Minimal host-side stand-in for OpenHarmony HUKS inner headers <hks_type.h>
 * (security_huks interfaces/inner_api). Only the types/enums hdc's
 * hdc_huks.{h,cpp} reference are defined; values mirror upstream HUKS so the
 * stub implementation in hks_stub.cpp can compile against them. On host these
 * APIs are meaningless (no keymaster behind them) and always fail — credential
 * features degrade gracefully, connect/install/uninstall/shell do not touch
 * them.
 */
#ifndef HDC_HOST_COMPAT_HKS_TYPE_H
#define HDC_HOST_COMPAT_HKS_TYPE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HKS_SUCCESS 0
#define HKS_FAILURE -1
#define HKS_ERROR_NOT_SUPPORTED -40

struct HksBlob {
    uint32_t size;
    uint8_t *data;
};

/* 匿名 union：上游直接 params[i].uint32Param 访问（不带 .value） */
struct HksParam {
    uint32_t tag;
    union {
        bool boolParam;
        int32_t int32Param;
        uint32_t uint32Param;
        uint64_t uint64Param;
        struct HksBlob blob;
    };
};

struct HksParamSet {
    uint32_t paramSetSize;
    uint32_t paramsCnt;
    struct HksParam *params;
};

/* algorithm */
#define HKS_ALG_RSA 128
#define HKS_ALG_AES 32

/* key purpose */
#define HKS_KEY_PURPOSE_ENCRYPT 1
#define HKS_KEY_PURPOSE_DECRYPT 2

/* digest */
#define HKS_DIGEST_SHA256 9

/* padding */
#define HKS_PADDING_NONE 0
#define HKS_PADDING_OAEP 4

/* block mode */
#define HKS_MODE_GCM 1

/* key size */
#define HKS_RSA_KEY_SIZE_2048 2048
#define HKS_RSA_KEY_SIZE_3072 3072
#define HKS_RSA_KEY_SIZE_4096 4096
#define HKS_AES_KEY_SIZE_128 128
#define HKS_AES_KEY_SIZE_256 256

/* storage */
#define HKS_STORAGE_PERSISTENT 1
#define HKS_AUTH_STORAGE_LEVEL_DE 2

/* tags */
#define HKS_TAG_PURPOSE 1
#define HKS_TAG_ALGORITHM 2
#define HKS_TAG_KEY_SIZE 3
#define HKS_TAG_PADDING 4
#define HKS_TAG_BLOCK_MODE 5
#define HKS_TAG_DIGEST 6
#define HKS_TAG_NONCE 7
#define HKS_TAG_KEY_STORAGE_FLAG 8
#define HKS_TAG_SPECIFIC_USER_ID 9
#define HKS_TAG_AUTH_STORAGE_LEVEL 10

#ifdef __cplusplus
}
#endif

#endif /* HDC_HOST_COMPAT_HKS_TYPE_H */
