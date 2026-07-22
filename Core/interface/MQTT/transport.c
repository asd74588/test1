#include <string.h>

#include "transport.h"

#include "esp8266.h"
#include "main.h"

#define TRANSPORT_SOCK_BUF_SIZE   512U
#define TRANSPORT_RECV_TIMEOUT_MS 1500U

static unsigned char s_sock_buf[TRANSPORT_SOCK_BUF_SIZE];
static int           s_rx_bytes = 0;

int transport_open(char *host, int port)
{
    transport_clearBuf();
    return esp8266_sock_connect(host, port) ? -1 : 0;
}

int transport_close(void)
{
    transport_clearBuf();
    return esp8266_sock_disconnect();
}

int transport_sendPacketBuffer(unsigned char *buf, int buflen)
{
    return esp8266_sock_send(buf, buflen);
}

int transport_getdata(unsigned char *buf, int count)
{
    uint32_t start_tick;
    int      rv;

    if (buf == NULL || count <= 0)
    {
        return -1;
    }

    start_tick = HAL_GetTick();

    while (s_rx_bytes < count)
    {
        rv = esp8266_sock_recv(&s_sock_buf[s_rx_bytes],
                               (int)(sizeof(s_sock_buf) - (uint32_t)s_rx_bytes));
        if (rv < 0)
        {
            return -1;
        }

        if (rv > 0)
        {
            s_rx_bytes += rv;
            continue;
        }

        if ((HAL_GetTick() - start_tick) >= TRANSPORT_RECV_TIMEOUT_MS)
        {
            break;
        }

        HAL_Delay(10U);
    }

    if (s_rx_bytes <= 0)
    {
        return 0;
    }

    rv = (count > s_rx_bytes) ? s_rx_bytes : count;
    memcpy(buf, s_sock_buf, (uint32_t)rv);
    s_rx_bytes -= rv;

    if (s_rx_bytes > 0)
    {
        memmove(s_sock_buf, &s_sock_buf[rv], (uint32_t)s_rx_bytes);
    }

    return rv;
}

void transport_clearBuf(void)
{
    memset(s_sock_buf, 0, sizeof(s_sock_buf));
    s_rx_bytes = 0;
}
