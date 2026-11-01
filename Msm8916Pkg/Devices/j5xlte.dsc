[Defines]
  PLATFORM_NAME                  = Msm8916Pkg
  PLATFORM_GUID                  = 28f1a3bf-193a-47e3-a7b9-5a435eaab2ee
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010019
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Msm8916Pkg/Devices/j5xlte.fdf

!include Msm8916Pkg/Msm8916Pkg.dsc

[BuildOptions.common]
  GCC:*_*_AARCH64_CC_FLAGS =

[PcdsFixedAtBuild.common]
  # Simple FrameBuffer (5.2" 720x1280)
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferWidth|720
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferHeight|1280
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferAddress|0x8e000000
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferPixelBpp|32
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferVisibleWidth|720
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferVisibleHeight|1280

  # SmBios Information
  gMsm8916PkgTokenSpaceGuid.PcdDeviceVendor|"Samsung"
  gMsm8916PkgTokenSpaceGuid.PcdDeviceProduct|"Galaxy J5 2016"
  gMsm8916PkgTokenSpaceGuid.PcdDeviceCodeName|"j5xlte"

  # SD card detect (sdc2_cd, active low)
  gQcomTokenSpaceGuid.PcdSdCardDetectGpio|38
  gQcomTokenSpaceGuid.PcdSdCardDetectActiveLow|TRUE

  # Imagis IST3038C on blsp_i2c5 (gpio18 SDA / gpio19 SCL), bit-banged
  gQcomTokenSpaceGuid.PcdTouchI2cSdaGpio|18
  gQcomTokenSpaceGuid.PcdTouchI2cSclGpio|19
  gQcomTokenSpaceGuid.PcdTouchI2cAddress|0x50
  gQcomTokenSpaceGuid.PcdTouchIntGpio|13
  # vdd_tsp_a LDO enable (J5 2016 only)
  gQcomTokenSpaceGuid.PcdTouchPowerEnableGpio|108
  gQcomTokenSpaceGuid.PcdTouchMaxX|720
  gQcomTokenSpaceGuid.PcdTouchMaxY|1280

  # SM5703 charger (OTG 5V boost) on i2c-gpio: SDA gpio22, SCL gpio23 (Samsung j5xlte r04 dts)
  gQcomTokenSpaceGuid.PcdOtgBoostI2cSdaGpio|22
  gQcomTokenSpaceGuid.PcdOtgBoostI2cSclGpio|23
  gQcomTokenSpaceGuid.PcdOtgBoostI2cAddress|0x49

  # SM5703 MUIC: i2c-gpio SDA gpio105, SCL gpio106 (mainline msm8916-samsung-j5-common.dtsi)
  gQcomTokenSpaceGuid.PcdMuicI2cSdaGpio|105
  gQcomTokenSpaceGuid.PcdMuicI2cSclGpio|106
  gQcomTokenSpaceGuid.PcdMuicI2cAddress|0x25

!ifdef USB_SELFTEST
  gQcomTokenSpaceGuid.PcdUsbHostSelfTest|TRUE
!endif

[Components.common]
  Msm8916Pkg/Drivers/Ist3038cTouchDxe/Ist3038cTouchDxe.inf
  Msm8916Pkg/Drivers/UsbHostDxe/UsbHostDxe.inf
  Msm8916Pkg/Application/TouchTestApp/TouchTestApp.inf
  Msm8916Pkg/Application/J5DiagApp/J5DiagApp.inf
