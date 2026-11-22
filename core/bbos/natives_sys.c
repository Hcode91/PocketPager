/* PocketPager - BlackBerry OS family: device identity and system services
 * for the emulated handheld (a BlackBerry Bold 9000 on OS 5.0). */
#include "natives.h"

#define HW_ID        0x84000e07u   /* Bold 9000 */
#define SCREEN_W     480
#define SCREEN_H     320
#define LOCALE_EN_GB 0x656e4742u
#define LOCALE_EN    0x656e0000u

NAT(k_zero) { UNUSED; ret[0] = 0; return NAT_OK; }
NAT(k_one) { UNUSED; ret[0] = 1; return NAT_OK; }
NAT(k_nop) { UNUSED; return NAT_OK; }
NAT(k_hwid) { UNUSED; ret[0] = HW_ID; return NAT_OK; }
NAT(k_localeGB) { UNUSED; ret[0] = LOCALE_EN_GB; return NAT_OK; }
NAT(k_width) { UNUSED; ret[0] = SCREEN_W; return NAT_OK; }
NAT(k_height) { UNUSED; ret[0] = SCREEN_H; return NAT_OK; }
NAT(k_colors) { UNUSED; ret[0] = 65536; return NAT_OK; }
NAT(k_ppm) { UNUSED; ret[0] = 6417; return NAT_OK; }   /* ~163 dpi */
NAT(k_battery) { UNUSED; ret[0] = 100; return NAT_OK; }
NAT(k_flash) { UNUSED; ret[0] = 1u << 30; return NAT_OK; }
NAT(k_deviceId) { UNUSED; ret[0] = 0x2a9000b8u; return NAT_OK; }
NAT(k_platformVersion) { UNUSED; ret[0] = vm_new_string(vm, "5.2.0.64"); return NAT_OK; }
NAT(k_manufacturer) { UNUSED; ret[0] = vm_new_string(vm, "Research In Motion"); return NAT_OK; }
NAT(k_idleTime) { UNUSED; nat_ret64(ret, 0); return NAT_OK; }
NAT(k_zero64) { UNUSED; nat_ret64(ret, 0); return NAT_OK; }
NAT(k_null) { UNUSED; ret[0] = 0; return NAT_OK; }

const NatEnt kNativesSys[] = {
    { "net.rim.device.internal.system.InternalServices.getHardwareID", k_hwid },
    { "net.rim.device.internal.system.InternalServices.getFormFactor", k_zero },
    { "net.rim.device.internal.system.InternalServices.getSafeBootMode", k_zero },
    { "net.rim.device.internal.system.InternalServices.isDeviceSecure", k_zero },
    { "net.rim.device.internal.system.InternalServices.getNetworkTimeOffset", k_zero64 },
    { "net.rim.device.internal.i18n.LocaleInternal.getDefaultLanguageLocale", k_localeGB },
    { "net.rim.device.internal.i18n.LocaleInternal.getDefaultInputLanguageLocale", k_localeGB },
    { "net.rim.device.internal.i18n.LocaleInternal.getDefaultInputVariantLocale", k_localeGB },
    { "net.rim.device.api.ui.Displays.getNumDisplays", k_one },
    { "net.rim.device.api.system.Display.getDisplayId", k_zero },
    { "net.rim.device.api.ui.DisplayInstance.getWidth0", k_width },
    { "net.rim.device.api.ui.DisplayInstance.getHeight0", k_height },
    { "net.rim.device.api.ui.DisplayInstance.getNumColorsImpl", k_colors },
    { "net.rim.device.api.ui.DisplayInstance.getHorizontalResolution0", k_ppm },
    { "net.rim.device.api.ui.DisplayInstance.getVerticalResolution0", k_ppm },
    { "net.rim.device.api.ui.DisplayInstance.isRowwiseImpl", k_one },
    { "net.rim.device.api.ui.DisplayInstance.queryOrientation", k_zero },
    { "net.rim.device.api.system.DeviceInfo.getDeviceId", k_deviceId },
    { "net.rim.device.api.system.DeviceInfo.getBatteryLevel", k_battery },
    { "net.rim.device.api.system.DeviceInfo.getPlatformVersion", k_platformVersion },
    { "net.rim.device.api.system.DeviceInfo.getManufacturerName", k_manufacturer },
    { "net.rim.device.api.system.DeviceInfo.getTotalFlashSize", k_flash },
    { "net.rim.device.api.system.DeviceInfo.getIdleTime", k_idleTime },
    { "net.rim.device.api.system.DeviceInfo.isSimulator", k_zero },
    { "net.rim.device.api.system.DeviceInfo.isInProduction", k_one },
    { "net.rim.device.api.system.Branding.getVendorId0", k_zero },
    { "net.rim.device.internal.system.SensorInternal.isSensorPresent", k_zero },
    { NULL, NULL }
};
