/*
 * OHOS-target libusb stubs: hdc's USB transport unconditionally references
 * libusb-1.0, but an on-device build has no USB-host scenario (and the OHOS
 * sysroot ships the USB NDK, not libusb-1.0). libusb_init fails, the USB
 * module disables itself, and TCP (tconn) keeps working. Only linked for
 * PLATFORM=ohos — the linux build links the real static libusb-1.0.a.
 */
#include <libusb/libusb.h>

#define STUB_LIBUSB_ERR LIBUSB_ERROR_NOT_SUPPORTED

extern "C" {

int libusb_init(libusb_context **ctx)
{
    (void)ctx;
    return STUB_LIBUSB_ERR;
}

int libusb_set_option(libusb_context *ctx, enum libusb_option option, ...)
{
    (void)ctx;
    (void)option;
    return STUB_LIBUSB_ERR;
}

void libusb_set_log_cb(libusb_context *ctx, libusb_log_cb cb, int mode)
{
    (void)ctx;
    (void)cb;
    (void)mode;
}

ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list)
{
    (void)ctx;
    (void)list;
    return STUB_LIBUSB_ERR;
}

void libusb_free_device_list(libusb_device **list, int unref_devices)
{
    (void)list;
    (void)unref_devices;
}

int libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *desc)
{
    (void)dev;
    (void)desc;
    return STUB_LIBUSB_ERR;
}

int libusb_get_active_config_descriptor(libusb_device *dev,
    struct libusb_config_descriptor **config)
{
    (void)dev;
    (void)config;
    return STUB_LIBUSB_ERR;
}

void libusb_free_config_descriptor(struct libusb_config_descriptor *config)
{
    (void)config;
}

uint8_t libusb_get_bus_number(libusb_device *dev)
{
    (void)dev;
    return 0;
}

uint8_t libusb_get_device_address(libusb_device *dev)
{
    (void)dev;
    return 0;
}

int libusb_open(libusb_device *dev, libusb_device_handle **handle)
{
    (void)dev;
    (void)handle;
    return STUB_LIBUSB_ERR;
}

void libusb_close(libusb_device_handle *handle)
{
    (void)handle;
}

int libusb_claim_interface(libusb_device_handle *handle, int interface_number)
{
    (void)handle;
    (void)interface_number;
    return STUB_LIBUSB_ERR;
}

int libusb_release_interface(libusb_device_handle *handle, int interface_number)
{
    (void)handle;
    (void)interface_number;
    return STUB_LIBUSB_ERR;
}

int libusb_set_configuration(libusb_device_handle *handle, int configuration)
{
    (void)handle;
    (void)configuration;
    return STUB_LIBUSB_ERR;
}

int libusb_kernel_driver_active(libusb_device_handle *handle, int interface_number)
{
    (void)handle;
    (void)interface_number;
    return STUB_LIBUSB_ERR;
}

int libusb_get_string_descriptor_ascii(libusb_device_handle *handle, uint8_t desc_index,
    unsigned char *data, int length)
{
    (void)handle;
    (void)desc_index;
    (void)data;
    (void)length;
    return STUB_LIBUSB_ERR;
}

int libusb_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint,
    unsigned char *data, int length, int *transferred, unsigned int timeout)
{
    (void)handle;
    (void)endpoint;
    (void)data;
    (void)length;
    (void)transferred;
    (void)timeout;
    return STUB_LIBUSB_ERR;
}

int libusb_submit_transfer(struct libusb_transfer *transfer)
{
    (void)transfer;
    return STUB_LIBUSB_ERR;
}

int libusb_cancel_transfer(struct libusb_transfer *transfer)
{
    (void)transfer;
    return STUB_LIBUSB_ERR;
}

int libusb_handle_events_timeout(libusb_context *ctx, struct timeval *tv)
{
    (void)ctx;
    (void)tv;
    return STUB_LIBUSB_ERR;
}

const char *libusb_error_name(int error_code)
{
    (void)error_code;
    return "LIBUSB_STUB (unsupported on this target)";
}

} // extern "C"
