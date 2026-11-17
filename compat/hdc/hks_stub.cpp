/*
 * Host-side HUKS stubs: on a build machine (and in an app sandbox)
 * there is no HUKS keymaster service, so every entry point fails with
 * HKS_ERROR_NOT_SUPPORTED. hdc only reaches these through the optional
 * credential/password machinery; connect/install/uninstall/shell never do.
 */
#include "hks_api.h"

int32_t HksGenerateKey(const struct HksBlob *, const struct HksParamSet *, const struct HksParamSet *)
{
    return HKS_ERROR_NOT_SUPPORTED;
}

int32_t HksExportPublicKey(const struct HksBlob *, const struct HksParamSet *, struct HksBlob *)
{
    return HKS_ERROR_NOT_SUPPORTED;
}

int32_t HksEncrypt(const struct HksBlob *, const struct HksParamSet *,
    const struct HksBlob *, struct HksBlob *)
{
    return HKS_ERROR_NOT_SUPPORTED;
}

int32_t HksDecrypt(const struct HksBlob *, const struct HksParamSet *,
    const struct HksBlob *, struct HksBlob *)
{
    return HKS_ERROR_NOT_SUPPORTED;
}

int32_t HksDeleteKey(const struct HksBlob *, const struct HksParamSet *)
{
    return HKS_ERROR_NOT_SUPPORTED;
}

int32_t HksKeyExist(const struct HksBlob *, const struct HksParamSet *)
{
    return HKS_ERROR_NOT_SUPPORTED;
}

int32_t HksGenerateRandom(const struct HksParamSet *, struct HksBlob *)
{
    return HKS_ERROR_NOT_SUPPORTED;
}

int32_t HksInitParamSet(struct HksParamSet **)
{
    return HKS_ERROR_NOT_SUPPORTED;
}

int32_t HksAddParams(struct HksParamSet *, const struct HksParam *, uint32_t)
{
    return HKS_ERROR_NOT_SUPPORTED;
}

int32_t HksBuildParamSet(struct HksParamSet **)
{
    return HKS_ERROR_NOT_SUPPORTED;
}

void HksFreeParamSet(struct HksParamSet **)
{
}
