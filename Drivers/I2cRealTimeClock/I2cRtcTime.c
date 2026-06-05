/** @file
  Unix timestamp ↔ EFI_TIME conversion and I2C read helpers.
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Protocol/I2cMaster.h>

#include "Include/I2cRtc.h"

// Days in each month for a non-leap year (index 0 = January).
STATIC CONST UINT8  mDaysInMonth[12] = {
  31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

STATIC
BOOLEAN
IsLeapYear (
  IN UINT16  Year
  )
{
  return ((Year % 4 == 0) && ((Year % 100 != 0) || (Year % 400 == 0)));
}

VOID
UnixTimeToEfiTime (
  IN  UINT32    UnixTime,
  OUT EFI_TIME  *EfiTime
  )
{
  UINT32  Days;
  UINT32  SecsInDay;
  UINT16  Year;
  UINT8   Month;
  UINT32  DaysInYear;
  UINT8   DaysThisMonth;

  ASSERT (EfiTime != NULL);
  ZeroMem (EfiTime, sizeof (EFI_TIME));

  SecsInDay = UnixTime % 86400;
  Days      = UnixTime / 86400;

  EfiTime->Hour   = (UINT8)(SecsInDay / 3600);
  EfiTime->Minute = (UINT8)((SecsInDay % 3600) / 60);
  EfiTime->Second = (UINT8)(SecsInDay % 60);

  EfiTime->Nanosecond = 0;
  EfiTime->TimeZone   = EFI_UNSPECIFIED_TIMEZONE;
  EfiTime->Daylight   = 0;

  // Walk years forward from the epoch.
  Year = 1970;
  while (TRUE) {
    DaysInYear = IsLeapYear (Year) ? 366 : 365;
    if (Days < DaysInYear) {
      break;
    }
    Days -= DaysInYear;
    Year++;
  }
  EfiTime->Year = Year;

  // Walk months forward within the year.
  for (Month = 0; Month < 12; Month++) {
    DaysThisMonth = mDaysInMonth[Month];
    if ((Month == 1) && IsLeapYear (Year)) {
      DaysThisMonth = 29;
    }
    if (Days < DaysThisMonth) {
      break;
    }
    Days -= DaysThisMonth;
  }
  EfiTime->Month = (UINT8)(Month + 1); // EFI_TIME months are 1-based
  EfiTime->Day   = (UINT8)(Days + 1);  // EFI_TIME days are 1-based
}

EFI_STATUS
I2cRtcReadUnixTime (
  IN  EFI_I2C_MASTER_PROTOCOL  *I2cMaster,
  OUT UINT32                   *UnixTime
  )
{
  EFI_STATUS               Status;
  UINT8                    RegAddr;
  UINT8                    RawBytes[I2C_RTC_TIMESTAMP_SIZE];
  EFI_I2C_REQUEST_PACKET   *RequestPacket;
  // Allocate on the stack: header + 2 operations (write reg addr, read data).
  UINT8                    PacketBuf[sizeof (EFI_I2C_REQUEST_PACKET) +
                                     sizeof (EFI_I2C_OPERATION)];

  if ((I2cMaster == NULL) || (UnixTime == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  RegAddr = I2C_RTC_REG_TIME;

  RequestPacket                             = (EFI_I2C_REQUEST_PACKET *)PacketBuf;
  RequestPacket->OperationCount             = 2;

  // Operation 0: write the register address.
  RequestPacket->Operation[0].Flags         = 0; // I2C write
  RequestPacket->Operation[0].LengthInBytes = sizeof (RegAddr);
  RequestPacket->Operation[0].Buffer        = &RegAddr;

  // Operation 1: read the timestamp bytes.
  RequestPacket->Operation[1].Flags         = I2C_FLAG_READ;
  RequestPacket->Operation[1].LengthInBytes = I2C_RTC_TIMESTAMP_SIZE;
  RequestPacket->Operation[1].Buffer        = RawBytes;

  Status = I2cMaster->StartRequest (
                         I2cMaster,
                         I2C_RTC_SLAVE_ADDR,
                         RequestPacket,
                         NULL, // synchronous — no event
                         NULL
                         );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "I2cRtc: I2C read failed: %r\n", Status));
    return EFI_DEVICE_ERROR;
  }

  // Timestamp is 4 bytes, little-endian.
  *UnixTime = (UINT32)RawBytes[0]
            | ((UINT32)RawBytes[1] << 8)
            | ((UINT32)RawBytes[2] << 16)
            | ((UINT32)RawBytes[3] << 24);

  return EFI_SUCCESS;
}
