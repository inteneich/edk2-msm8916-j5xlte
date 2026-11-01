/*
 * Imagis IST3038C touchscreen driver.
 *
 * Produces EFI_ABSOLUTE_POINTER_PROTOCOL so the UEFI console (ConSplitter)
 * and touch aware boot loaders (e.g. Windows Boot Manager) can use it.
 *
 * Register protocol taken from the mainline Linux driver
 * (drivers/input/touchscreen/imagis.c).
 */

#include <Library/LKEnvLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/QcomGpioTlmmLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/AbsolutePointer.h>
#include <Protocol/DevicePath.h>

#include <Library/GpioI2cLib.h>

#define GPIO_NONE 0xFFFFFFFF

#define IST3038C_HIB_ACCESS          (0x800BU << 16)
#define IST3038C_DIRECT_ACCESS       BIT31
#define IST3038C_REG_CHIPID          (0x40001000U | IST3038C_DIRECT_ACCESS)
#define IST3038C_REG_HIB_BASE        0x30000100U
#define IST3038C_REG_INTR_MESSAGE    (IST3038C_REG_HIB_BASE | IST3038C_HIB_ACCESS | 0x4)
#define IST3038C_REG_TOUCH_COORD     (IST3038C_REG_HIB_BASE | IST3038C_HIB_ACCESS | 0x8)
#define IST3038C_WHOAMI              0x38c
#define IST3038C_CHIP_ON_DELAY_MS    60
#define IST3038C_I2C_RETRY_COUNT     3
#define IST3038C_MAX_FINGER_NUM      10

#define IST3038C_X(v)                (((v) >> 12) & 0xFFF)
#define IST3038C_Y(v)                ((v) & 0xFFF)
#define IST3038C_FINGER_COUNT(v)     (((v) >> 12) & 0xF)
#define IST3038C_FINGER_STATUS(v)    ((v) & 0x3FF)

/* Poll at 50Hz, and read unconditionally every 5th tick in case an INT edge was missed */
#define TOUCH_POLL_PERIOD_100NS      (20 * 10000)
#define TOUCH_FORCED_POLL_TICKS      5

#define TOUCH_DEV_SIGNATURE SIGNATURE_32('I', 'S', 'T', 'C')

typedef struct {
  VENDOR_DEVICE_PATH       Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} TOUCH_DEVICE_PATH;

typedef struct {
  UINT32                        Signature;
  EFI_HANDLE                    Handle;
  EFI_ABSOLUTE_POINTER_PROTOCOL AbsPointer;
  EFI_ABSOLUTE_POINTER_MODE     Mode;
  TOUCH_DEVICE_PATH             DevicePath;

  GPIO_I2C_BUS                  Bus;
  UINT8                         Address;
  UINT32                        IntGpio;

  EFI_EVENT                     PollEvent;
  UINTN                         TickCount;

  EFI_ABSOLUTE_POINTER_STATE    State;
  BOOLEAN                       StateChanged;
} TOUCH_DEV;

#define TOUCH_DEV_FROM_ABSPOINTER(a) CR(a, TOUCH_DEV, AbsPointer, TOUCH_DEV_SIGNATURE)

STATIC TOUCH_DEVICE_PATH mTouchDevicePathTemplate = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof(VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof(VENDOR_DEVICE_PATH)) >> 8),
      },
    },
    EFI_CALLER_ID_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {sizeof(EFI_DEVICE_PATH_PROTOCOL), 0},
  },
};

STATIC EFI_STATUS
Ist3038cReadReg (
  IN  TOUCH_DEV  *Dev,
  IN  UINT32     Reg,
  OUT UINT32     *Value
  )
{
  EFI_STATUS Status = EFI_DEVICE_ERROR;
  UINT8      RegBe[4];
  UINT8      Data[4];
  UINTN      Retry;

  RegBe[0] = (UINT8)(Reg >> 24);
  RegBe[1] = (UINT8)(Reg >> 16);
  RegBe[2] = (UINT8)(Reg >> 8);
  RegBe[3] = (UINT8)Reg;

  for (Retry = 0; Retry < IST3038C_I2C_RETRY_COUNT; Retry++) {
    Status = GpioI2cTransfer(&Dev->Bus, Dev->Address, RegBe, sizeof(RegBe), Data, sizeof(Data));
    if (!EFI_ERROR(Status)) {
      *Value = ((UINT32)Data[0] << 24) | ((UINT32)Data[1] << 16) |
               ((UINT32)Data[2] << 8) | Data[3];
      return EFI_SUCCESS;
    }
  }

  return Status;
}

STATIC VOID
TransformCoordinates (
  IN OUT UINT64  *X,
  IN OUT UINT64  *Y
  )
{
  UINT64 Tmp;

  if (FixedPcdGetBool(PcdTouchSwapXY)) {
    Tmp = *X;
    *X  = *Y;
    *Y  = Tmp;
  }

  if (*X > FixedPcdGet32(PcdTouchMaxX) - 1) {
    *X = FixedPcdGet32(PcdTouchMaxX) - 1;
  }
  if (*Y > FixedPcdGet32(PcdTouchMaxY) - 1) {
    *Y = FixedPcdGet32(PcdTouchMaxY) - 1;
  }

  if (FixedPcdGetBool(PcdTouchInvertX)) {
    *X = FixedPcdGet32(PcdTouchMaxX) - 1 - *X;
  }
  if (FixedPcdGetBool(PcdTouchInvertY)) {
    *Y = FixedPcdGet32(PcdTouchMaxY) - 1 - *Y;
  }
}

STATIC VOID
Ist3038cPoll (
  IN TOUCH_DEV  *Dev
  )
{
  EFI_STATUS Status;
  UINT32     Message;
  UINT32     Coord;
  UINTN      FingerCount;
  UINTN      FingerStatus;
  UINTN      Index;
  BOOLEAN    Touching = FALSE;
  UINT64     X = Dev->State.CurrentX;
  UINT64     Y = Dev->State.CurrentY;

  Status = Ist3038cReadReg(Dev, IST3038C_REG_INTR_MESSAGE, &Message);
  if (EFI_ERROR(Status)) {
    return;
  }

  FingerCount  = IST3038C_FINGER_COUNT(Message);
  FingerStatus = IST3038C_FINGER_STATUS(Message);
  if (FingerCount > IST3038C_MAX_FINGER_NUM) {
    return;
  }

  /* Only the first finger that is down is reported, it acts like a mouse */
  for (Index = 0; Index < FingerCount; Index++) {
    if (!(FingerStatus & (1U << Index))) {
      continue;
    }

    Status = Ist3038cReadReg(Dev, IST3038C_REG_TOUCH_COORD + (Index * 4), &Coord);
    if (EFI_ERROR(Status)) {
      return;
    }

    X        = IST3038C_X(Coord);
    Y        = IST3038C_Y(Coord);
    Touching = TRUE;
    TransformCoordinates(&X, &Y);
    break;
  }

  if (Touching != ((Dev->State.ActiveButtons & EFI_ABSP_TouchActive) != 0) ||
      X != Dev->State.CurrentX || Y != Dev->State.CurrentY) {
    Dev->State.CurrentX      = X;
    Dev->State.CurrentY      = Y;
    Dev->State.CurrentZ      = 0;
    Dev->State.ActiveButtons = Touching ? EFI_ABSP_TouchActive : 0;
    Dev->StateChanged        = TRUE;
  }
}

STATIC VOID
EFIAPI
TouchPollNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  TOUCH_DEV *Dev     = Context;
  BOOLEAN   Pending  = FALSE;

  Dev->TickCount++;

  /* INT is active low; keep reading while a finger is down so we see the release */
  if (Dev->IntGpio == GPIO_NONE || gGpioTlmm->Get(Dev->IntGpio) == 0) {
    Pending = TRUE;
  }
  if (Dev->State.ActiveButtons & EFI_ABSP_TouchActive) {
    Pending = TRUE;
  }
  if (Dev->TickCount % TOUCH_FORCED_POLL_TICKS == 0) {
    Pending = TRUE;
  }

  if (Pending) {
    Ist3038cPoll(Dev);
  }
}

STATIC EFI_STATUS
EFIAPI
TouchReset (
  IN EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  IN BOOLEAN                        ExtendedVerification
  )
{
  TOUCH_DEV *Dev = TOUCH_DEV_FROM_ABSPOINTER(This);
  EFI_TPL   OldTpl;

  OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  ZeroMem(&Dev->State, sizeof(Dev->State));
  Dev->StateChanged = FALSE;
  gBS->RestoreTPL(OldTpl);

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
EFIAPI
TouchGetState (
  IN     EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  IN OUT EFI_ABSOLUTE_POINTER_STATE     *State
  )
{
  TOUCH_DEV  *Dev = TOUCH_DEV_FROM_ABSPOINTER(This);
  EFI_TPL    OldTpl;
  EFI_STATUS Status = EFI_NOT_READY;

  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  if (Dev->StateChanged) {
    CopyMem(State, &Dev->State, sizeof(*State));
    Dev->StateChanged = FALSE;
    Status            = EFI_SUCCESS;
  }
  gBS->RestoreTPL(OldTpl);

  return Status;
}

STATIC VOID
EFIAPI
TouchWaitForInput (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  TOUCH_DEV *Dev = Context;

  if (Dev->StateChanged) {
    gBS->SignalEvent(Event);
  }
}

STATIC VOID
Ist3038cPowerOn (
  IN TOUCH_DEV  *Dev
  )
{
  UINT32 PowerGpio = FixedPcdGet32(PcdTouchPowerEnableGpio);

  if (PowerGpio != GPIO_NONE) {
    gGpioTlmm->SetFunction(PowerGpio, 0);
    gGpioTlmm->SetDriveStrength(PowerGpio, 2);
    gGpioTlmm->SetPull(PowerGpio, GPIO_PULL_NONE);
    gGpioTlmm->DirectionOutput(PowerGpio, 1);
  }

  if (Dev->IntGpio != GPIO_NONE) {
    gGpioTlmm->SetFunction(Dev->IntGpio, 0);
    gGpioTlmm->SetDriveStrength(Dev->IntGpio, 2);
    gGpioTlmm->SetPull(Dev->IntGpio, GPIO_PULL_NONE);
    gGpioTlmm->DirectionInput(Dev->IntGpio);
  }

  MicroSecondDelay(IST3038C_CHIP_ON_DELAY_MS * 1000);
}

EFI_STATUS
EFIAPI
Ist3038cTouchDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;
  TOUCH_DEV  *Dev;
  UINT32     ChipId;

  Dev = AllocateZeroPool(sizeof(*Dev));
  if (Dev == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Dev->Signature        = TOUCH_DEV_SIGNATURE;
  Dev->Bus.SdaGpio      = FixedPcdGet32(PcdTouchI2cSdaGpio);
  Dev->Bus.SclGpio      = FixedPcdGet32(PcdTouchI2cSclGpio);
  Dev->Bus.HalfPeriodUs = 5; /* ~100kHz */
  Dev->Address          = FixedPcdGet8(PcdTouchI2cAddress);
  Dev->IntGpio          = FixedPcdGet32(PcdTouchIntGpio);

  Ist3038cPowerOn(Dev);
  GpioI2cInit(&Dev->Bus);

  Status = Ist3038cReadReg(Dev, IST3038C_REG_CHIPID, &ChipId);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "IST3038C: no response at 0x%02x: %r\n", Dev->Address, Status));
    goto Fail;
  }

  if (ChipId != IST3038C_WHOAMI) {
    /* Not fatal, other IST30xxC parts speak the same protocol */
    DEBUG((EFI_D_ERROR, "IST3038C: unexpected chip id 0x%08x\n", ChipId));
  } else {
    DEBUG((EFI_D_ERROR, "IST3038C: found chip id 0x%x\n", ChipId));
  }

  Dev->Mode.AbsoluteMinX = 0;
  Dev->Mode.AbsoluteMinY = 0;
  Dev->Mode.AbsoluteMinZ = 0;
  Dev->Mode.AbsoluteMaxX = FixedPcdGet32(PcdTouchMaxX);
  Dev->Mode.AbsoluteMaxY = FixedPcdGet32(PcdTouchMaxY);
  Dev->Mode.AbsoluteMaxZ = 0;
  Dev->Mode.Attributes   = 0;

  Dev->AbsPointer.Reset    = TouchReset;
  Dev->AbsPointer.GetState = TouchGetState;
  Dev->AbsPointer.Mode     = &Dev->Mode;

  CopyMem(&Dev->DevicePath, &mTouchDevicePathTemplate, sizeof(Dev->DevicePath));

  Status = gBS->CreateEvent(
      EVT_NOTIFY_WAIT, TPL_NOTIFY, TouchWaitForInput, Dev,
      &Dev->AbsPointer.WaitForInput);
  if (EFI_ERROR(Status)) {
    goto Fail;
  }

  Status = gBS->CreateEvent(
      EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_NOTIFY, TouchPollNotify, Dev,
      &Dev->PollEvent);
  if (EFI_ERROR(Status)) {
    goto FailWaitEvent;
  }

  Status = gBS->SetTimer(Dev->PollEvent, TimerPeriodic, TOUCH_POLL_PERIOD_100NS);
  if (EFI_ERROR(Status)) {
    goto FailPollEvent;
  }

  Status = gBS->InstallMultipleProtocolInterfaces(
      &Dev->Handle,
      &gEfiAbsolutePointerProtocolGuid, &Dev->AbsPointer,
      &gEfiDevicePathProtocolGuid, &Dev->DevicePath,
      NULL);
  if (EFI_ERROR(Status)) {
    goto FailPollEvent;
  }

  return EFI_SUCCESS;

FailPollEvent:
  gBS->CloseEvent(Dev->PollEvent);
FailWaitEvent:
  gBS->CloseEvent(Dev->AbsPointer.WaitForInput);
Fail:
  FreePool(Dev);
  return Status;
}
