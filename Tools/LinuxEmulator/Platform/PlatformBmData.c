/** @file
  Headless console for the Linux Redfish emulator. SerialPortLib uses stdin/stdout.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "PlatformBm.h"
#include <Guid/SerialPortLibVendor.h>
#include <Guid/PcAnsi.h>
#include <Protocol/SerialIo.h>

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Serial;
  UART_DEVICE_PATH            Uart;
  VENDOR_DEVICE_PATH          Terminal;
  EFI_DEVICE_PATH_PROTOCOL    End;
} LINUX_SERIAL_DEVICE_PATH;
#pragma pack()

STATIC LINUX_SERIAL_DEVICE_PATH  mSerial = {
  {
    {
      HARDWARE_DEVICE_PATH, HW_VENDOR_DP,{
        sizeof (VENDOR_DEVICE_PATH), 0
      }
    }, EDKII_SERIAL_PORT_LIB_VENDOR_GUID
  },
  {
    {
      MESSAGING_DEVICE_PATH, MSG_UART_DP,{
        sizeof (UART_DEVICE_PATH), 0
      }
    }, 0, 115200, 8, NoParity, OneStopBit
  },
  {
    {
      MESSAGING_DEVICE_PATH, MSG_VENDOR_DP,{
        sizeof (VENDOR_DEVICE_PATH), 0
      }
    }, EFI_VT_100_GUID
  },
  gEndEntire
};

BDS_CONSOLE_CONNECT_ENTRY  gPlatformConsole[] = {
  { (EFI_DEVICE_PATH_PROTOCOL *)&mSerial, CONSOLE_ALL },
  { NULL,                                 0           }
};
