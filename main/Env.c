// CO2 + other sensors all
const char TAG[] = "CO2";

#include "revk.h"
#include <driver/i2c.h>
#include <math.h>

#include "owb.h"
#include "owb_rmt.h"
#include "ds18b20.h"

#define ACK_CHECK_EN 0x1        /*!< I2C master will check ack from slave */
#define ACK_CHECK_DIS 0x0       /*!< I2C master will not check ack from slave */
#define ACK_VAL 0x0             /*!< I2C ack value */
#define NACK_VAL 0x1            /*!< I2C nack value */
#define	MAX_OWB	8
#define DS18B20_RESOLUTION   (DS18B20_RESOLUTION_12_BIT)

#define settings	\
	s8(co2sda,17)	\
	s8(co2scl,16)	\
	s8(co2address,0x61)	\
	s8(co2places,-1)	\
	u32(co2damp,100)	\
	s8(tempplaces,1)	\
	s8(rhplaces,0)	\
	u32(rhdamp,10)	\
	s8(ds18b20,19)	\
	s8(oledsda,5)	\
	s8(oledscl,18)	\
	s8(oledaddress,0x3D)	\
	b(oledflip)	\
	b(f)	\
	s(fanon)	\
	s(fanoff)	\
	u32(fanco2,1000)	\

#define u32(n,d)	uint32_t n;
#define s8(n,d)	int8_t n;
#define b(n) uint8_t n;
#define s(n) char * n;
settings
#undef u32
#undef s8
#undef b
#undef s
static float lastco2 = -10000;
static float lastrh = -10000;
static float lasttemp = -10000;
static float lastotemp = -10000;
static int lastfan = -1;
static float thisco2 = -10000;
static float thistemp = -10000;
static float thisrh = -10000;
static SemaphoreHandle_t i2c_mutex = NULL;
static SemaphoreHandle_t oled_mutex = NULL;
static int8_t co2port = -1,
   oledport = -1;
static int8_t num_owb = 0;
static OneWireBus *owb = NULL;
static owb_rmt_driver_info rmt_driver_info;
static DS18B20_Info *ds18b20s[MAX_OWB] = { 0 };


static float
report (const char *tag, float last, float this, int places)
{
   float mag = powf (10.0, -places);    // Rounding
   if (this < last)
   {
      this += mag * 0.3;        // Hysteresis
      if (this > last)
         return last;
   } else if (this > last)
   {
      this -= mag * 0.3;        // Hysteresis
      if (this < last)
         return last;
   }
   this = roundf (this / mag) * mag;
   if (this == last)
      return last;
   if (places <= 0)
      revk_info (tag, "%d", (int) this);
   else
      revk_info (tag, "%.*f", places, this);
   return this;
}

static void
sendall (void)
{
   lastco2 = -10000;
   lasttemp = -10000;
   lastotemp = -10000;
   lastrh = -10000;
   lastfan = -1;
}

const char *
app_command (const char *tag, unsigned int len, const unsigned char *value)
{
   if (!strcmp (tag, "connect"))
      sendall ();
   return "";
}

#define	OLEDW	128
#define	OLEDH	128
#define	OLEDB	4
static uint8_t oled[OLEDW * OLEDH * OLEDB / 8];
static volatile char oledchanged = 1;

static inline void
oledbit (int x, int y, uint8_t v)
{
   uint8_t m = (1 << OLEDB) - 1;
   if (x < 0 || x >= OLEDW)
      return;
   if (y < 0 || y >= OLEDH)
      return;
   if (v > m)
      v = m;
   int s = ((8 / OLEDB) - 1 - (x % (8 / OLEDB))) * OLEDB;
   int o = y * OLEDW * OLEDB / 8 + x * OLEDB / 8;
   uint8_t new = ((oled[o] & ~(m << s)) | (v << s));
   if (oled[o] == new)
      return;
   oled[o] = new;
   oledchanged = 1;
}

static inline int
oledcopy (int x, int y, const uint8_t * src, int dx)
{                               // Copy pixels
   x -= x % (8 / OLEDB);        // Align to byte
   dx -= dx % (8 / OLEDB);      // Align to byte
   if (y >= 0 && y < OLEDH && x + dx >= 0 && x < OLEDW)
   {                            // Fits
      int pix = dx;
      if (x < 0)
      {                         // Truncate left
         pix += x;
         x = 0;
      }
      if (x + pix > OLEDW)
         pix = OLEDW - x;       // Truncate right
      uint8_t *dst = oled + y * OLEDW * OLEDB / 8 + x * OLEDB / 8;
      if (memcmp (dst, src, pix * OLEDB / 8))
      {                         // Changed
         memcpy (dst, src, pix * OLEDB / 8);
         oledchanged = 1;
      }
   }
   return dx * OLEDB / 8;       // Bytes (would be) copied
}

#include CONFIG_ENV_LOGO
#include "font1.h"
#include "font2.h"
#include "font3.h"
#include "font4.h"
#include "font5.h"
const uint8_t *const fonts[] = { font1, font2, font3, font4, font5 };

static inline int
textw (uint8_t size)
{
   return size * 6;
}

static inline int
texth (uint8_t size)
{
   return size * 9;
}

int
text (uint8_t size, int x, int y, char *t)
{
   if (!size)
      size = 1;
   else if (size > sizeof (fonts) / sizeof (*fonts))
      size = sizeof (fonts) / sizeof (*fonts);
   int w = textw (size);
   int h = texth (size);
   y -= size * 2;               // Baseline
   while (*t)
   {
      int c = *t++;
      if (c >= 0x7F)
         continue;
      const uint8_t *base = fonts[size - 1] + (c - ' ') * h * w * OLEDB / 8;
      int ww = w;
      if (c < ' ')
      {                         // Sub space
         ww = size * c;
         c = ' ';
      }
      if (c == '.' || c == ':')
      {
         ww = size * 2;
         base += size * 2 * OLEDB / 8;
      }                         // Special case for .
      c -= ' ';
      for (int dy = 0; dy < h; dy++)
      {
         oledcopy (x, y + h - 1 - dy, base, ww);
         base += w * OLEDB / 8;
      }
      x += ww;
   }
   return x;
}

void
oled_task (void *p)
{
   int try = 10;
   esp_err_t e;
   while (try--)
   {
      if (i2c_mutex)
         xSemaphoreTake (i2c_mutex, portMAX_DELAY);
      oledchanged = 0;
      i2c_cmd_handle_t t = i2c_cmd_link_create ();
      i2c_master_start (t);
      i2c_master_write_byte (t, (oledaddress << 1) | I2C_MASTER_WRITE, true);
      i2c_master_write_byte (t, 0x00, true);    // Cmds
      i2c_master_write_byte (t, 0xA5, true);    // White
      i2c_master_write_byte (t, 0xAF, true);    // On
      i2c_master_write_byte (t, 0xA0, true);    // Remap
      i2c_master_write_byte (t, oledflip ? 0x41 : 0x52, true);  // Match display
      i2c_master_stop (t);
      e = i2c_master_cmd_begin (oledport, t, 10 / portTICK_PERIOD_MS);
      i2c_cmd_link_delete (t);
      if (i2c_mutex)
         xSemaphoreGive (i2c_mutex);
      if (!e)
         break;
      sleep (1);
   }
   if (e)
   {
      revk_error ("OLED", "Configuration failed %s", esp_err_to_name (e));
      vTaskDelete (NULL);
      return;
   }

   memset (oled, 0x00, sizeof (oled));  // Blank
   {
      int w = sizeof (logo[0]) * 8 / OLEDB;
      int h = sizeof (logo) / sizeof(*logo);
      for (int dy = 0; dy < h; dy++)
         oledcopy (OLEDW - w, 10 + h - dy, logo[dy], w);
      text (1, 0, 0, CONFIG_ENV_TAG);
   }

   char running = 0;
   while (1)
   {                            // Update
      if (!oledchanged)
      {
         usleep (100000);
         continue;
      }
      if (i2c_mutex)
         xSemaphoreTake (i2c_mutex, portMAX_DELAY);
      xSemaphoreTake (oled_mutex, portMAX_DELAY);
      oledchanged = 0;
      i2c_cmd_handle_t t;
      e = 0;
      if (running < 2)
      {                         // Set up
         t = i2c_cmd_link_create ();
         i2c_master_start (t);
         i2c_master_write_byte (t, (oledaddress << 1) | I2C_MASTER_WRITE, true);
         i2c_master_write_byte (t, 0x00, true); // Cmds
         if (running)
            i2c_master_write_byte (t, 0xA4, true);      // Normal mode
         i2c_master_write_byte (t, 0x15, true); // Col
         i2c_master_write_byte (t, 0x00, true); // 0
         i2c_master_write_byte (t, 0x7F, true); // 127
         i2c_master_write_byte (t, 0x75, true); // Row
         i2c_master_write_byte (t, 0x00, true); // 0
         i2c_master_write_byte (t, 0x7F, true); // 127
         i2c_master_stop (t);
         e = i2c_master_cmd_begin (oledport, t, 100 / portTICK_PERIOD_MS);
         i2c_cmd_link_delete (t);
      }
      if (!e)
      {                         // data
         t = i2c_cmd_link_create ();
         i2c_master_start (t);
         i2c_master_write_byte (t, (oledaddress << 1) | I2C_MASTER_WRITE, true);
         i2c_master_write_byte (t, 0x40, true); // Data
         i2c_master_write (t, oled, sizeof (oled), true);       // Buffer
         i2c_master_stop (t);
         e = i2c_master_cmd_begin (oledport, t, 100 / portTICK_PERIOD_MS);
         i2c_cmd_link_delete (t);
      }
      if (e)
         revk_error ("OLED", "Data failed %s", esp_err_to_name (e));
      if (!running || e)
      {
         running = 1;
         oledchanged = 1;
      } else
         running = 2;
      xSemaphoreGive (oled_mutex);
      if (i2c_mutex)
         xSemaphoreGive (i2c_mutex);
   }
}

void
co2_task (void *p)
{
   p = p;
   int try = 10;
   esp_err_t e;
   while (try--)
   {
      if (i2c_mutex)
         xSemaphoreTake (i2c_mutex, portMAX_DELAY);
      i2c_cmd_handle_t i = i2c_cmd_link_create ();
      i2c_master_start (i);
      i2c_master_write_byte (i, (co2address << 1), ACK_CHECK_EN);
      i2c_master_write_byte (i, 0x00, ACK_CHECK_EN);    // 0010=start measurements
      i2c_master_write_byte (i, 0x10, ACK_CHECK_EN);
      i2c_master_write_byte (i, 0x00, ACK_CHECK_EN);    // Pressure (0=unknown)
      i2c_master_write_byte (i, 0x00, ACK_CHECK_EN);
      i2c_master_write_byte (i, 0x81, ACK_CHECK_EN);    // CRC
      i2c_master_stop (i);
      e = i2c_master_cmd_begin (co2port, i, 10 / portTICK_PERIOD_MS);
      i2c_cmd_link_delete (i);
      if (i2c_mutex)
         xSemaphoreGive (i2c_mutex);
      if (!e)
         break;
      sleep (1);
   }
   if (e)
   {                            // failed
      revk_error ("CO2", "Configuration failed %s", esp_err_to_name (e));
      vTaskDelete (NULL);
      return;
   }
   // Get measurements
   while (1)
   {
      usleep (100000);
      if (i2c_mutex)
         xSemaphoreTake (i2c_mutex, portMAX_DELAY);
      i2c_cmd_handle_t i = i2c_cmd_link_create ();
      i2c_master_start (i);
      i2c_master_write_byte (i, (co2address << 1), ACK_CHECK_EN);
      i2c_master_write_byte (i, 0x02, ACK_CHECK_EN);    // 0202 get reading state
      i2c_master_write_byte (i, 0x02, ACK_CHECK_EN);
      i2c_master_stop (i);
      esp_err_t err = i2c_master_cmd_begin (co2port, i, 10 / portTICK_PERIOD_MS);
      i2c_cmd_link_delete (i);
      if (err)
         ESP_LOGI (TAG, "Tx GetReady %s", esp_err_to_name (err));
      else
      {
         uint8_t buf[3];
         i = i2c_cmd_link_create ();
         i2c_master_start (i);
         i2c_master_write_byte (i, (co2address << 1) + 1, ACK_CHECK_EN);
         i2c_master_read (i, buf, 2, ACK_VAL);
         i2c_master_read_byte (i, buf + 2, NACK_VAL);
         i2c_master_stop (i);
         esp_err_t err = i2c_master_cmd_begin (co2port, i, 10 / portTICK_PERIOD_MS);
         i2c_cmd_link_delete (i);
         if (err)
            ESP_LOGI (TAG, "Rx GetReady %s", esp_err_to_name (err));
         else if ((buf[0] << 8) + buf[1] == 1)
         {
            i = i2c_cmd_link_create ();
            i2c_master_start (i);
            i2c_master_write_byte (i, (co2address << 1), ACK_CHECK_EN);
            i2c_master_write_byte (i, 0x03, ACK_CHECK_EN);      // 0300 Read data
            i2c_master_write_byte (i, 0x00, ACK_CHECK_EN);
            i2c_master_stop (i);
            esp_err_t err = i2c_master_cmd_begin (co2port, i, 10 / portTICK_PERIOD_MS);
            i2c_cmd_link_delete (i);
            if (err)
               ESP_LOGI (TAG, "Tx GetData %s", esp_err_to_name (err));
            else
            {
               uint8_t buf[18];
               i = i2c_cmd_link_create ();
               i2c_master_start (i);
               i2c_master_write_byte (i, (co2address << 1) + 1, ACK_CHECK_EN);
               i2c_master_read (i, buf, 17, ACK_VAL);
               i2c_master_read_byte (i, buf + 17, NACK_VAL);
               i2c_master_stop (i);
               esp_err_t err = i2c_master_cmd_begin (co2port, i, 10 / portTICK_PERIOD_MS);
               i2c_cmd_link_delete (i);
               if (err)
                  ESP_LOGI (TAG, "Rx Data %s", esp_err_to_name (err));
               else
               {
                  //ESP_LOG_BUFFER_HEX_LEVEL (TAG, buf, 18, ESP_LOG_INFO);
                  uint8_t d[4];
                  d[3] = buf[0];
                  d[2] = buf[1];
                  d[1] = buf[3];
                  d[0] = buf[4];
                  float co2 = *(float *) d;
                  d[3] = buf[6];
                  d[2] = buf[7];
                  d[1] = buf[9];
                  d[0] = buf[10];
                  float t = *(float *) d;
                  d[3] = buf[12];
                  d[2] = buf[13];
                  d[1] = buf[15];
                  d[0] = buf[16];
                  float rh = *(float *) d;
                  if (thisco2 < 0)
                     thisco2 = co2;
                  else
                     thisco2 = (thisco2 * co2damp + co2) / (co2damp + 1);
                  if (thisrh < 0)
                     thisrh = rh;
                  else
                     thisrh = (thisrh * rhdamp + rh) / (rhdamp + 1);
                  if (!num_owb)
                     lasttemp = report ("temp", lasttemp, thistemp = t, tempplaces);    // Use temp here as no DS18B20
                  lastco2 = report ("co2", lastco2, thisco2, co2places);
                  lastrh = report ("rh", lastrh, thisrh, rhplaces);
               }
            }
         }
      }
      if (i2c_mutex)
         xSemaphoreGive (i2c_mutex);
   }
}

void
ds18b20_task (void *p)
{
   p = p;
   while (1)
   {
      usleep (100000);
      ds18b20_convert_all (owb);
      ds18b20_wait_for_conversion (ds18b20s[0]);
      float readings[MAX_OWB] = { 0 };
      DS18B20_ERROR errors[MAX_OWB] = { 0 };
      for (int i = 0; i < num_owb; ++i)
         errors[i] = ds18b20_read_temp (ds18b20s[i], &readings[i]);
      if (!errors[0])
         lasttemp = report ("temp", lasttemp, thistemp = readings[0], tempplaces);      // Use temp here as no DS18B20
      if (num_owb > 1 && !errors[1])
         lastotemp = report ("otemp", lastotemp, readings[1], tempplaces);
   }
}

void
app_main ()
{
   revk_init (&app_command);
#define b(n) revk_register(#n,0,sizeof(n),&n,NULL,SETTING_BOOLEAN);
#define u32(n,d) revk_register(#n,0,sizeof(n),&n,#d,0);
#define s8(n,d) revk_register(#n,0,sizeof(n),&n,#d,SETTING_SIGNED);
#define s(n) revk_register(#n,0,0,&n,NULL,0);
   settings
#undef u32
#undef s8
#undef b
#undef s
      oled_mutex = xSemaphoreCreateMutex ();    // Shared text access
   if (co2sda >= 0 && co2scl >= 0)
   {
      co2port = 0;
      if (i2c_driver_install (co2port, I2C_MODE_MASTER, 0, 0, 0))
      {
         revk_error ("CO2", "I2C config fail");
         co2port = -1;
      } else
      {
         i2c_config_t config = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = co2sda,
            .scl_io_num = co2scl,
            .sda_pullup_en = true,
            .scl_pullup_en = true,
            .master.clk_speed = 100000,
         };
         if (i2c_param_config (co2port, &config))
         {
            i2c_driver_delete (co2port);
            revk_error ("CO2", "I2C config fail");
            co2port = -1;
         } else
            i2c_set_timeout (co2port, 160000);  // 2ms? allow for clock stretching
      }
   }
   if (oledsda == co2sda && oledscl == co2scl)
   {
      oledport = co2port;
      i2c_mutex = xSemaphoreCreateMutex ();     // Shared I2C
   } else if (oledsda >= 0 && oledscl >= 0)
   {                            // Separate OLED port
      oledport = 1;
      if (i2c_driver_install (oledport, I2C_MODE_MASTER, 0, 0, 0))
      {
         revk_error ("OLED", "I2C config fail");
         oledport = -1;
      } else
      {
         i2c_config_t config = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = oledsda,
            .scl_io_num = oledscl,
            .sda_pullup_en = true,
            .scl_pullup_en = true,
            .master.clk_speed = 100000,
         };
         if (i2c_param_config (oledport, &config))
         {
            i2c_driver_delete (oledport);
            revk_error ("OLED", "I2C config fail");
            oledport = -1;
         } else
            i2c_set_timeout (oledport, 160000); // 2ms? allow for clock stretching
      }
   }
   if (co2port >= 0)
      revk_task ("CO2", co2_task, NULL);
   if (oledport >= 0)
      revk_task ("OLED", oled_task, NULL);
   if (ds18b20 >= 0)
   {                            // DS18B20 init
      owb = owb_rmt_initialize (&rmt_driver_info, ds18b20, RMT_CHANNEL_1, RMT_CHANNEL_0);
      owb_use_crc (owb, true);  // enable CRC check for ROM code
      OneWireBus_ROMCode device_rom_codes[MAX_OWB] = { 0 };
      OneWireBus_SearchState search_state = { 0 };
      bool found = false;
      owb_search_first (owb, &search_state, &found);
      while (found && num_owb < MAX_OWB)
      {
         char rom_code_s[17];
         owb_string_from_rom_code (search_state.rom_code, rom_code_s, sizeof (rom_code_s));
         device_rom_codes[num_owb] = search_state.rom_code;
         ++num_owb;
         owb_search_next (owb, &search_state, &found);
      }
      for (int i = 0; i < num_owb; i++)
      {
         DS18B20_Info *ds18b20_info = ds18b20_malloc ();        // heap allocation
         ds18b20s[i] = ds18b20_info;
         if (num_owb == 1)
            ds18b20_init_solo (ds18b20_info, owb);      // only one device on bus
         else
            ds18b20_init (ds18b20_info, owb, device_rom_codes[i]);      // associate with bus and device
         ds18b20_use_crc (ds18b20_info, true);  // enable CRC check for temperature readings
         ds18b20_set_resolution (ds18b20_info, DS18B20_RESOLUTION);
      }
      if (!num_owb)
         revk_error ("temp", "No OWB devices");
      else
         revk_task ("DS18B20", ds18b20_task, NULL);
   }
   // Main task...
   while (1)
   {
      xSemaphoreTake (oled_mutex, portMAX_DELAY);
      char s[30];
      static time_t showtime = 0;
      time_t now = time (0);
      if (now != showtime)
      {
         showtime = now;
         struct tm *t;
         t = localtime (&showtime);
         static char lasth = -1;
         if (t->tm_hour != lasth)
         {
            lasth = t->tm_hour;
            sendall ();
         }
         if (t->tm_year > 100)
         {
            strftime (s, sizeof (s), "%F\004%T %Z", t);
            text (1, 0, 0, s);
         }
      }
      int x,
        y = OLEDH - 1,
         space = (OLEDH - 28 - 35 - 21 - 9) / 3;
      y -= 28;
      static float showco2 = -1000;
      if (thisco2 != showco2)
      {
         showco2 = thisco2;
         if (showco2 < 300)
            strcpy (s, "____");
         else if (showco2 >= 10000)
            strcpy (s, "^^^^");
         else
            sprintf (s, "%4d", (int) showco2);
         x = text (4, 0, y, s);
         text (1, x, y + 9, "CO2");
         x = text (1, x, y, "ppm");
      }
      y -= space;               // Space
      y -= 35;
      static float showtemp = -1000;
      if (thistemp != showtemp)
      {
         showtemp = thistemp;
         if (f)
         {                      // Fahrenheit
            int fh = (showtemp + 40.0) * 1.8 - 40.0;
            if (fh <= -100)
               strcpy (s, "___");
            else if (fh >= 1000)
               strcpy (s, "^^^");
            else
               sprintf (s, "%3d", fh);
         } else
         {                      // Celsius
            if (showtemp <= -10)
               strcpy (s, "__._");
            else if (showtemp >= 100)
               strcpy (s, "^^.^");
            else
               sprintf (s, "%4.1f", showtemp);
         }
         x = text (5, 10, y, s);
         x = text (1, x, y + 12, "o");
         x = text (2, x, y, f ? "F" : "C");
      }
      y -= space;               // Space
      y -= 21;
      static float showrh = -1000;
      if (thisrh != showrh)
      {
         showrh = thisrh;
         if (showrh <= 0)
            strcpy (s, "__");
         else if (showrh >= 100)
            strcpy (s, "^^");
         else
            sprintf (s, "%2d", (int) showrh);
         x = text (3, 0, y, s);
         x = text (2, x, y, "%");
         text (1, x, y + 8, "R");
         x = text (1, x, y, "H");
      }
      y -= space;
      xSemaphoreGive (oled_mutex);
      // Fan control
      const char *fan = NULL;
      if (thisco2 > fanco2 && lastfan != 1)
      {
         fan = fanon;
         lastfan = 1;
      } else if (thisco2 < fanco2 && lastfan != 0)
      {
         fan = fanoff;
         lastfan = 0;
      }
      if (fan && *fan)
      {
         char *topic = strdup (fan);
         char *data = strchr (topic, ' ');
         if (data)
            *data++ = 0;
         revk_raw (NULL, topic, data ? strlen (data) : 0, data, 0);
         free (topic);
      }
      // Next second
      usleep (1000000LL - (esp_timer_get_time () % 1000000LL));
   }
}
