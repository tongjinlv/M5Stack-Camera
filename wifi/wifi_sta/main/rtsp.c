#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <time.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "string.h"
#include <signal.h>
#include "tcpip_adapter.h"
#include "lwip/ip4_addr.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "lwip/udp.h"


#include "lwip/init.h"
#include "lwip/timeouts.h"

#include "netif/etharp.h"
static const char* TAG = "rtsp";

enum Method
    {
        NONE,OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER, RTCP
    };
uint32_t Session=0;
extern u16_t client_port;
static bool parse_request1(char *p,int *value)
{
    char method[20] = {0};
    char url[50] = {0};
    char version[20] = {0};
    ESP_LOGI(TAG,"parserequest1:%s",p);
    if(sscanf(p, "%s %s %s\n", method, url, version) != 3)
    {
        return false; 
    }
    ESP_LOGI(TAG,"method:%s",method);
    ESP_LOGI(TAG,"url:%s",url);
    ESP_LOGI(TAG,"version:%s",version);
    if(!strcmp(method, "OPTIONS"))
    {
        *value=OPTIONS;
    }
    if(!strcmp(method, "DESCRIBE"))
    {
        *value=DESCRIBE;
    }
    if(!strcmp(method, "SETUP"))
    {
        *value=SETUP;
    }
    if(!strcmp(method, "PLAY"))
    {
        *value=PLAY;
    }
    if(!strcmp(method, "TEARDOWN"))
    {
        *value=TEARDOWN;
    }
    if(!strcmp(method, "GET_PARAMETER"))
    {
        *value=GET_PARAMETER;
    }
    return true;
}
uint32_t parse_cseq(char *msg,int *value)
{
    char *p=strstr(msg, "CSeq:");
    if (p)
    {
        uint32_t cseq = 0;
        sscanf(p, "%*[^:]: %u", &cseq);
        *value=cseq;
        return true;
    }
    return false;
}

uint32_t parse_user_agent(char *msg,char *value)
{
    char *p=strstr(msg, "User-Agent:");
    if (p)
    {
        sscanf(p, "%*[^:]:%s", value);
        return true;
    }
    return false;
}
uint32_t parse_accept(char *msg,char *value)
{
    char *p=strstr(msg, "Accept:");
    if (p)
    {
        sscanf(p, "%*[^:]:%s", value);
        return true;
    }
    return false;
}
uint32_t parse_transport(char *msg,char *value)
{
    char *p=strstr(msg, "Transport:");
    if (p)
    {
        sscanf(p, "%*[^:]:%s", value);
        return true;
    }
    return false;
}
uint32_t parse_session(char *msg,uint32_t *value)
{
    char *p=strstr(msg, "Session:");
    if (p)
    {
        uint32_t v = 0;
        ESP_LOGI(TAG,"parse_session=%s",p);
        sscanf(p, "%*[^:]: %u", &v);
        *value=v;
        return true;
    }
    return false;
}
uint32_t parse_content_length(char *msg,uint32_t *value)
{
    char *p=strstr(msg, "Content-Length:");
    if (p)
    {
        uint32_t v = 0;
        sscanf(p, "%*[^:]: %u", &v);
        *value=v;
        return true;
    }
    return false;
}

void generate_SDP_description(char *buf)
{
    tcpip_adapter_ip_info_t info;
    memset(&info, 0x00, sizeof(tcpip_adapter_ip_info_t));
    tcpip_adapter_get_ip_info(ESP_IF_WIFI_STA, &info);
    sprintf(buf,
        "RTSP/1.0 200 OK\n"
        "CSeq: 1\n"
        "Content-Length: 372\n"
        "Content-Type: application/sdp\n"
        "\n"
        "v=0\n"
        "o=- 9%ld 1 IN IP4 %s\n"
        "t=0 0\n"
        "a=control:*\n"
        "a=type:broadcast\n"
        "m=video 0 RTP/AVP 96\n"
        "c=IN IP4 0.0.0.0\n"
        "a=rtpmap:96 JPEG/90000\n"
        "a=framerate:25\n"
        "a=control:track0\n"
        "m=audio 0 RTP/AVP 97\n"
        "c=IN IP4 0.0.0.0\n"
        "a=rtpmap:97 mpeg4-generic/44100/2\n"
        "a=fmtp:97 profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1210\n"
        "a=control:track1\n",(long)time(NULL),inet_ntoa(info.ip));
}
bool parse_client_port(char *msg,uint32_t *port1,uint32_t *port2)
{
    char value[20];
    char *p=strstr(msg, "client_port=");
    if (p)
    {
        sscanf(p, "%*[^=]=%s", value);
        sscanf(value, "%u-%u", port1,port2);
        ESP_LOGI(TAG,"port1=%s",value);
        return true;
    }
    return false;
}
bool switch_method(char *msg,char *ret)
{
    int method=NONE;
    int cseq=0;
    char user_agent[50];
    char accept[50];
    char transport[50];
    uint32_t session=0;
    uint32_t client_port1=0,client_port2=0;
    uint32_t content_length=0;
    memset(user_agent,0x00,sizeof(user_agent));
    memset(accept,0x00,sizeof(accept));
    memset(transport,0x00,sizeof(transport));
    char *p=strtok(msg, "\n");
    ESP_LOGI(TAG,"switch_method");
    parse_request1(p,&method);
    while(p)
    {  
        ESP_LOGI(TAG,"%s", p); 
        parse_cseq(p,&cseq);
        parse_user_agent(p,&user_agent[0]);
        parse_accept(p,&accept[0]);
        parse_session(p,&session);
        parse_transport(p,&transport[0]);
        parse_content_length(p,&content_length);
        parse_client_port(p,&client_port1,&client_port2);
        p = strtok(NULL, "\n");  
    }
    ESP_LOGI(TAG,"Method:%d",method);
    ESP_LOGI(TAG,"Cseq:%d",cseq);
    ESP_LOGI(TAG,"User_agent:%s",user_agent);
    ESP_LOGI(TAG,"Accept:%s",accept);
    ESP_LOGI(TAG,"Transport:%s",transport);
    ESP_LOGI(TAG,"Session:%08d",session);
    ESP_LOGI(TAG,"Content-Length:%08d",content_length);
    switch(method)
    {
        case OPTIONS:
            sprintf(ret,"RTSP/1.0 200 OK CSeq: %d\nPublic: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY\n\n",cseq);
            break;
        case DESCRIBE:
            generate_SDP_description(ret);
            break;
        case SETUP:
            Session=rand();
            sprintf(ret,"RTSP/1.0 200 OK CSeq: %d\nTransport: RTP/AVP;unicast;client_port=%d-%d;server_port=18546-18547\nSession: %08d\n\n",cseq,client_port1,client_port2,Session);
            break;
        case PLAY:
            sprintf(ret,"rtsp://10.0.75.2:8554/live  RTSP/1.0\nCSeq:%d\nUser-Agent: LibVLC/3.0.3 (LIVE555 Streaming Media v2016.11.28)\nSession: %08d\nRange: npt=0.000-\n\nnterleaved=2-3\n",cseq,Session);
            break;
        case TEARDOWN:
            sprintf(ret,"rtsp://10.0.75.2:8554/live  RTSP/1.0\nCSeq:%d\n",cseq);
            client_port=0;
            break;
    }
    return true;
}
