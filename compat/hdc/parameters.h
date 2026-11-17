/*
 * Minimal host-side stand-in for OHOS <parameters.h> (C++ wrapper over the
 * system parameter service). No service on a build machine: every overload
 * returns the caller-supplied default. That makes hdc's opt-in reporting /
 * enterprise-validation switches read as disabled — exactly what we want.
 */
#ifndef HDC_HOST_COMPAT_PARAMETERS_H
#define HDC_HOST_COMPAT_PARAMETERS_H

#include <string>

namespace OHOS {
namespace system {

inline std::string GetParameter(const std::string &key, const char *def)
{
    (void)key;
    return std::string(def ? def : "");
}

inline std::string GetParameter(const std::string &key, const std::string &def)
{
    (void)key;
    return def;
}

inline bool GetBoolParameter(const std::string &key, bool def)
{
    (void)key;
    return def;
}

inline int GetIntParameter(const std::string &key, int def)
{
    (void)key;
    return def;
}

} // namespace system
} // namespace OHOS

#endif /* HDC_HOST_COMPAT_PARAMETERS_H */
