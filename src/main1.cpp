#include <Arduino.h>
#include "USBHIDPendant.h"
#include "Pendant_WHB04B6.h"
#include "GrblCode.h"
#include "SerialDebug.h"

// Example used as base for USB HID stuff:
// https://github.com/adafruit/Adafruit_TinyUSB_Arduino/tree/master/examples/DualRole/HID/hid_device_report

/*********************************************************************
 Adafruit invests time and resources providing this open source code,
 please support Adafruit and open-source hardware by purchasing
 products from Adafruit!

 MIT license, check LICENSE for more information
 Copyright (c) 2019 Ha Thach for Adafruit Industries
 All text above, and the splash screen below must be included in
 any redistributionf
*********************************************************************/

// pio-usb is required for rp2040 host
#include "pio_usb.h"
#define HOST_PIN_DP 16 // GPIO Pin used as D+ for host, D- = D+ + 1

#include "Adafruit_TinyUSB.h"

#define LANGUAGE_ID 0x0409 // English

// USB Host object
Adafruit_USBH_Host USBHost;

// holding device descriptor
tusb_desc_device_t desc_device;

#define MOUNT_CHECK_INTERVAL 1000
#define MAX_DEV 5
struct USBHIDPendantDevice devices[MAX_DEV];

// the setup function runs once when you press reset or power the board

// core1's setup
void setup1()
{
  // while ( !Serial ) delay(10);   // wait for native usb
  Serial.println("Core1 setup to run TinyUSB host with pio-usb");

  // Check for CPU frequency, must be multiple of 120Mhz for bit-banging USB
  uint32_t cpu_hz = clock_get_hz(clk_sys);
  if (cpu_hz != 120000000UL && cpu_hz != 240000000UL)
  {
    while (!Serial)
      delay(10); // wait for native usb
    Serial.printf("Error: CPU Clock = %u, PIO USB require CPU clock must be multiple of 120 Mhz\r\n", cpu_hz);
    Serial.printf("Change your CPU Clock to either 120 or 240 Mhz in Menu->CPU Speed \r\n", cpu_hz);
    while (1)
      delay(1);
  }

  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
  pio_cfg.pin_dp = HOST_PIN_DP;
  USBHost.configure_pio_usb(1, &pio_cfg);

  for (uint8_t i = 0; i < MAX_DEV; i++)
  {
    devices[i].dev_addr = 0;
    devices[i].instance = 0;
    devices[i].object = 0;
  }
  // run host stack on controller (rhport) 1
  // Note: For rp2040 pico-pio-usb, calling USBHost.begin() on core1 will have most of the
  // host bit-banging processing works done in core1 to free up core0 for other works

  // Send ready state and wait for other core
  Serial.println("Core1 Ready");
  rp2040.fifo.push(0);
  rp2040.fifo.pop();
  Serial.println("Core1 Start");

  USBHost.begin(1);
}

void check_devices_still_mounted(unsigned long now)
{
  for (uint8_t i = 0; i < MAX_DEV; i++)
  {
    if (devices[i].object)
    {
      if (!tuh_hid_mounted(devices[i].dev_addr, devices[i].instance))
      {
#if SERIALDEBUG > 0
        Serial.printf("HID device address = %d, instance = %d no longer mounted\r\n", devices[i].dev_addr, devices[i].instance);
#endif
        delete devices[i].object;
        devices[i].dev_addr = 0;
        devices[i].instance = 0;
        devices[i].object = 0;
      }
      else
      {
#if (KEEPALIVE)
        Serial.printf("Mounted @ %d\r\n", now);
#endif
      }
    }
  }
}

// core1's loop
void loop1()
{
  USBHost.task();

  static unsigned long last_mount_check = millis();
  unsigned long now = millis();
  if ((now - last_mount_check) > MOUNT_CHECK_INTERVAL)
  {
    last_mount_check = millis();
    check_devices_still_mounted(last_mount_check);
  }

  // check for new Grbl status messages from Serial routines on other core
  // Create pointer to new structure and set to null address
  GRBLSTATUS *GrblStatus = 0;
  if (rp2040.fifo.available())
    GrblStatus = (GRBLSTATUS *)rp2040.fifo.pop();

  // loop through devices, forward any grbl status messages and call loop function
  for (uint8_t i = 0; i < MAX_DEV; i++)
  {
    if (devices[i].object)
    {
      if (GrblStatus)
        devices[i].object->grblstatus_received(GrblStatus);

      devices[i].object->loop();
    }
  }
  // Cleanup
  if (GrblStatus)
    delete GrblStatus;
}

// Invoked when device with hid interface is mounted
// Report descriptor is also available for use.
// tuh_hid_parse_report_descriptor() can be used to parse common/simple enough
// descriptor. Note: if report descriptor length > CFG_TUH_ENUMERATION_BUFSIZE,
// it will be skipped therefore report_desc = NULL, desc_len = 0
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len)
{
  (void)desc_report;
  (void)desc_len;
  uint16_t vid, pid;

#if SERIALDEBUG > 1
  if (desc_len)
  {
    for (uint8_t i = 0; i < desc_len; i++)
    {
      if ((i % 8) == 0)
        Serial.println();
      Serial.print((char)*desc_report, HEX);
      Serial.write(" ");
      desc_report++;
    }
    Serial.println();
  }
#endif

  // check if already mounted, immitate unmount first if it is
  for (uint8_t i = 0; i < MAX_DEV; i++)
  {
    if (devices[i].dev_addr == dev_addr && devices[i].instance == instance && devices[i].object)
    {
      Serial.printf("HID device address = %d, instance = %d was already mounted, treat as unmounted first\r\n", dev_addr, instance);
      delete devices[i].object;
      devices[i].object = 0;
    }
  }

  tuh_vid_pid_get(dev_addr, &vid, &pid);

  Serial.printf("HID device address = %d, instance = %d is mounted\r\n", dev_addr, instance);
  Serial.printf("VID = %04x, PID = %04x\r\n", vid, pid);
  if (!tuh_hid_receive_report(dev_addr, instance))
  {
    Serial.printf("Error: cannot request to receive report\r\n");
    return;
  }
  // Interface protocol (hid_interface_protocol_enum_t)
  const char *protocol_str[] = {"None", "Keyboard", "Mouse"};
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
  Serial.printf("HID Interface Protocol = %s(%d)\r\n", protocol_str[itf_protocol], itf_protocol);

  // loop though device object slots
  for (uint8_t i = 0; i < MAX_DEV; i++)
  {
    // search a free slot
    if (!devices[i].object)
    {
      // prepare slot
      devices[i].dev_addr = dev_addr;
      devices[i].instance = instance;
      USBHIDPendant *object = 0;
      // check if known device type and create matching object
      //
      // // WHB04B-6 Wireless CNC Pendant
      // https://github.com/LinuxCNC/linuxcnc/tree/master/src/hal/user_comps/xhc-whb04b-6
      if (itf_protocol == HID_ITF_PROTOCOL_NONE && vid == 0x10ce && pid == 0xeb93)
      {
        Serial.printf("Found new WHB04B-6 device\r\n");
        object = new Pendant_WHB04B6(dev_addr, instance);
      }
      devices[i].object = object;
      break;
    }
  }
}

// Invoked when device with hid interface is un-mounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
  Serial.printf("HID device address = %d, instance = %d is unmounted\r\n", dev_addr, instance);

  // check if object exists for device and destroy it
  for (uint8_t i = 0; i < MAX_DEV; i++)
  {
    if (devices[i].dev_addr == dev_addr && devices[i].instance == instance && devices[i].object)
    {
      delete devices[i].object;
      devices[i].object = 0;
    }
  }
}

// Invoked when received report from device via interrupt endpoint
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len)
{
  if (false)
  {
    Serial.printf("HIDreport from device address = %d, instance = %d : ", dev_addr, instance);
    for (uint16_t i = 0; i < len; i++)
    {
      Serial.printf("0x%02X ", report[i]);
    }
    Serial.println();
  } // continue to request to receive report
  if (!tuh_hid_receive_report(dev_addr, instance))
  {
    Serial.printf("Error: cannot request to receive report\r\n");
  }

  // check if object exists for device and pass report
  for (uint8_t i = 0; i < MAX_DEV; i++)
  {
    if (devices[i].dev_addr == dev_addr && devices[i].instance == instance && devices[i].object)
    {
      devices[i].object->report_received(report, len);
    }
  }
}

void tuh_hid_set_report_complete_cb(uint8_t dev_addr, uint8_t instance, uint8_t report_id, uint8_t report_type, uint16_t len)
{
#if SERIALDEBUG > 1
  Serial.printf("HID set report complete for device address = %d, instance = %d, report_id = %d, report_type = %d, len = %d\r\n", dev_addr, instance, report_id, report_type, len);
#endif

  // search device object and call report callback
  for (uint8_t i = 0; i < MAX_DEV; i++)
  {
    if (devices[i].dev_addr == dev_addr && devices[i].instance == instance && devices[i].object)
    {
      devices[i].object->set_report_complete(report_id, report_type, len);
    }
  }
}
