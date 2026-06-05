/** @file
  I2C-backed Real Time Clock Arch Protocol driver.

  Reads Unix timestamp from an external I2C device and exposes it
  as the UEFI RTC so that gRT->GetTime() works system-wide and the
  Linux kernel can obtain the correct time at boot without a
  dedicated hardware RTC chip.
**/

#ifndef I2C_RTC_H_
#define I2C_RTC_H_

#include <Uefi.h>
#include <Protocol/I2cMaster.h>

// -----------------------------------------------------------------------
// Board-specific constants — adjust for your hardware
// -----------------------------------------------------------------------

// 7-bit I2C address of the device that supplies the Unix timestamp.
#define I2C_RTC_SLAVE_ADDR   0x68

// The device sends a 4-byte (UINT32) little-endian Unix timestamp.
#define I2C_RTC_REG_TIME     0x00
#define I2C_RTC_TIMESTAMP_SIZE  4

// GUID used to locate the I2C master protocol instance on the correct
// controller.  Replace with the GUID exported by your platform's I2C
// host controller driver.
#define I2C_RTC_MASTER_GUID \
  { 0x00000000, 0x0000, 0x0000, \
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } }

// -----------------------------------------------------------------------

/**
  Read a 4-byte Unix timestamp from the I2C device.

  @param[in]  I2cMaster   Pointer to EFI_I2C_MASTER_PROTOCOL.
  @param[out] UnixTime    Receives the Unix timestamp (seconds since
                          1970-01-01 00:00:00 UTC).

  @retval EFI_SUCCESS           Timestamp read successfully.
  @retval EFI_DEVICE_ERROR      I2C transaction failed.
  @retval EFI_INVALID_PARAMETER A pointer argument is NULL.
**/
EFI_STATUS
I2cRtcReadUnixTime (
  IN  EFI_I2C_MASTER_PROTOCOL  *I2cMaster,
  OUT UINT32                   *UnixTime
  );

/**
  Convert a Unix timestamp to an EFI_TIME structure.

  @param[in]  UnixTime   Seconds since 1970-01-01 00:00:00 UTC.
  @param[out] EfiTime    Receives the broken-down time.
**/
VOID
UnixTimeToEfiTime (
  IN  UINT32    UnixTime,
  OUT EFI_TIME  *EfiTime
  );

#endif // I2C_RTC_H_
