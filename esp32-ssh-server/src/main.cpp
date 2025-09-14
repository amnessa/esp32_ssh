#include "wifi_manager/WifiManager.h"

// Set local WiFi credentials below.
const char *configSTASSID = "SUPERONLINE_Wi-Fi_A662";
const char *configSTAPSK = "FReSBNCQy4";

// The command line you would use to run this from a shell prompt.
//#define EX_CMD "samplesshd-kbdint", "--hostkey", "/spiffs/.ssh/id_ed25519", \
               "::"

// Stack size needed to run SSH.
const unsigned int configSTACK = 10240;

// Include Arduino core first for basic definitions
#include <Arduino.h>

#ifndef LED_BUILTIN
#define LED_BUILTIN 2 // Default LED pin for ESP32
#endif

// Then include lower-level network headers
#include <arpa/inet.h>
#include "esp_netif.h"

// Use LibSSH-ESP32 helper API for password-only server
#include <libssh_esp32.h>

volatile bool wifiPhyConnected;

// Timing and timeout configuration.
#define WIFI_TIMEOUT_S 10
#define NET_WAIT_MS 100

// Networking state of this esp32 device.
typedef enum
{
  STATE_NEW,
  STATE_PHY_CONNECTED,
  STATE_WAIT_IPADDR,
  STATE_GOT_IPADDR,
  STATE_OTA_UPDATING,
  STATE_OTA_COMPLETE,
  STATE_LISTENING,
  STATE_TCP_DISCONNECTED
} devState_t;

static volatile devState_t devState;
static volatile bool gotIpAddr, gotIp6Addr;

#include "SPIFFS.h"

WifiManager wifiManager;
// Diagnostic plain TCP server to verify inbound connectivity independent of libssh
#include <WiFi.h>
WiFiServer diagServer(8080);
static TaskHandle_t diagTaskHandle = nullptr;

// NEW: SSH password-only server task
static TaskHandle_t sshTaskHandle = nullptr;
static const char* sshUser = "esp32";
static const char* sshPass = "qwe123asd";

void diagServerTask(void *param) {
  for (;;) {
    WiFiClient c = diagServer.available();
    if (c) {
      c.println("ESP32 TCP diag OK (port 8080)\r");
      c.stop();
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

#define newDevState(s) (devState = s)

void event_cb(void *args, esp_event_base_t base, int32_t id, void* event_data)
{
  switch(id)
  {
    case WIFI_EVENT_STA_START:
      Serial.print("% WiFi enabled with SSID=");
      Serial.println(configSTASSID);
      break;
    case WIFI_EVENT_STA_CONNECTED:
      Serial.println("% WiFi connected");
      wifiPhyConnected = true;
      if (devState < STATE_PHY_CONNECTED) newDevState(STATE_PHY_CONNECTED);
      break;
    case WIFI_EVENT_STA_DISCONNECTED:
      if (devState < STATE_WAIT_IPADDR) newDevState(STATE_NEW);
      if (wifiPhyConnected)
      {
        Serial.println("% WiFi disconnected");
        wifiPhyConnected = false;
      }
      wifiManager.connect(configSTASSID, configSTAPSK);
      break;
    case IP_EVENT_GOT_IP6:
      {
        ip_event_got_ip6_t* event = (ip_event_got_ip6_t*) event_data;
        if (event->ip6_info.ip.addr[0] != htons(0xFE80) && !gotIp6Addr)
        {
          gotIp6Addr = true;
        }
        Serial.print("% IPv6 Address: ");
        #if ESP_IDF_VERSION_MAJOR >= 5
        Serial.println(IPAddress((const uint8_t*)&event->ip6_info.ip.addr));
        #else
        Serial.println(IPv6Address(event->ip6_info.ip.addr));
        #endif
      }
      break;
    case IP_EVENT_STA_GOT_IP:
      {
        #if ESP_IDF_VERSION_MAJOR >= 5
        WiFi.enableIPv6(); // Under IDF 5 we need to get IPv4 address first.
        #else
        WiFi.enableIpV6(); // Under IDF 5 we need to get IPv4 address first.
        #endif
        gotIpAddr = true;
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        Serial.print("% IPv4 Address: ");
        Serial.println(IPAddress(event->ip_info.ip.addr));
        // Start diagnostic server once
        if (!diagTaskHandle) {
          diagServer.begin();
          xTaskCreatePinnedToCore(diagServerTask, "diag", 4096, nullptr, 1, &diagTaskHandle, 0);
          Serial.println("% Diagnostic TCP server listening on port 8080");
        }

        // Start SSH server once (password-only)
        if (!sshTaskHandle) {
          auto sshServerTask = [](void*){
            if (!libssh_begin()) {
              Serial.println("[SSH] init failed");
              vTaskDelete(nullptr);
              return;
            }
            ssh_server_set_auth_password(sshUser, sshPass);
            ssh_server_begin();
            Serial.println("[SSH] Server started (password auth, default port 22)");
            for (;;) {
              if (ssh_server_has_client_command()) {
                String cmd = ssh_server_get_client_command();
                Serial.print("[SSH] cmd: ");
                Serial.println(cmd);
                // Echo back
                ssh_server_write_client(cmd.c_str());
                ssh_server_write_client("\n");
              }
              vTaskDelay(10 / portTICK_PERIOD_MS);
            }
          };
          xTaskCreatePinnedToCore(sshServerTask, "ssh", 8192, nullptr, 2, &sshTaskHandle, 0);
        }
      }
      break;
    case IP_EVENT_STA_LOST_IP:
      //gotIpAddr = false;
    default:
      break;
  }
}

void controlTask(void *pvParameter)
{
  _REENT_INIT_PTR((&reent_data_esp32));

  // Mount the file system.
  boolean fsGood = SPIFFS.begin();
  if (!fsGood)
  {
    printf("%% No formatted SPIFFS filesystem found to mount.\n");
    printf("%% Format SPIFFS and mount now (NB. may cause data loss) [y/n]?\n");
    while (!Serial.available()) {} // Waits here for input.
    char c = Serial.read();
    if (c == 'y' || c == 'Y')
    {
      printf("%% Formatting...\n");
      fsGood = SPIFFS.format();
      if (fsGood) SPIFFS.begin();
    }
  }
  if (!fsGood)
  {
    printf("%% Aborting now.\n");
    while (1) vTaskDelay(60000 / portTICK_PERIOD_MS);
  }
  printf(
    "%% Mounted SPIFFS used=%d total=%d\r\n", SPIFFS.usedBytes(),
    SPIFFS.totalBytes());

  wifiPhyConnected = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_MODE_STA);
  gotIpAddr = false; gotIp6Addr = false;
  wifiManager.connect(configSTASSID, configSTAPSK);

  TickType_t xStartTime;
  xStartTime = xTaskGetTickCount();
  const TickType_t xTicksTimeout = WIFI_TIMEOUT_S*1000/portTICK_PERIOD_MS;
  bool aborting;

  while (1)
  {
    switch (devState)
    {
      case STATE_NEW :
        vTaskDelay(NET_WAIT_MS / portTICK_PERIOD_MS);
        break;
      case STATE_PHY_CONNECTED :
        newDevState(STATE_WAIT_IPADDR);
        // Set the initial time, where timeout will be started
        xStartTime = xTaskGetTickCount();
        break;
      case STATE_WAIT_IPADDR :
        if (gotIpAddr && gotIp6Addr)
          newDevState(STATE_GOT_IPADDR);
        else
        {
          // Check the timeout.
          if (xTaskGetTickCount() >= xStartTime + xTicksTimeout)
          {
            printf("%% Timeout waiting for all IP addresses\n");
            if (gotIpAddr || gotIp6Addr)
              newDevState(STATE_GOT_IPADDR);
            else
              newDevState(STATE_NEW);
          }
          else
          {
            vTaskDelay(NET_WAIT_MS / portTICK_PERIOD_MS);
          }
        }
        break;
      case STATE_GOT_IPADDR :
        newDevState(STATE_OTA_UPDATING);
        break;
      case STATE_OTA_UPDATING :
        // No OTA for this sketch.
        newDevState(STATE_OTA_COMPLETE);
        break;
      case STATE_OTA_COMPLETE :
        // No longer block here running sshServer.begin()/handleClient()
        // SSH starts from event_cb when IP is ready
        newDevState(STATE_LISTENING);
        break;
      case STATE_LISTENING :
        // Idle state; nothing to do
        vTaskDelay(NET_WAIT_MS / portTICK_PERIOD_MS);
        break;
      case STATE_TCP_DISCONNECTED :
        // This would be the place to free net resources, if needed,
        newDevState(STATE_LISTENING);
        break;
      default :
        break;
    }
  }
}

void setup()
{
  devState = STATE_NEW;

  Serial.begin(115200);

  esp_netif_init();
  esp_event_loop_create_default();
  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_cb, NULL, NULL);
  esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, event_cb, NULL, NULL);

  // Stack size needs to be larger, so continue in a new task.
  xTaskCreatePinnedToCore(controlTask, "ctl", configSTACK, NULL,
    (tskIDLE_PRIORITY + 3), NULL, portNUM_PROCESSORS - 1);
}

void loop()
{
  // Nothing to do here since controlTask has taken over.
  vTaskDelay(60000 / portTICK_PERIOD_MS);
}