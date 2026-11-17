/*
 * Minimal host-side stand-in for OHOS <parameter.h> (base/startup/syspara).
 * On a build machine there is no system parameter service, so GetParameter
 * just returns the caller-supplied default (length included).
 */
#ifndef HDC_HOST_COMPAT_PARAMETER_H
#define HDC_HOST_COMPAT_PARAMETER_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int GetParameter(const char *key, const char *def,
    char *value, uint32_t len)
{
    (void)key;
    if (value == NULL || len == 0) {
        return -1;
    }
    const char *d = def ? def : "";
    size_t n = strlen(d);
    if (n >= len) {
        n = len - 1;
    }
    memcpy(value, d, n);
    value[n] = '\0';
    return (int)n;
}

static inline int SetParameter(const char *key, const char *value)
{
    (void)key;
    (void)value;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* HDC_HOST_COMPAT_PARAMETER_H */
