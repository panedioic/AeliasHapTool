/*
 * Minimal host-side stand-in for OpenHarmony utils/native <singleton.h>.
 * Only what hdc's command_event_report.h needs: OHOS::DelayedSingleton<T>
 * (GetInstance() -> std::shared_ptr<T>) plus the DECLARE_DELAYED_SINGLETON
 * class macro. Instances are created with plain `new` inside the template
 * (not std::make_shared) so a private ctor + friend declaration is enough.
 */
#ifndef HDC_HOST_COMPAT_SINGLETON_H
#define HDC_HOST_COMPAT_SINGLETON_H

#include <memory>

namespace OHOS {

template<typename T>
class DelayedSingleton {
public:
    static std::shared_ptr<T> GetInstance()
    {
        static std::shared_ptr<T> instance(new T());
        return instance;
    }
};

} // namespace OHOS

#define DECLARE_DELAYED_SINGLETON(type) \
public:\
    type();\
    ~type();\
private:\
    friend class OHOS::DelayedSingleton<type>;

#endif /* HDC_HOST_COMPAT_SINGLETON_H */
