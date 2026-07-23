#include "transport.h"

#include "esp8266.h"

#define TRANSPORT_DRAIN_BUF_SIZE 32U

int transport_open(char *host, int port)
{
    transport_clearBuf();
    return esp8266_sock_connect(host, port) ? -1 : 0;
}

int transport_close(void)
{
    int rv;

    rv = esp8266_sock_disconnect();
    transport_clearBuf();
    return rv;
}

int transport_sendPacketBuffer(unsigned char *buf, int buflen)
{
    return esp8266_sock_send(buf, buflen);
}

int transport_getdata(unsigned char *buf, int count)
{
    if (buf == NULL || count <= 0)
    {
        return -1;
    }

    return esp8266_sock_recv(buf, count);
}

void transport_clearBuf(void)
{
    unsigned char drain_buf[TRANSPORT_DRAIN_BUF_SIZE];

    while (esp8266_sock_recv(drain_buf, (int)sizeof(drain_buf)) > 0)
    {
    }
}
