#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"

#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "esp_event_loop.h"
#include "esp_http_server.h"
#include "config.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "lwip/udp.h"


#include "lwip/init.h"
#include "lwip/timeouts.h"

#include "netif/etharp.h"
#include "rtsp.h"
#include <signal.h>

//IP_REASSEMBLY=y
//IP_FRAG=y
static const char* TAG = "camera";
#define CAM_USE_WIFI

#if 0
#define ESP_WIFI_SSID "M5Psram_Cam"
#define ESP_WIFI_PASS ""
#else

#define ESP_WIFI_SSID "Sanby"
#define ESP_WIFI_PASS "1234567890"

#endif

#define MAX_STA_CONN  1

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static EventGroupHandle_t s_wifi_event_group;
static ip4_addr_t s_ip_addr;
const int CONNECTED_BIT = BIT0;
extern void led_brightness(int duty);
static camera_config_t camera_config = {
    .pin_reset = -1,
    .pin_xclk = 0,
    .pin_sscb_sda = 26,
    .pin_sscb_scl = 27,
    .pin_d7 = 35,
    .pin_d6 = 34,
    .pin_d5 = 39,
    .pin_d4 = 36,
    .pin_d3 = 21,
    .pin_d2 = 19,
    .pin_d1 = 18,
    .pin_d0 = 5,
    .pin_vsync = 25,
    .pin_href = 23,
    .pin_pclk = 22,
    .pin_pwdn = 32,
    //XCLK 20MHz or 10MHz
    .xclk_freq_hz = CAM_XCLK_FREQ,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,//YUV422,GRAYSCALE,RGB565,JPEG
    .frame_size = FRAMESIZE_HVGA,//QQVGA-UXGA Do not use sizes above QVGA when not JPEG
    .jpeg_quality = 20, //0-63 lower number means higher quality
    .fb_count = 1 //if more than one, i2s runs in continuous mode. Use only with JPEG
};

static void wifi_init_softap();
static esp_err_t http_server_init();


const ip_addr_t client_addr;
u16_t client_port=0;
struct udp_pcb* client_pcb;
static void udp_test_recv(void *arg,struct udp_pcb *upcb,struct pbuf *p,const ip_addr_t *addr,u16_t port)
{
    memcpy((void *)&client_addr,(void *)addr,sizeof(ip_addr_t));
    client_port=port;
    ESP_LOGI(TAG,"addr->type=%08x",addr->u_addr.ip4.addr);
    ESP_LOGI(TAG,"Received UDP Packet from ip_addr %s,%d",  inet_ntoa(addr->u_addr.ip4),port);
    LWIP_UNUSED_ARG(arg);
    if (p != NULL) {
            pbuf_free(p);
           /* p=pbuf_alloc(PBUF_TRANSPORT, 3000, PBUF_RAM);
            printf("UDP send length:%d\r\n",p->len);
            err_t code = udp_sendto(upcb, p, addr, port); //send it back to port 5555
            printf("Echo'd packet, result code is %d\r\n",code);
            pbuf_free(p);*/
    }
}
void udp_test_send(struct udp_pcb *upcb,const char *buf,int length)
{
    struct pbuf *p;
	if(length>25000)length=25000;
    p=pbuf_alloc_reference((void *)buf,length, PBUF_ROM);
    err_t code = udp_sendto(upcb, p, &client_addr, client_port); //send it back to port 5555
    ESP_LOGI(TAG,"Echo'd packet, result code is %d\r\n",code);
    pbuf_free(p);
}
static void udp_server_init(void *pvParameters){
    client_pcb = udp_new();
    udp_bind(client_pcb, IP_ADDR_ANY, 554); 
    udp_recv(client_pcb,udp_test_recv, NULL);
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len;
    uint8_t * _jpg_buf;
    static int64_t last_frame = 0;
    if(!last_frame) {
        last_frame = esp_timer_get_time();
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    while(true){
        if(client_port)
        {
            fb = esp_camera_fb_get();
            if (!fb) {
                ESP_LOGE(TAG, "Camera capture failed");
                res = ESP_FAIL;
            } else {
                if(fb->format != PIXFORMAT_JPEG){
                    bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
                    if(!jpeg_converted){
                        ESP_LOGE(TAG, "JPEG compression failed");
                        esp_camera_fb_return(fb);
                        res = ESP_FAIL;
                    }
                } else {
                    _jpg_buf_len = fb->len;
                    _jpg_buf = fb->buf;
                }
            }
            udp_test_send(client_pcb,(const char *)_jpg_buf,_jpg_buf_len);
            if(fb->format != PIXFORMAT_JPEG){
                free(_jpg_buf);
            }
            esp_camera_fb_return(fb);
            if(res != ESP_OK){
                break;
            }
            int64_t fr_end = esp_timer_get_time();
            int64_t frame_time = fr_end - last_frame;
            last_frame = fr_end;
            frame_time /= 1000;
            ESP_LOGI(TAG, "MJPG: %uKB %ums (%.1ffps)",
                (uint32_t)(_jpg_buf_len/1024),
                (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time);
        }else 
        {
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
    }
    last_frame = 0;
}
static void tcp_client_task(void *pvParameters)
{
    int tcpsock=(int)pvParameters;
    uint8_t rx_buffer[512] = { 0 };
    uint8_t ret_buffer[1024] = { 0 };
    ESP_LOGI(TAG, "pvParameters=%d",tcpsock);
    while(true)
    {
        int len = recv(tcpsock, rx_buffer, sizeof(rx_buffer) - 1, 0);
		if (len > 0) {
			rx_buffer[len] = 0;
			printf("Received %d bytes from:%s\n", len, rx_buffer);
            memset(ret_buffer, 0x00, sizeof(ret_buffer));
            switch_method((char *)rx_buffer,(char *)ret_buffer);
            send(tcpsock, ret_buffer,strlen((char *)ret_buffer), 0);
		} else {
			close(tcpsock);
            break;
		}
        vTaskDelay(100);
        ESP_LOGI(TAG, "pvParameters=%d",(int)pvParameters);
    }
    ESP_LOGI(TAG, "Client Disconnect");
    vTaskDelete(NULL);
}
void tcp_server_init(){
    int addr_family;
    int ip_protocol;
    addr_family 			 = AF_INET;
    ip_protocol 			 = IPPROTO_IP;
    struct sockaddr_in localAddr;
    localAddr.sin_addr.s_addr 	= htonl(INADDR_ANY);
    localAddr.sin_family		= AF_INET;
    localAddr.sin_port			=htons(8080);
    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) 
    {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return;
	}
    int err = bind(listen_sock, (struct sockaddr *)&localAddr, sizeof(localAddr));
    if (err < 0) 
    {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
    }
    ESP_LOGI(TAG, "Socket created");
    err = listen(listen_sock,0);
    if(err != 0)
    {
        ESP_LOGI(TAG,"Socket unable to connect: errno %d", errno);
    }
    ESP_LOGI(TAG,"Socket is listening");
    struct sockaddr_in  sourceAddr;
    uint addrLen = sizeof(sourceAddr);
    while(true)
    {
        int sock = accept(listen_sock, (struct sockaddr *)&sourceAddr, &addrLen);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            return;
        }
        ESP_LOGI(TAG, "Welcome %s:%d here!", inet_ntoa(sourceAddr.sin_addr.s_addr), sourceAddr.sin_port);
        ESP_LOGI(TAG, "Socket accepted sock is %d",sock);
        portBASE_TYPE res1 = xTaskCreate(tcp_client_task, "taskName",4048, (void *)sock,7, NULL);
        ESP_LOGI(TAG, "xTaskCreate res is %d",res1);
    }
}


void app_main()
{
    printf("Camera capture start\n");
    esp_log_level_set("wifi", ESP_LOG_INFO);
    
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ESP_ERROR_CHECK( nvs_flash_init() );
    }

    err = esp_camera_init(&camera_config);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera Init Failed");
        for(;;) {
            vTaskDelay(10);
        }
    } else {
        led_brightness(20);
    }

#ifdef FISH_EYE_CAM
    // flip img, other cam setting view sensor.h
    sensor_t *s = esp_camera_sensor_get();
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);
#endif

#ifdef CAM_USE_WIFI
    wifi_init_softap();

    vTaskDelay(100 / portTICK_PERIOD_MS);
    //http_server_init();
    xTaskCreate(udp_server_init, "UDPTask",4048, (void *)0,7, NULL);
    //tcp_server_init();
    //http_server_init();
    
#endif
}

#ifdef CAM_USE_WIFI

esp_err_t jpg_httpd_handler(httpd_req_t *req){
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t fb_len = 0;
    int64_t fr_start = esp_timer_get_time();

    fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    res = httpd_resp_set_type(req, "image/jpeg");
    if(res == ESP_OK){
        res = httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    }

    if(res == ESP_OK){
        fb_len = fb->len;
        res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    }
    esp_camera_fb_return(fb);
    int64_t fr_end = esp_timer_get_time();
    ESP_LOGI(TAG, "JPG: %uKB %ums", (uint32_t)(fb_len/1024), (uint32_t)((fr_end - fr_start)/1000));
    return res;
}
esp_err_t jpg_stream_httpd_handler(httpd_req_t *req){
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len;
    uint8_t * _jpg_buf;
    char * part_buf[64];
    static int64_t last_frame = 0;
    if(!last_frame) {
        last_frame = esp_timer_get_time();
    }

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK){
        return res;
    }

    while(true){
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
        } else {
            if(fb->format != PIXFORMAT_JPEG){
                bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
                if(!jpeg_converted){
                    ESP_LOGE(TAG, "JPEG compression failed");
                    esp_camera_fb_return(fb);
                    res = ESP_FAIL;
                }
            } else {
                _jpg_buf_len = fb->len;
                _jpg_buf = fb->buf;
            }
        }
        if(res == ESP_OK){
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);

            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
            
        }
        
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(fb->format != PIXFORMAT_JPEG){
            free(_jpg_buf);
        }
        esp_camera_fb_return(fb);
        if(res != ESP_OK){
            break;
        }
        int64_t fr_end = esp_timer_get_time();
        int64_t frame_time = fr_end - last_frame;
        last_frame = fr_end;
        frame_time /= 1000;
        
        ESP_LOGI(TAG, "MJPG: %uKB %ums (%.1ffps)",
            (uint32_t)(_jpg_buf_len/1024),
            (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time);
    }

    last_frame = 0;
    return res;
}

static esp_err_t http_server_init(){
    httpd_handle_t server;
    httpd_uri_t jpeg_uri = {
        .uri = "/jpg",
        .method = HTTP_GET,
        .handler = jpg_httpd_handler,
        .user_ctx = NULL
    };

    httpd_uri_t jpeg_stream_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = jpg_stream_httpd_handler,
        .user_ctx = NULL
    };

    httpd_config_t http_options = HTTPD_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(httpd_start(&server, &http_options));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &jpeg_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &jpeg_stream_uri));

    return ESP_OK;
}

static esp_err_t event_handler(void* ctx, system_event_t* event) 
{
  switch (event->event_id) {
    case SYSTEM_EVENT_STA_START:
      esp_wifi_connect();
      break;
    case SYSTEM_EVENT_STA_GOT_IP:
      ESP_LOGI(TAG, "got ip:%s", ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
      s_ip_addr = event->event_info.got_ip.ip_info.ip;
      xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
      break;
    case SYSTEM_EVENT_AP_STACONNECTED:
      ESP_LOGI(TAG, "station:" MACSTR " join, AID=%d", MAC2STR(event->event_info.sta_connected.mac),
               event->event_info.sta_connected.aid);
      xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
      break;
    case SYSTEM_EVENT_AP_STADISCONNECTED:
      ESP_LOGI(TAG, "station:" MACSTR "leave, AID=%d", MAC2STR(event->event_info.sta_disconnected.mac),
               event->event_info.sta_disconnected.aid);
      xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
      break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
      esp_wifi_connect();
      xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
      break;
    default:
      break;
  }
  return ESP_OK;
}

static void wifi_init_softap() 
{
  s_wifi_event_group = xEventGroupCreate();

  tcpip_adapter_init();
  ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  wifi_config_t wifi_config = {
      .sta = {
           .ssid = ESP_WIFI_SSID,
           .password = ESP_WIFI_PASS
      },
  };
  ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
}

#endif