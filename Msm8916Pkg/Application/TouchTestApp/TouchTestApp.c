/*
 * TouchTest: draws a dot wherever the screen is touched.
 * Press any key (volume / power) to exit.
 */

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/AbsolutePointer.h>
#include <Protocol/GraphicsOutput.h>

#define DOT_SIZE 12

STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mDown = {0x00, 0xFF, 0x00, 0}; /* green */
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mUp   = {0x00, 0x00, 0xFF, 0}; /* red */

STATIC VOID
DrawDot (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop,
  IN UINTN                          X,
  IN UINTN                          Y,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Color
  )
{
  UINTN Width  = Gop->Mode->Info->HorizontalResolution;
  UINTN Height = Gop->Mode->Info->VerticalResolution;

  X = (X >= DOT_SIZE / 2) ? X - DOT_SIZE / 2 : 0;
  Y = (Y >= DOT_SIZE / 2) ? Y - DOT_SIZE / 2 : 0;
  if (X + DOT_SIZE > Width) {
    X = Width - DOT_SIZE;
  }
  if (Y + DOT_SIZE > Height) {
    Y = Height - DOT_SIZE;
  }

  Gop->Blt(Gop, Color, EfiBltVideoFill, 0, 0, X, Y, DOT_SIZE, DOT_SIZE, 0);
}

EFI_STATUS
EFIAPI
TouchTestMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                    Status;
  EFI_ABSOLUTE_POINTER_PROTOCOL *Touch;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  EFI_ABSOLUTE_POINTER_STATE    State;
  EFI_EVENT                     Events[2];
  UINTN                         Index;
  UINTN                         X, Y;
  UINT64                        RangeX, RangeY;
  EFI_INPUT_KEY                 Key;

  Status = gBS->LocateProtocol(&gEfiAbsolutePointerProtocolGuid, NULL, (VOID **)&Touch);
  if (EFI_ERROR(Status)) {
    Print(L"No touchscreen (EFI_ABSOLUTE_POINTER_PROTOCOL) found: %r\n", Status);
    return Status;
  }

  Status = gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR(Status)) {
    Print(L"No GOP found: %r\n", Status);
    return Status;
  }

  RangeX = Touch->Mode->AbsoluteMaxX - Touch->Mode->AbsoluteMinX;
  RangeY = Touch->Mode->AbsoluteMaxY - Touch->Mode->AbsoluteMinY;
  if (RangeX == 0 || RangeY == 0) {
    Print(L"Touchscreen reports an empty range\n");
    return EFI_DEVICE_ERROR;
  }

  gST->ConOut->ClearScreen(gST->ConOut);
  Print(L"TouchTest: touch the screen, press any key to exit\n");
  Print(L"Range X %lu..%lu  Y %lu..%lu\n",
        Touch->Mode->AbsoluteMinX, Touch->Mode->AbsoluteMaxX,
        Touch->Mode->AbsoluteMinY, Touch->Mode->AbsoluteMaxY);

  Touch->Reset(Touch, FALSE);

  Events[0] = Touch->WaitForInput;
  Events[1] = gST->ConIn->WaitForKey;

  for (;;) {
    Status = gBS->WaitForEvent(2, Events, &Index);
    if (EFI_ERROR(Status) || Index == 1) {
      break;
    }

    if (EFI_ERROR(Touch->GetState(Touch, &State))) {
      continue;
    }

    X = (UINTN)(((State.CurrentX - Touch->Mode->AbsoluteMinX) *
                 Gop->Mode->Info->HorizontalResolution) / RangeX);
    Y = (UINTN)(((State.CurrentY - Touch->Mode->AbsoluteMinY) *
                 Gop->Mode->Info->VerticalResolution) / RangeY);

    DrawDot(Gop, X, Y, (State.ActiveButtons & EFI_ABSP_TouchActive) ? &mDown : &mUp);
  }

  gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);
  gST->ConOut->ClearScreen(gST->ConOut);

  return EFI_SUCCESS;
}
