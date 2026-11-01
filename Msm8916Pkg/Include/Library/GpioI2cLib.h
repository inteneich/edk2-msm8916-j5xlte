#ifndef __GPIO_I2C_LIB_H__
#define __GPIO_I2C_LIB_H__

#include <Uefi.h>

typedef struct {
  UINT32 SdaGpio;
  UINT32 SclGpio;
  UINT32 HalfPeriodUs;
} GPIO_I2C_BUS;

VOID
GpioI2cInit (
  IN GPIO_I2C_BUS  *Bus
  );

/*
 * Write WriteLen bytes, then (if ReadLen != 0) issue a repeated start and
 * read ReadLen bytes. Returns EFI_DEVICE_ERROR on NACK or bus timeout.
 */
EFI_STATUS
GpioI2cTransfer (
  IN  GPIO_I2C_BUS  *Bus,
  IN  UINT8         Address,
  IN  CONST UINT8   *WriteBuf,
  IN  UINTN         WriteLen,
  OUT UINT8         *ReadBuf,
  IN  UINTN         ReadLen
  );

#endif
