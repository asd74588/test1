#ifndef NET_CFG_H
#define NET_CFG_H

#include <stdint.h>

#define NET_CFG_WIFI_SSID_MAX     32U
#define NET_CFG_WIFI_PASSWORD_MAX 32U
#define NET_CFG_MQTT_HOST_MAX     64U
#define NET_CFG_ACCESS_TOKEN_MAX  64U

typedef struct {
    char        wifi_ssid[NET_CFG_WIFI_SSID_MAX + 1U];
    char        wifi_password[NET_CFG_WIFI_PASSWORD_MAX + 1U];
    char        mqtt_host[NET_CFG_MQTT_HOST_MAX + 1U];
    uint16_t    mqtt_port;
    char        access_token[NET_CFG_ACCESS_TOKEN_MAX + 1U];
} net_cfg_t;

void net_cfg_load_default(net_cfg_t *cfg);
int net_cfg_load(net_cfg_t *cfg);
int net_cfg_save(const net_cfg_t *cfg);
int net_cfg_reload(void);
const net_cfg_t *net_cfg_get(void);
int net_cfg_is_valid(const net_cfg_t *cfg);

#endif /* NET_CFG_H */
