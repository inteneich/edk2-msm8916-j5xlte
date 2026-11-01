/*
 * J5Diag: dump the CPU / timer / GIC state Windows' HAL depends on.
 *
 *   J5Diag          safe register reads only
 *   J5Diag psci     also ask the secure firmware for PSCI_VERSION (SMC)
 *   J5Diag mmio     also read the memory mapped timer frame (CNTCTLBase)
 *   J5Diag usb      dump the ChipIdea USB host port, then do an EHCI
 *                   HCRESET like Windows' usbehci and show what survives
 *
 * The risky probes print what they are about to do first, so if the
 * phone hangs the last line on screen says which probe did it.
 */

#include <Uefi.h>
#include <Library/ArmGenericTimerCounterLib.h>
#include <Library/ArmLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/LoadedImage.h>

#define GICD_CTLR    0x000
#define GICD_TYPER   0x004
#define GICD_IIDR    0x008
#define GICD_PIDR2   0xFE8
#define GICC_CTLR    0x000
#define GICC_IIDR    0x0FC

#define MSM8916_CNTCTLBASE 0x0B020000
#define CNTCR              0x000
#define CNTFID0            0x020

#define PSCI_VERSION_FN    0x84000000

#define USB_CAP            0x078D9100
#define USB_HCCPARAMS      (USB_CAP + 0x08)
#define USB_HCSPARAMS      (USB_CAP + 0x04)
#define OP_USBCMD          0x00
#define OP_USBSTS          0x04
#define OP_PORTSC          0x44
#define OP_OTGSC           0x64
#define OP_DEVLC           0x84
#define OP_OTGSC_LPM       0xC4
#define OP_USBMODE         0x68
#define OP_USBMODE_LPM     0xC8

STATIC VOID
DumpUsb (
  IN CONST CHAR16  *When,
  IN UINTN         Op,
  IN BOOLEAN       Lpm
  )
{
  UINT32 PortSc = MmioRead32(Op + OP_PORTSC);
  UINT32 Mode   = MmioRead32(Op + (Lpm ? OP_USBMODE_LPM : OP_USBMODE));
  UINT32 OtgSc  = MmioRead32(Op + (Lpm ? OP_OTGSC_LPM : OP_OTGSC));

  Print(L"-- %s\n", When);
  Print(L"USBCMD 0x%08x  USBSTS 0x%08x\n", MmioRead32(Op + OP_USBCMD), MmioRead32(Op + OP_USBSTS));
  Print(L"USBMODE 0x%08x  (CM=%u: 0 idle, 2 device, 3 host)\n", Mode, Mode & 3);
  Print(L"PORTSC 0x%08x  connect=%u enabled=%u power=%u PTS=%u\n",
        PortSc, PortSc & 1, (PortSc >> 2) & 1, (PortSc >> 12) & 1, PortSc >> 30);
  if (Lpm) {
    UINT32 DevLc = MmioRead32(Op + OP_DEVLC);
    Print(L"DEVLC/HOSTPC 0x%08x  PTS=%u speed=%u (0 FS, 1 LS, 2 HS)\n",
          DevLc, DevLc >> 29, (DevLc >> 25) & 3);
  }
  Print(L"OTGSC 0x%08x  ID=%u (0 = OTG adapter) BSessValid=%u\n",
        OtgSc, (OtgSc >> 8) & 1, (OtgSc >> 11) & 1);
}

STATIC VOID
UsbTest (
  VOID
  )
{
  UINTN   Op  = USB_CAP + (MmioRead32(USB_CAP) & 0xFF);
  BOOLEAN Lpm = (MmioRead32(USB_HCCPARAMS) & BIT17) != 0;
  UINTN   Timeout;

  Print(L"\n[usb] HCSPARAMS 0x%08x HCCPARAMS 0x%08x LPM %u\n",
        MmioRead32(USB_HCSPARAMS), MmioRead32(USB_HCCPARAMS), Lpm);
  DumpUsb(L"as left by UsbHostDxe", Op, Lpm);

  Print(L"[usb] HCRESET (what usbehci does) ...\n");
  MmioOr32(Op + OP_USBCMD, BIT1);
  for (Timeout = 100000; (MmioRead32(Op + OP_USBCMD) & BIT1) && Timeout; Timeout--) {
    gBS->Stall(1);
  }
  DumpUsb(L"after HCRESET", Op, Lpm);

  Print(L"[usb] restoring host mode + ULPI + port power, waiting 500ms ...\n");
  MmioAndThenOr32(Op + (Lpm ? OP_USBMODE_LPM : OP_USBMODE), ~3U, 3);
  if (Lpm) {
    MmioAndThenOr32(Op + OP_DEVLC, ~(0x7U << 29), 0x2U << 29);
  } else {
    MmioAndThenOr32(Op + OP_PORTSC, ~(0x3U << 30 | BIT25), 0x2U << 30);
  }
  MmioOr32(Op + OP_PORTSC, BIT12);
  gBS->Stall(500000);
  DumpUsb(L"after restoring host mode", Op, Lpm);
}

STATIC UINT64
ReadCurrentEL (
  VOID
  )
{
  UINT64 Value;

  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (Value));
  return (Value >> 2) & 3;
}

STATIC BOOLEAN
HasArg (
  IN EFI_HANDLE    ImageHandle,
  IN CONST CHAR16  *Arg
  )
{
  EFI_LOADED_IMAGE_PROTOCOL *Image;
  CHAR16                    *Cmd;
  UINTN                     Len;
  UINTN                     ArgLen = StrLen(Arg);
  UINTN                     Index;

  if (EFI_ERROR(gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Image)) ||
      Image->LoadOptions == NULL) {
    return FALSE;
  }

  Cmd = Image->LoadOptions;
  Len = Image->LoadOptionsSize / sizeof(CHAR16);

  for (Index = 0; Index + ArgLen <= Len; Index++) {
    if (CompareMem(&Cmd[Index], Arg, ArgLen * sizeof(CHAR16)) == 0) {
      return TRUE;
    }
  }

  return FALSE;
}

EFI_STATUS
EFIAPI
J5DiagMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINTN       GicD = (UINTN)PcdGet64(PcdGicDistributorBase);
  UINTN       GicC = (UINTN)PcdGet64(PcdGicInterruptInterfaceBase);
  UINT64      Freq;
  UINT64      Count0, Count1;
  UINT32      Typer;
  ARM_SMC_ARGS Smc;

  Print(L"J5Diag\n");
  Print(L"CurrentEL      : EL%lu\n", ReadCurrentEL());
  Print(L"MIDR           : 0x%08lx\n", (UINT64)ArmReadMidr());
  Print(L"MPIDR          : 0x%08lx\n", (UINT64)ArmReadMpidr());

  Freq   = ArmGenericTimerGetTimerFreq();
  Count0 = ArmGenericTimerGetSystemCount();
  gBS->Stall(100000);
  Count1 = ArmGenericTimerGetSystemCount();
  Print(L"CNTFRQ_EL0     : %lu Hz\n", Freq);
  Print(L"CNTPCT +100ms  : %lu ticks (expect ~%lu)\n", Count1 - Count0, Freq / 10);

  Typer = MmioRead32(GicD + GICD_TYPER);
  Print(L"GICD base      : 0x%lx\n", (UINT64)GicD);
  Print(L"GICD_CTLR      : 0x%08x\n", MmioRead32(GicD + GICD_CTLR));
  Print(L"GICD_TYPER     : 0x%08x (%u IRQs, security ext %u)\n",
        Typer, ((Typer & 0x1F) + 1) * 32, (Typer >> 10) & 1);
  Print(L"GICD_IIDR      : 0x%08x\n", MmioRead32(GicD + GICD_IIDR));
  Print(L"GICD_PIDR2     : 0x%08x (arch rev %u)\n",
        MmioRead32(GicD + GICD_PIDR2), (MmioRead32(GicD + GICD_PIDR2) >> 4) & 0xF);
  Print(L"GICC base      : 0x%lx\n", (UINT64)GicC);
  Print(L"GICC_CTLR      : 0x%08x\n", MmioRead32(GicC + GICC_CTLR));
  Print(L"GICC_IIDR      : 0x%08x\n", MmioRead32(GicC + GICC_IIDR));

  if (HasArg(ImageHandle, L"mmio")) {
    Print(L"\n[mmio] reading CNTCTLBase 0x%x ...\n", MSM8916_CNTCTLBASE);
    Print(L"CNTCR          : 0x%08x\n", MmioRead32(MSM8916_CNTCTLBASE + CNTCR));
    Print(L"CNTFID0        : %u\n", MmioRead32(MSM8916_CNTCTLBASE + CNTFID0));
  }

  if (HasArg(ImageHandle, L"usb")) {
    UsbTest();
  }

  if (HasArg(ImageHandle, L"psci")) {
    Print(L"\n[psci] SMC PSCI_VERSION ...\n");
    ZeroMem(&Smc, sizeof(Smc));
    Smc.Arg0 = PSCI_VERSION_FN;
    ArmCallSmc(&Smc);
    if ((INT32)Smc.Arg0 < 0) {
      Print(L"PSCI           : not supported (ret %d)\n", (INT32)Smc.Arg0);
    } else {
      Print(L"PSCI           : v%lu.%lu\n", (Smc.Arg0 >> 16) & 0xFFFF, Smc.Arg0 & 0xFFFF);
    }
  }

  Print(L"\nDone.\n");
  return EFI_SUCCESS;
}
