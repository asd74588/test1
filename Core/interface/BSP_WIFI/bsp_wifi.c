#include "bsp_wifi.h"
#include <string.h>

static wifi_status_t s_wifi_status = WIFI_STATUS_DISCONNECTED;

int wifi_init(void)
{
    s_wifi_status = WIFI_STATUS_DISCONNECTED;
    return WIFI_ERROR;
}

int wifi_connect_ap(const char *ssid, const char *password)
{
    (void)ssid;
    (void)password;
    s_wifi_status = WIFI_STATUS_DISCONNECTED;
    return WIFI_ERROR;
}

wifi_status_t wifi_get_status(void)
{
    return s_wifi_status;
}

int wifi_get_ip(char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0U)
    {
        return WIFI_ERROR;
    }

    strncpy(buf, "0.0.0.0", buf_size - 1U);
    buf[buf_size - 1U] = '\0';
    return WIFI_ERROR;
}
