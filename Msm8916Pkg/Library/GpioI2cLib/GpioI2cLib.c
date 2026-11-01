/*
 * Minimal bit-banged I2C master on top of the TLMM GPIO protocol.
 *
 * Lines are driven open-drain style: "low" means output 0, "high" means
 * switching the pin to input and letting the board pull-ups take it high.
 * This avoids having to bring up the BLSP QUP controller and its clocks.
 */

#include <Library/LKEnvLib.h>
#include <Library/QcomGpioTlmmLib.h>
#include <Library/TimerLib.h>

#include <Library/GpioI2cLib.h>

#define I2C_STRETCH_TIMEOUT_US 1000

STATIC VOID
LineLow (
  IN UINT32  Gpio
  )
{
  gGpioTlmm->DirectionOutput(Gpio, 0);
}

STATIC VOID
LineRelease (
  IN UINT32  Gpio
  )
{
  gGpioTlmm->DirectionInput(Gpio);
}

STATIC UINTN
LineGet (
  IN UINT32  Gpio
  )
{
  return gGpioTlmm->Get(Gpio);
}

STATIC VOID
Delay (
  IN GPIO_I2C_BUS  *Bus
  )
{
  MicroSecondDelay(Bus->HalfPeriodUs);
}

STATIC EFI_STATUS
SclRelease (
  IN GPIO_I2C_BUS  *Bus
  )
{
  UINTN Timeout = I2C_STRETCH_TIMEOUT_US;

  LineRelease(Bus->SclGpio);

  /* Honour clock stretching */
  while (!LineGet(Bus->SclGpio)) {
    if (Timeout-- == 0) {
      return EFI_TIMEOUT;
    }
    MicroSecondDelay(1);
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
I2cStart (
  IN GPIO_I2C_BUS  *Bus
  )
{
  EFI_STATUS Status;

  /* Also works as a repeated start when SCL is currently low */
  LineRelease(Bus->SdaGpio);
  Delay(Bus);
  Status = SclRelease(Bus);
  if (EFI_ERROR(Status)) {
    return Status;
  }
  Delay(Bus);

  LineLow(Bus->SdaGpio);
  Delay(Bus);
  LineLow(Bus->SclGpio);
  Delay(Bus);

  return EFI_SUCCESS;
}

STATIC VOID
I2cStop (
  IN GPIO_I2C_BUS  *Bus
  )
{
  LineLow(Bus->SdaGpio);
  Delay(Bus);
  SclRelease(Bus);
  Delay(Bus);
  LineRelease(Bus->SdaGpio);
  Delay(Bus);
}

STATIC EFI_STATUS
I2cWriteByte (
  IN GPIO_I2C_BUS  *Bus,
  IN UINT8         Byte
  )
{
  EFI_STATUS Status;
  INTN       Bit;
  UINTN      Nack;

  for (Bit = 7; Bit >= 0; Bit--) {
    if (Byte & (1 << Bit)) {
      LineRelease(Bus->SdaGpio);
    } else {
      LineLow(Bus->SdaGpio);
    }
    Delay(Bus);
    Status = SclRelease(Bus);
    if (EFI_ERROR(Status)) {
      return Status;
    }
    Delay(Bus);
    LineLow(Bus->SclGpio);
  }

  /* Read the ACK bit */
  LineRelease(Bus->SdaGpio);
  Delay(Bus);
  Status = SclRelease(Bus);
  if (EFI_ERROR(Status)) {
    return Status;
  }
  Delay(Bus);
  Nack = LineGet(Bus->SdaGpio);
  LineLow(Bus->SclGpio);

  return Nack ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS
I2cReadByte (
  IN  GPIO_I2C_BUS  *Bus,
  OUT UINT8         *Byte,
  IN  BOOLEAN       Ack
  )
{
  EFI_STATUS Status;
  UINTN      Bit;
  UINT8      Value = 0;

  LineRelease(Bus->SdaGpio);

  for (Bit = 0; Bit < 8; Bit++) {
    Delay(Bus);
    Status = SclRelease(Bus);
    if (EFI_ERROR(Status)) {
      return Status;
    }
    Delay(Bus);
    Value = (Value << 1) | (LineGet(Bus->SdaGpio) ? 1 : 0);
    LineLow(Bus->SclGpio);
  }

  /* Send ACK (more bytes wanted) or NACK (last byte) */
  if (Ack) {
    LineLow(Bus->SdaGpio);
  } else {
    LineRelease(Bus->SdaGpio);
  }
  Delay(Bus);
  Status = SclRelease(Bus);
  if (EFI_ERROR(Status)) {
    return Status;
  }
  Delay(Bus);
  LineLow(Bus->SclGpio);
  LineRelease(Bus->SdaGpio);

  *Byte = Value;
  return EFI_SUCCESS;
}

VOID
GpioI2cInit (
  IN GPIO_I2C_BUS  *Bus
  )
{
  UINTN Count;

  gGpioTlmm->SetFunction(Bus->SdaGpio, 0);
  gGpioTlmm->SetFunction(Bus->SclGpio, 0);
  gGpioTlmm->SetDriveStrength(Bus->SdaGpio, 2);
  gGpioTlmm->SetDriveStrength(Bus->SclGpio, 2);
  /* The board has external pull-ups, the internal ones just help a bit */
  gGpioTlmm->SetPull(Bus->SdaGpio, GPIO_PULL_UP);
  gGpioTlmm->SetPull(Bus->SclGpio, GPIO_PULL_UP);

  LineRelease(Bus->SdaGpio);
  LineRelease(Bus->SclGpio);
  Delay(Bus);

  /* Bus recovery: clock out any slave that is still holding SDA low */
  for (Count = 0; Count < 9 && !LineGet(Bus->SdaGpio); Count++) {
    LineLow(Bus->SclGpio);
    Delay(Bus);
    LineRelease(Bus->SclGpio);
    Delay(Bus);
  }

  I2cStop(Bus);
}

EFI_STATUS
GpioI2cTransfer (
  IN  GPIO_I2C_BUS  *Bus,
  IN  UINT8         Address,
  IN  CONST UINT8   *WriteBuf,
  IN  UINTN         WriteLen,
  OUT UINT8         *ReadBuf,
  IN  UINTN         ReadLen
  )
{
  EFI_STATUS Status;
  UINTN      Index;

  Status = I2cStart(Bus);
  if (EFI_ERROR(Status)) {
    goto Out;
  }

  if (WriteLen != 0) {
    Status = I2cWriteByte(Bus, (UINT8)(Address << 1));
    if (EFI_ERROR(Status)) {
      goto Out;
    }

    for (Index = 0; Index < WriteLen; Index++) {
      Status = I2cWriteByte(Bus, WriteBuf[Index]);
      if (EFI_ERROR(Status)) {
        goto Out;
      }
    }

    if (ReadLen != 0) {
      Status = I2cStart(Bus);
      if (EFI_ERROR(Status)) {
        goto Out;
      }
    }
  }

  if (ReadLen != 0) {
    Status = I2cWriteByte(Bus, (UINT8)((Address << 1) | 1));
    if (EFI_ERROR(Status)) {
      goto Out;
    }

    for (Index = 0; Index < ReadLen; Index++) {
      Status = I2cReadByte(Bus, &ReadBuf[Index], Index + 1 < ReadLen);
      if (EFI_ERROR(Status)) {
        goto Out;
      }
    }
  }

Out:
  I2cStop(Bus);
  return Status;
}
