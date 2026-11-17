/*
 * Minimal host-side stand-in for <hks_api.h>: the 11 HUKS entry points
 * hdc_huks.cpp actually calls. Signatures mirror upstream HUKS inner API.
 */
#ifndef HDC_HOST_COMPAT_HKS_API_H
#define HDC_HOST_COMPAT_HKS_API_H

#include "hks_type.h"

#ifdef __cplusplus
extern "C" {
#endif

int32_t HksGenerateKey(const struct HksBlob *keyAlias, const struct HksParamSet *paramSetIn,
    const struct HksParamSet *paramSetOut);
int32_t HksExportPublicKey(const struct HksBlob *keyAlias, const struct HksParamSet *paramSet,
    struct HksBlob *publicKey);
int32_t HksEncrypt(const struct HksBlob *key, const struct HksParamSet *paramSet,
    const struct HksBlob *plainText, struct HksBlob *cipherText);
int32_t HksDecrypt(const struct HksBlob *key, const struct HksParamSet *paramSet,
    const struct HksBlob *cipherText, struct HksBlob *plainText);
int32_t HksDeleteKey(const struct HksBlob *keyAlias, const struct HksParamSet *paramSet);
int32_t HksKeyExist(const struct HksBlob *keyAlias, const struct HksParamSet *paramSet);
int32_t HksGenerateRandom(const struct HksParamSet *paramSet, struct HksBlob *random);

int32_t HksInitParamSet(struct HksParamSet **paramSet);
int32_t HksAddParams(struct HksParamSet *paramSet, const struct HksParam *params, uint32_t paramCnt);
int32_t HksBuildParamSet(struct HksParamSet **paramSet);
void HksFreeParamSet(struct HksParamSet **paramSet);

#ifdef __cplusplus
}
#endif

#endif /* HDC_HOST_COMPAT_HKS_API_H */
