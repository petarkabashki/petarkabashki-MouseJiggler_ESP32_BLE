// port.h on bare ESP-IDF — no Arduino core underneath.
//
// The console is the C3's built-in USB Serial/JTAG peripheral, which is what
// the Arduino build also talks over (ARDUINO_USB_CDC_ON_BOOT=1). Build with
// -DPORT_CONSOLE_UART to use UART0 and an external USB-serial chip instead.
#include "port.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_littlefs.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef PORT_CONSOLE_UART
#include "driver/uart.h"
#define CONSOLE_UART UART_NUM_0
#else
#include "driver/usb_serial_jtag.h"
#endif

void portConsoleInit(void) {
#ifdef PORT_CONSOLE_UART
  const uart_config_t cfg = {
      .baud_rate = 115200,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };
  uart_driver_install(CONSOLE_UART, 512, 512, 0, NULL, 0);
  uart_param_config(CONSOLE_UART, &cfg);
#else
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  usb_serial_jtag_driver_install(&cfg);
#endif
}

static void writeBytes(const char *s, size_t n) {
#ifdef PORT_CONSOLE_UART
  uart_write_bytes(CONSOLE_UART, s, n);
#else
  // A host that is not reading drains nowhere, so this must not block forever:
  // a disconnected terminal would otherwise stall the jiggler.
  usb_serial_jtag_write_bytes(s, n, pdMS_TO_TICKS(20));
#endif
}

void portPut(const char *s) { writeBytes(s, strlen(s)); }
void portPutc(char c) { writeBytes(&c, 1); }

void portPrintf(const char *fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
  writeBytes(buf, (size_t)n);
}

void portFlush(void) {
#ifdef PORT_CONSOLE_UART
  uart_wait_tx_done(CONSOLE_UART, pdMS_TO_TICKS(50));
#else
  usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(50));
#endif
}

int portGetc(void) {
  uint8_t c;
#ifdef PORT_CONSOLE_UART
  int n = uart_read_bytes(CONSOLE_UART, &c, 1, 0);
#else
  int n = usb_serial_jtag_read_bytes(&c, 1, 0);
#endif
  return n == 1 ? (int)c : -1;
}

uint32_t portMillis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void portDelay(uint32_t ms) {
  // vTaskDelay hands the CPU to the idle task, which is what feeds the
  // watchdog, so there is nothing to reset by hand here.
  if (ms) vTaskDelay(pdMS_TO_TICKS(ms));
}

void portYield(void) {
  // Returns ESP_ERR_NOT_FOUND when this task is not subscribed, which is fine
  // and is the normal case for a task that never trips the watchdog.
  esp_task_wdt_reset();
}

// --- filesystem ------------------------------------------------------------
// Same mount point as the Arduino build so the portable file words see the
// same paths. The partition is the one `partitions_huge.csv` labels `spiffs`;
// the label is historical, the contents are LittleFS.
#define FS_MOUNT "/littlefs"

static bool fsMounted;

bool portFsMountInit(void) {
  esp_vfs_littlefs_conf_t conf = {
      .base_path = FS_MOUNT,
      .partition_label = "spiffs",
      .format_if_mount_failed = true,
      .dont_mount = false,
  };
  fsMounted = esp_vfs_littlefs_register(&conf) == ESP_OK;
  return fsMounted;
}

const char *portFsMount(void) { return FS_MOUNT; }

bool portFsInfo(uint32_t *used, uint32_t *total) {
  if (!fsMounted) return false;
  size_t t = 0, u = 0;
  if (esp_littlefs_info("spiffs", &t, &u) != ESP_OK) return false;
  *used = (uint32_t)u;
  *total = (uint32_t)t;
  return true;
}

uint32_t portFreeHeap(void) { return esp_get_free_heap_size(); }
void portRestart(void) { esp_restart(); }

int32_t portRandom(int32_t lo, int32_t hi) {
  if (hi <= lo) return lo;
  return lo + (int32_t)(esp_random() % (uint32_t)(hi - lo));
}
