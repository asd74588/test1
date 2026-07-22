#ifndef __BSP_WIFI_H
#define __BSP_WIFI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum
{
    WIFI_OK    = 0,
    WIFI_ERROR = -1,
} wifi_ret_t;

typedef enum
{
    WIFI_STATUS_DISCONNECTED = 0,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_GOT_IP,
} wifi_status_t;

int           wifi_init(void);
int           wifi_connect_ap(const char *ssid, const char *password);
wifi_status_t wifi_get_status(void);
int           wifi_get_ip(char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_WIFI_H */
