#pragma once

// Build-time branding — override via PlatformIO -D flags
// See platformio.ini [env:nanoc6-serin] for branded build example

#ifndef BRAND_NAME
#define BRAND_NAME "Mini Split"
#endif

#ifndef BRAND_AP_PREFIX
#define BRAND_AP_PREFIX "Serin"
#endif

#ifndef BRAND_AP_PASSWORD
#define BRAND_AP_PASSWORD "serinlabs"
#endif

#ifndef BRAND_MANUFACTURER
#define BRAND_MANUFACTURER "Mitsubishi Electric"
#endif

#ifndef BRAND_MODEL
#define BRAND_MODEL "CN105"
#endif

#ifndef BRAND_QR_ID
#define BRAND_QR_ID "MCAC"
#endif

#ifndef BRAND_THEME_COLOR
#define BRAND_THEME_COLOR "#f48120"
#endif

// Build-time WiFi credentials (dev convenience, no defaults)
// Define via -DWIFI_SSID=\"...\" -DWIFI_PASSWORD=\"...\" in platformio_override.ini
#ifdef WIFI_SSID
  #ifndef WIFI_PASSWORD
    #error "WIFI_PASSWORD must be defined when WIFI_SSID is set"
  #endif
#endif
