/** @file
  DXE driver that installs EfiRealTimeClockArchProtocol backed by an
  I2C device supplying a Unix timestamp.

  The driver:
    1. Locates the EFI_I2C_MASTER_PROTOCOL on the target controller.
    2. Reads the current Unix timestamp from the I2C device.
    3. Installs EfiRealTimeClockArchProtocol so that gRT->GetTime()
       returns the correct time to all UEFI consumers and to the Linux
       kernel via EFI runtime services.

  Because there is no writable hardware RTC, SetTime() and the wakeup
  timer stubs return EFI_UNSUPPORTED.  Linux will fall back to reading
  time via GetTime() which is sufficient for hctosys.
**/

#include <Uefi.h>
#include <PiDxe.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>
#include <Library/UefiLib.h>

#include <Protocol/RealTimeClock.h>
#include <Protocol/I2cMaster.h>

#include "Include/I2cRtc.h"

// -----------------------------------------------------------------------
// Module globals
// -----------------------------------------------------------------------

// Handle on which EfiRealTimeClockArchProtocol is installed.
STATIC EFI_HANDLE  mRtcHandle = NULL;

// Cached I2C master — used by GetTime at runtime.
STATIC EFI_I2C_MASTER_PROTOCOL  *mI2cMaster = NULL;

// Cached time read at EndOfDxe; used after ExitBootServices when I2C
// may no longer be accessible.
STATIC UINT32  mCachedUnixTime    = 0;
STATIC UINT64  mCachedPerformanceTick = 0; // TSC/HPET tick at cache time

// -----------------------------------------------------------------------
// EfiRealTimeClockArchProtocol callbacks
// -----------------------------------------------------------------------

/**
  Return the current time.

  Before ExitBootServices the I2C device is queried directly.
  After ExitBootServices the cached value is adjusted by elapsed
  time using the firmware performance counter so the kernel still
  gets a monotonically increasing timestamp.

  @param[out] Time         Receives broken-down current time.
  @param[out] Capabilities Optional — receives clock capabilities.

  @retval EFI_SUCCESS           Time returned successfully.
  @retval EFI_DEVICE_ERROR      I2C read failed (pre-ExitBootServices).
  @retval EFI_INVALID_PARAMETER Time is NULL.
**/
STATIC
EFI_STATUS
EFIAPI
RtcGetTime (
  OUT EFI_TIME                *Time,
  OUT EFI_TIME_CAPABILITIES   *Capabilities  OPTIONAL
  )
{
  EFI_STATUS  Status;
  UINT32      UnixTime;
  UINT64      Elapsed;

  if (Time == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (!EfiAtRuntime ()) {
    // Boot-services phase: read the device directly.
    Status = I2cRtcReadUnixTime (mI2cMaster, &UnixTime);
    if (EFI_ERROR (Status)) {
      return EFI_DEVICE_ERROR;
    }
    // Refresh cache so the runtime path starts from a recent value.
    mCachedUnixTime        = UnixTime;
    mCachedPerformanceTick = GetPerformanceCounter ();
  } else {
    // Runtime phase (after ExitBootServices): I2C stack is gone.
    // Advance the cached timestamp by the elapsed seconds measured
    // with the performance counter.
    Elapsed = GetTimeInNanoSecond (
                GetPerformanceCounter () - mCachedPerformanceTick
                );
    UnixTime = mCachedUnixTime + (UINT32)(Elapsed / 1000000000ULL);
  }

  UnixTimeToEfiTime (UnixTime, Time);

  if (Capabilities != NULL) {
    Capabilities->Resolution = 1;       // 1 Hz
    Capabilities->Accuracy   = 0;       // unknown drift
    Capabilities->SetsToZero = FALSE;
  }

  return EFI_SUCCESS;
}

/**
  Set the time.

  There is no writeable RTC hardware, so this is a no-op stub.
  Returning EFI_UNSUPPORTED tells the caller (including Linux) that
  time persistence is not available — the kernel handles this
  gracefully and still uses the time returned by GetTime().
**/
STATIC
EFI_STATUS
EFIAPI
RtcSetTime (
  IN EFI_TIME  *Time
  )
{
  return EFI_UNSUPPORTED;
}

/**
  Get wakeup alarm — not supported.
**/
STATIC
EFI_STATUS
EFIAPI
RtcGetWakeupTime (
  OUT BOOLEAN   *Enabled,
  OUT BOOLEAN   *Pending,
  OUT EFI_TIME  *Time
  )
{
  return EFI_UNSUPPORTED;
}

/**
  Set wakeup alarm — not supported.
**/
STATIC
EFI_STATUS
EFIAPI
RtcSetWakeupTime (
  IN BOOLEAN   Enable,
  IN EFI_TIME  *Time  OPTIONAL
  )
{
  return EFI_UNSUPPORTED;
}

// -----------------------------------------------------------------------
// Driver entry point
// -----------------------------------------------------------------------

/**
  DXE driver entry point.

  Locates the I2C master, performs an initial time read to validate
  connectivity, then installs EfiRealTimeClockArchProtocol.

  @param[in] ImageHandle   The firmware-allocated handle for this image.
  @param[in] SystemTable   Pointer to the EFI System Table.

  @retval EFI_SUCCESS           Protocol installed successfully.
  @retval EFI_NOT_FOUND         I2C master protocol not found.
  @retval EFI_DEVICE_ERROR      Initial I2C read failed.
  @retval other                 Protocol installation failed.
**/
EFI_STATUS
EFIAPI
I2cRtcDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT32      UnixTime;
  EFI_GUID    I2cMasterGuid = I2C_RTC_MASTER_GUID;

  // ------------------------------------------------------------------
  // 1. Locate the I2C master that owns the RTC device.
  // ------------------------------------------------------------------
  Status = gBS->LocateProtocol (
                  &I2cMasterGuid,
                  NULL,
                  (VOID **)&mI2cMaster
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "I2cRtc: cannot locate I2C master: %r\n", Status));
    return EFI_NOT_FOUND;
  }

  // ------------------------------------------------------------------
  // 2. Sanity-check: read the time once at boot.
  // ------------------------------------------------------------------
  Status = I2cRtcReadUnixTime (mI2cMaster, &UnixTime);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "I2cRtc: initial time read failed: %r\n", Status));
    return EFI_DEVICE_ERROR;
  }

  mCachedUnixTime        = UnixTime;
  mCachedPerformanceTick = GetPerformanceCounter ();

  DEBUG ((DEBUG_INFO, "I2cRtc: Unix time at boot = %u\n", UnixTime));

  // ------------------------------------------------------------------
  // 3. Install EfiRealTimeClockArchProtocol.
  //    This replaces any default (stub) RTC the platform may have
  //    and wires gRT->GetTime / SetTime to our callbacks.
  // ------------------------------------------------------------------
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mRtcHandle,
                  &gEfiRealTimeClockArchProtocolGuid, NULL,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "I2cRtc: InstallMultipleProtocolInterfaces: %r\n",
            Status));
    return Status;
  }

  // Hook gRT entries to point at our implementations.
  gRT->GetTime        = RtcGetTime;
  gRT->SetTime        = RtcSetTime;
  gRT->GetWakeupTime  = RtcGetWakeupTime;
  gRT->SetWakeupTime  = RtcSetWakeupTime;

  DEBUG ((DEBUG_INFO, "I2cRtc: EfiRealTimeClockArchProtocol installed.\n"));
  return EFI_SUCCESS;
}
