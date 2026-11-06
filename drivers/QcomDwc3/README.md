# QcomDwc3

USB device and host mode for Qualcomm DWC3 controllers behind Apple's own stacks on iPadOS 27 (24A446).

`QcomDwc3Xdci` derives from the system's `AppleUSBXDCI` and `QcomDwc3Xhci` from `AppleUSBXHCI`. Each supplies what an Apple SoC subclass supplies: the register window, the interrupt, power and the PHYs. `IOUSBDeviceFamily`, `AppleUSBDeviceMux`, `IOUSBHostFamily`, configd's `USBDeviceConfiguration` plug-in and `lockdownd` run unchanged.

## Hardware

SM8550 with a PM8550B eUSB2 repeater, a QMP USB3/DP PHY and a PS5169 retimer. Sequences follow mainline Linux (`dwc3-qcom.c`, `phy-snps-eusb2.c`, `phy-qcom-eusb2-repeater.c`, `phy-qcom-qmp-combo.c`, `ps5169.c`). `QcomEusb2Phy.h` and `QcomQmpUsb3.h` are GPL-2.0-only; the rest is BSD-2-Clause-Patent.

## Roles

UCSI reports the cable partner: a host selects device mode and a peripheral selects host mode. Until UCSI answers, the charger's online value stands in for a host. One role runs at a time, with a core soft reset between roles.

In host mode the device stack asks for no power. The xHCI subclass keeps the controller on while its ports are idle.

## Interfaces

- `qcom_dwc3_bring_up()`, `qcom_dwc3_publish()`: called by the board's USB probe.
- `qcom_dwc3_vbus(int)`: the charger's online value.
- `qcom_ucsi_start()`, `qcom_ucsi_receive()`, `qcom_ucsi_tick()`, `qcom_ucsi_polling()`: UCSI over the charger's GLINK channel.
- Boot argument `sheng-usb`: 1 runs the driver, 2 adds register sampling, 3 also takes the controller off the bus and back once.
- Boot arguments `sheng-usb-host=<s>` and `sheng-usb-back=<s>`: force host mode after a delay, then return to the cable's role.
- `native_usb_stats`: the `[usb]` rows of the boot log (`log-sys=1`).

## Coherence

`AppleUSBXDCI` does no cache maintenance. Stream 0x40 uses its own stage 1 SMMU context with an identity map of DRAM in write-back inner shareable 1 GiB blocks. The shared bypass bank leaves this master non-coherent.

## State

| State | Features |
| --- | --- |
| Working | Enumeration as `05ac:12ab` at 10 Gbps on a USB 3 cable and 480 Mbps on a USB 2 cable, re-enumeration, Trust dialog, pairing with a Mac, lockdown sessions and service start |
| Partial | Host mode: Apple's xHCI stack runs and completes commands; enumerating a peripheral is not verified |
| Not tested | Sustained transfers, app installation, the reverse cable orientation on USB 3 |
