/*
 * Firmware_Concentrador.cpp — Gateway LoRa a HTTP (ESP-IDF)
 *
 * Características Modificadas:
 * - Rediseño del Dashboard OLED para mostrar diagnósticos persistentes de Wi-Fi y LoRa.
 * - Monitoreo en tiempo real de Calidad de Enlace (RSSI / SNR), Voltaje y Canal activo.
 * - Solución definitiva al problema de sobreescritura de estados entre tareas.
 */

//=============================================================================
// 1) INCLUDES
//=============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <inttypes.h>
#include <string.h>

#include <RadioLib.h>
#include <u8g2.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_rom_sys.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_random.h"       

#include "driver/gpio.h"
#include "driver/spi_master.h" 
#include "driver/i2c_master.h"

#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "lwip/inet.h"

//=============================================================================
// 2) CONFIGURACIÓN
//=============================================================================
#define LORA_MISO                11
#define LORA_MOSI                10
#define LORA_SCK                 12
#define LORA_NSS                 7
#define LORA_RST                 9
#define LORA_DIO0                8

#define OLED_SCL                 47  
#define OLED_SDA                 48  
#define OLED_I2C_ADDR            0x3C

#define WIFI_SSID                "FIUNA"
#define WIFI_PASS                "fiuna#2024"
#define WIFI_PS_NONE_FORCE       1

#define DB_POST_URL              "http://200.10.231.202/en/insertarDatosv3.php"
#define DB_AUTH_HEADER           NULL
#define DB_API_KEY               "estacion_guille"

#define LOG_MOUNT_POINT          "/spiflash"
#define UNSENT_TX_FILE_PATH      LOG_MOUNT_POINT "/unsent_tx.csv"

#define TASK_STACK_DEFAULT       4096
#define TASK_PRIO_WIFI_SUP       4
#define TASK_PRIO_LORA_RX        5
#define TASK_PRIO_NET_TX         4
#define NET_QUEUE_DEPTH          48

//=============================================================================
// 3) TIPOS Y CONSTANTES LOCALES
//=============================================================================
#pragma pack(push, 1)
typedef struct {
    uint8_t  node_id;   
    uint32_t seq_min;   
    uint8_t  ch;
    float    Energy;        
    float    Vrms;      
    float    Irms;      
    float    Pavg;      
    float    PF;        
} LoRaPayload;          
#pragma pack(pop)

static struct {
    uint8_t  last_node;
    uint32_t last_seq;
    float    last_energy;
    float    last_pavg;
    float    last_vrms;
    uint8_t  last_ch;
    float    last_rssi;
    float    last_snr;
    int      total_packets;
    char     last_rx_time[20];
    char     last_net_status[24]; 
} g_display_stats = {0, 0, 0.0f, 0.0f, 0.0f, 0, 0.0f, 0.0f, 0, "--:--:--", "Inicializando"};

//=============================================================================
// 4) ESTADO GLOBAL
//=============================================================================
static QueueHandle_t q_net = NULL;
static SemaphoreHandle_t s_unsent_mutex = NULL;
static SemaphoreHandle_t s_oled_mutex = NULL;

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static volatile bool s_time_synced = false;
static volatile bool g_wifi_connected = false;

EspHal hal(LORA_SCK, LORA_MISO, LORA_MOSI);
Module mod(&hal, LORA_NSS, LORA_DIO0, LORA_RST, RADIOLIB_NC);
SX1278 radio(&mod);

static u8g2_t u8g2;
static i2c_master_dev_handle_t oled_i2c_dev = NULL;

//=============================================================================
// 5) PANTALLA OLED SH1106 (HARDWARE I2C NATIVO REDISEÑADO)
//=============================================================================
uint8_t u8x8_byte_espidf_v6_i2c(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    static uint8_t buffer[512];
    static uint16_t buf_idx = 0;

    switch(msg) {
        case U8X8_MSG_BYTE_INIT: break;
        case U8X8_MSG_BYTE_START_TRANSFER: buf_idx = 0; break;
        case U8X8_MSG_BYTE_SEND: {
            uint8_t *data = (uint8_t *)arg_ptr;
            if (buf_idx + arg_int <= sizeof(buffer)) {
                memcpy(&buffer[buf_idx], data, arg_int);
                buf_idx += arg_int;
            }
            break;
        }
        case U8X8_MSG_BYTE_END_TRANSFER:
            if (buf_idx > 0 && oled_i2c_dev != NULL) {
                i2c_master_transmit(oled_i2c_dev, buffer, buf_idx, 100);
            }
            break;
        default: return 0;
    }
    return 1;
}

uint8_t u8x8_gpio_and_delay_espidf(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    switch (msg) {
        case U8X8_MSG_DELAY_MILLI: vTaskDelay(pdMS_TO_TICKS(arg_int)); break;
        case U8X8_MSG_DELAY_10MICRO: esp_rom_delay_us(10); break;
        case U8X8_MSG_DELAY_100NANO: esp_rom_delay_us(1); break;
        default: break;
    }
    return 1;
}

static void oled_update_ui(const char* net_status) {
    if (xSemaphoreTake(s_oled_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    if (net_status != NULL) {
        snprintf(g_display_stats.last_net_status, sizeof(g_display_stats.last_net_status), "%s", net_status);
    }

    u8g2_ClearBuffer(&u8g2);
    
    // Fila 1: Título fijo
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
    u8g2_DrawStr(&u8g2, 0, 10, "LoRa ---> Wi-Fi");
    u8g2_DrawHLine(&u8g2, 0, 12, 128);

    u8g2_SetFont(&u8g2, u8g2_font_5x7_tf);
    char buf[64];

    // --- Obtener Hora Sincronizada por SNTP ---
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char time_str[20];

    // Verificar si el reloj ya fue sincronizado (año > 2020)
    if (s_time_synced && timeinfo.tm_year > (2020 - 1900)) {
        // Formato: DD/MM HH:MM:SS (Ejemplo: 21/07 14:21:05)
        strftime(time_str, sizeof(time_str), "%d/%m %H:%M:%S", &timeinfo);
    } else {
        snprintf(time_str, sizeof(time_str), "Sin Sync Hora");
    }

    // Fila 2: Muestra el estado del WiFi y la hora en que LLEGÓ el último paquete
    snprintf(buf, sizeof(buf), "%s | Rx: %s", 
            g_wifi_connected ? "WiFi OK" : "NO WiFi", g_display_stats.last_rx_time);
    u8g2_DrawStr(&u8g2, 0, 22, buf);

    // Fila 3: ESTADO LORA (RSSI / SNR)
    if (g_display_stats.total_packets > 0) {
        snprintf(buf, sizeof(buf), "Rx: RSSI:%.1fdBm SNR:%.1f", 
                 g_display_stats.last_rssi, g_display_stats.last_snr);
    } else {
        snprintf(buf, sizeof(buf), "LoRa: Escuchando aire...");
    }
    u8g2_DrawStr(&u8g2, 0, 32, buf);

    // Fila 4: Origen de Datos (Nodo, Canal, Secuencia)
    if (g_display_stats.total_packets > 0) {
        snprintf(buf, sizeof(buf), "Nd: 0x%02X | CH: %u | Seq: %u", 
                 g_display_stats.last_node, g_display_stats.last_ch, (unsigned int)g_display_stats.last_seq);
    } else {
        snprintf(buf, sizeof(buf), "Nd: -- | CH: -- | Seq: --");
    }
    u8g2_DrawStr(&u8g2, 0, 43, buf);

    // Fila 5: Telemetría (Vrms removido, espacio maximizado para Energía)
    if (g_display_stats.total_packets > 0) {
        snprintf(buf, sizeof(buf), "Energia: %.2f Wh", g_display_stats.last_energy);
    } else {
        snprintf(buf, sizeof(buf), "Energia: 0.00 Wh");
    }
    u8g2_DrawStr(&u8g2, 0, 54, buf);

    // Fila 6: Contador Acumulativo Total
    snprintf(buf, sizeof(buf), "Total Paquetes Rx: %d", g_display_stats.total_packets);
    u8g2_DrawStr(&u8g2, 0, 64, buf);

    u8g2_SendBuffer(&u8g2);
    xSemaphoreGive(s_oled_mutex);
}

static void init_oled_display(void) {
    s_oled_mutex = xSemaphoreCreateMutex();
    configASSERT(s_oled_mutex);

    ESP_LOGI("OLED", "Inicializando SH1106 via Hardware I2C Nativo...");
    
    i2c_master_bus_config_t i2c_mst_config = {}; 
    i2c_mst_config.i2c_port = I2C_NUM_0;
    i2c_mst_config.sda_io_num = (gpio_num_t)OLED_SDA;
    i2c_mst_config.scl_io_num = (gpio_num_t)OLED_SCL;
    i2c_mst_config.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_mst_config.glitch_ignore_cnt = 7;
    i2c_mst_config.flags.enable_internal_pullup = true; 

    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_config, &bus_handle));

    i2c_device_config_t dev_config = {}; 
    dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_config.device_address = OLED_I2C_ADDR;
    dev_config.scl_speed_hz = 100000; 

    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_config, &oled_i2c_dev));
    
    u8g2_Setup_sh1106_i2c_128x64_noname_f(&u8g2, U8G2_R0, u8x8_byte_espidf_v6_i2c, u8x8_gpio_and_delay_espidf);
    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0); 
    u8g2_ClearBuffer(&u8g2);
    oled_update_ui("Iniciando...");
}

//=============================================================================
// 6) UTILIDADES BÁSICAS Y TIEMPO
//=============================================================================
static inline uint32_t rand_between(uint32_t a_ms, uint32_t b_ms){
    if (b_ms <= a_ms) return a_ms;
    return a_ms + (uint32_t)(esp_random() % (b_ms - a_ms + 1));
}

static void iso8601_local(time_t t, char *buf, size_t len) {
    struct tm lt; localtime_r(&t, &lt);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%S%z", &lt);
}

//=============================================================================
// 7) WIFI Y SNTP
//=============================================================================
static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            g_wifi_connected = false;
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            oled_update_ui("Desconectado");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        g_wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
        // --- RE-SINCRONIZACIÓN AUTOMÁTICA AL VOLVER EL WIFI ---
        // Forzamos la consulta al servidor NTP usando la API moderna de esp_netif_sntp
        esp_netif_sntp_start();
        ESP_LOGI("SNTP", "WiFi restablecido: Solicitando sincronización de hora...");
        
        oled_update_ui("Conectado");
    }
}

static void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wc = {};
    strcpy((char*)wc.sta.ssid, WIFI_SSID);
    strcpy((char*)wc.sta.password, WIFI_PASS);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
#if WIFI_PS_NONE_FORCE
    esp_wifi_set_ps(WIFI_PS_NONE);
#endif
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void time_sync_cb(struct timeval *tv) { 
    s_time_synced = true; 
    oled_update_ui("Hora Sync");
}

static void init_sntp_and_tz(void) {
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = time_sync_cb;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&cfg));
    setenv("TZ", "<-03>3", 1); 
    tzset();
}

static void wifi_supervisor_task(void *arg) {
    (void)arg;
    TickType_t backoff = pdMS_TO_TICKS(5000);
    for (;;) {
        if (g_wifi_connected) {
            backoff = pdMS_TO_TICKS(5000);
            vTaskDelay(pdMS_TO_TICKS(15000));
            continue;
        }
        ESP_LOGW("WIFI_SUP", "Intentando reconectar...");
        oled_update_ui("Buscando WiFi");
        esp_wifi_connect();
        vTaskDelay(backoff);
        if (backoff < pdMS_TO_TICKS(60000)) backoff *= 2;
    }
}

//=============================================================================
// 8) GESTIÓN DE ARCHIVOS Y ENVÍO HTTP
//=============================================================================
static void fs_init_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = LOG_MOUNT_POINT,
        .partition_label = NULL,
        .max_files = 4,
        .format_if_mount_failed = true
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
    
    FILE *ftx = fopen(UNSENT_TX_FILE_PATH, "a+");
    if (ftx) fclose(ftx);
}

static esp_err_t net_post_payload(const char* payload, size_t len) {
    esp_http_client_config_t cfg = {};
    cfg.url = DB_POST_URL;
    cfg.method = HTTP_METHOD_POST;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;

    // Cabeceras exigidas por el script de inserción
    esp_http_client_set_header(c, "X-API-Key", DB_API_KEY);
    esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(c, payload, len);

    esp_err_t err = esp_http_client_perform(c);
    if (err == ESP_OK && esp_http_client_get_status_code(c) >= 300) {
        err = ESP_FAIL;
    }
    esp_http_client_cleanup(c);
    return err;
}

static void unsent_append_line(const char* line) {
    xSemaphoreTake(s_unsent_mutex, portMAX_DELAY);
    FILE *f = fopen(UNSENT_TX_FILE_PATH, "a");
    if (f) {
        fputs(line, f); fputc('\n', f);
        fclose(f);
    }
    xSemaphoreGive(s_unsent_mutex);
}

static bool net_try_flush_unsent(void) {
    if (!g_wifi_connected) return false;
    xSemaphoreTake(s_unsent_mutex, portMAX_DELAY);

    FILE *fin = fopen(UNSENT_TX_FILE_PATH, "r");
    if (!fin) { xSemaphoreGive(s_unsent_mutex); return true; }

    char tmp_path[128]; snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", UNSENT_TX_FILE_PATH);
    FILE *ftmp = fopen(tmp_path, "w");
    if (!ftmp) { fclose(fin); xSemaphoreGive(s_unsent_mutex); return false; }

    char line[256]; bool any_failure = false; int sent = 0;
    while (fgets(line, sizeof(line), fin)) {
        size_t n = strnlen(line, sizeof(line));
        if (n <= 1) continue;

        if (!any_failure && net_post_payload(line, n) == ESP_OK) {
            sent++;
            vTaskDelay(pdMS_TO_TICKS(rand_between(50, 150))); 
            if (sent >= 20) any_failure = true; 
        } else {
            fputs(line, ftmp);
            any_failure = true;
        }
    }

    fclose(fin); fclose(ftmp);
    unlink(UNSENT_TX_FILE_PATH);
    rename(tmp_path, UNSENT_TX_FILE_PATH);
    xSemaphoreGive(s_unsent_mutex);
    return !any_failure;
}

static void net_tx_task(void *arg) {
    (void)arg;
    char line[256];

    for (;;) {
        if (g_wifi_connected) net_try_flush_unsent();

        LoRaPayload r;
        if (!xQueueReceive(q_net, &r, pdMS_TO_TICKS(2000))) continue;

        // Obtener el Timestamp Unix actual en segundos
        time_t now;
        time(&now);

        // tramar=timestamp,ch,Energy,Vrms,Irms,Pavg,PF
        int n = snprintf(line, sizeof(line),
            "tramar=%ld,%u,%.6f,%.2f,%.2f,%.2f,%.3f",
            (long)now, r.ch, r.Energy, r.Vrms, r.Irms, r.Pavg, r.PF);

        if (!g_wifi_connected) {
            unsent_append_line(line);
            oled_update_ui("HTTP: SPIFFS");
            continue;
        }

        if (net_post_payload(line, n) == ESP_OK) {
            ESP_LOGI("NET", "Enviado OK a la BD: Node %u Ch %u", r.node_id, r.ch);
            oled_update_ui("HTTP: POST OK");
        } else {
            unsent_append_line(line);
            oled_update_ui("HTTP: ERR->CSV");
        }
    }
}

//=============================================================================
// 9) TAREA DE RECEPCIÓN LORA (MÉTRICAS INTEGRADAS)
//=============================================================================
static void lora_rx_task(void *arg) {
    (void)arg;
    ESP_LOGI("LORA_RX", "Inicializando módulo LoRa SX1278 (Ra-02)...");
    
    int state = radio.begin(433.0, 125.0, 7);
    if (state == RADIOLIB_ERR_NONE) {
        ESP_LOGI("LORA_RX", "LoRa OK. Escuchando paquetes...");
        radio.setSyncWord(0x55); 
        oled_update_ui("LoRa Listo");
    } else {
        ESP_LOGE("LORA_RX", "Fallo LoRa: %d", state);
        oled_update_ui("LoRa CRITIC");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        LoRaPayload packet;
        state = radio.receive((uint8_t*)&packet, sizeof(LoRaPayload));
        
        if (state == RADIOLIB_ERR_NONE) {
            if (packet.node_id < 5) {
                ESP_LOGI("LORA_RX", "Rx Válido - Nodo: %u, Ch: %u, Vrms: %.2f", 
                         packet.node_id, packet.ch, packet.Vrms);

                time_t now;
                time(&now);
                struct tm timeinfo;
                localtime_r(&now, &timeinfo);
                strftime(g_display_stats.last_rx_time, sizeof(g_display_stats.last_rx_time), "%H:%M:%S", &timeinfo);
                         
                // CAMBIO CRÍTICO: Capturamos las variables de telemetría y RF al instante
                g_display_stats.last_node = packet.node_id;
                g_display_stats.last_seq = packet.seq_min;
                g_display_stats.last_energy = packet.Energy;
                g_display_stats.last_pavg = packet.Pavg;
                g_display_stats.last_vrms = packet.Vrms;
                g_display_stats.last_ch = packet.ch;
                g_display_stats.last_rssi = radio.getRSSI(); // Lee RSSI físico del paquete
                g_display_stats.last_snr  = radio.getSNR();  // Lee SNR físico del paquete
                g_display_stats.total_packets++;
                
                // Forzar refresco inmediato reflejando que llegó por aire
                oled_update_ui("LoRa: Rx OK");
                
                xQueueSend(q_net, &packet, 0);
            } else {
                ESP_LOGW("LORA_RX", "Bloqueado ID no autorizado: %u", packet.node_id);
                oled_update_ui("ID Intruso");
            }
        } else if (state != RADIOLIB_ERR_RX_TIMEOUT) {
            ESP_LOGE("LORA_RX", "Error de recepción: %d", state);
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

//=============================================================================
// 10) APP MAIN
//=============================================================================
extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    init_oled_display();
    fs_init_spiffs();
    wifi_init_sta();
    init_sntp_and_tz();

    q_net = xQueueCreate(NET_QUEUE_DEPTH, sizeof(LoRaPayload)); configASSERT(q_net);
    s_unsent_mutex = xSemaphoreCreateMutex();                   configASSERT(s_unsent_mutex);

    xTaskCreatePinnedToCore(wifi_supervisor_task, "wifi_sup", TASK_STACK_DEFAULT, NULL, TASK_PRIO_WIFI_SUP, NULL, 0);
    xTaskCreatePinnedToCore(net_tx_task,          "net_tx",   6144,               NULL, TASK_PRIO_NET_TX,   NULL, 0);
    xTaskCreatePinnedToCore(lora_rx_task,         "lora_rx",  TASK_STACK_DEFAULT, NULL, TASK_PRIO_LORA_RX,  NULL, 1);
}