#pragma once

// Build-time branding — override via CMake cache variables (-DBRAND_*)
// Set in sdkconfig.defaults or pass to idf.py build

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
// Define via -DWIFI_SSID=\"...\" -DWIFI_PASSWORD=\"...\" in CMake cache or sdkconfig
#ifdef WIFI_SSID
  #ifndef WIFI_PASSWORD
    #error "WIFI_PASSWORD must be defined when WIFI_SSID is set"
  #endif
#endif
