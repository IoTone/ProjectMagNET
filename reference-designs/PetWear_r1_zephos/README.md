# Overview

This is the PetWear prototype for NRF52.

## References

TODO

## Organization

Source follows the strategy for using NRF Connect SDK in VSCode.  Code is laid out according to that project structure.

## Build

TODO: write up

### Flash

TODO

### Test

TODO

## Bootloaders

### MCUBoot

MCUBoot is the bootloader used by the ZephyrOS firmware in the nRF SDK.  It is an optional component we are choosing to use to support DFU (OTA upgrades) and security requirements.
Read more about it here: https://github.com/mcu-tools/mcuboot/blob/main/docs/readme-zephyr.md

For purposes of understanding how it is used an additional section is defined below to descreibe the general memory layout.


### Building and using MCUboot with Zephyr

MCUboot began its life as the bootloader for Mynewt.  It has since
acquired the ability to be used as a bootloader for Zephyr as well.
There are some pretty significant differences in how apps are built
for Zephyr, and these are documented here.

Please see the [design document](design.md) for documentation on the design
and operation of the bootloader itself. This functionality should be the same
on all supported RTOSs.

The first step required for Zephyr is making sure your board has flash
partitions defined in its device tree. These partitions are:

- `boot_partition`: for MCUboot itself
- `slot0_partition`: the primary slot of Image 0
- `slot1_partition`: the secondary slot of Image 0

It is not recommended to use the swap-using-scratch algorithm of MCUboot, but
if this operating mode is desired then the following flash partition is also
needed (see end of this help file for details on creating a scratch partition
and how to use the swap-using-scratch algorithm):

- `scratch_partition`: the scratch slot

Currently, the two image slots must be contiguous. If you are running
MCUboot as your stage 1 bootloader, `boot_partition` must be configured
so your SoC runs it out of reset. If there are multiple updateable images
then the corresponding primary and secondary partitions must be defined for
the rest of the images too (for example, `slot2_partition` and
`slot3_partition` for Image 1).

The flash partitions are typically defined in the Zephyr boards folder, in a
file named `boards/<arch>/<board>/<board>.dts`. An example `.dts` file with
flash partitions defined is the frdm_k64f's in
`boards/arm/frdm_k64f/frdm_k64f.dts`. Make sure the DT node labels in your board's
`.dts` file match the ones used there.


## nRF Soft Device

Nordic Semiconductor protocol stacks are known as SoftDevices. SoftDevices are pre-compiled, pre-
linked binary files. SoftDevices can be programmed in nRF52 series SoCs and are downloadable from
the Nordic Semiconductor website. The BMD-300 with the nRF52832 SoC supports the S132
(Bluetooth low energy Central & Peripheral), S212 (ANT) and S312 (ANT and Bluetooth low energy)
SoftDevices.

Section 4.3 of https://content.u-blox.com/sites/default/files/BMD-300_DataSheet_UBX-19033350.pdf

describes the soft device options.  

## XIAO nrf52 BLE_SENSE Pin Mapping

Based on Revision 3 of the OTA Daughterboard, this is the current pin mapping.

It **must** be modified to match the schematic for any new designs down the road

TODO: update this to pin map here: https://wiki.seeedstudio.com/XIAO_BLE/


| GPIO PIN  | Pin#  | Default Assignment  | New Assignment  | TYPE/Conf (I/O)  |
|---|---|---|---|---|
| P0.00  | 13  | -  | -  | - |
| P0.01  | 14  | -  | -  | -  |
| P0.02 | 15  | ?  | EEPROM CS  | OUTPUT  |
| P0.03 | 19  |  ? | EEPROM SCK  | OUTPUT  |
| P0.04 | 20  | ?  | EEPROM MOSI  | OUTPUT  |
| P0.05 | 21  | UART0 RTS  | -  | -  |
| P0.06 | 22  | UART0 TX  | BLE to X (??Tx)  | OUTPUT  |
| P0.07 | 23  | UART0 CTS | -  | =  |
| P0.08 | 24  | UART0 RX  | X to BLE (??Rx)  | INPUT  |
| P0.09 | 25  | ?  | MISO  | INPUT/OUTPUT  |
| P0.10 | 26  | ?  | -  | -  |
| P0.11 | 27  | ?  | -  | -  |
| P0.12 | 28  | ?  | -  | -  |
| P0.13 | 31  | ?  | -  | -  |
| P0.14 | 32  | ?  | -  | -  |
| P0.15 | 33  | ?  | -  | -  |
| P0.16 | 34  | ?  | -  | -  |
| P0.17 | 35  | LED0  | SWO  | I/O  |
| P0.18 | 36  | LED1  | -  | -  |
| P0.19 | 37  | LED2  |   |   |
| P0.20 | 38  |   |   |   |
| P0.21 | 39  | ?  | RESET BLE  | INPUT  |
| P0.22 | 40  | ?  | -  | -  |
| P0.23 | 41  | SPI2 MOSI | -  | -  |
| P0.24 | 42  | SPI2 MISO | MCLR1  | I/O  |
| P0.25 | 6  | SPI2 SCK | -  | -  |
| P0.26 | 7  | I2C0 SCA  | -  | -  |
| P0.27 | 8  | I2C0 SCL  |  LED0 | -  |
| P0.28 | 9  | ?   |  - | -  |
| P0.29 | 10  | SPI1 MISO  |  - | -  |
| P0.30 | 11  | SPI1 MOSI  |  - | -  |
| P0.31 | 12  | SPI1 SCK  |  - | -  |

Issues noted above
TBD

# Manufacturing

We intend to make board files available, and design for manufacturing.

## Configuration

The current design involves some planning around programming:
- serial number
- default BLE advertisement

These are likely to be stored in FLASH or in the UICR area of the nrf chip: https://infocenter.nordicsemi.com/topic/drivers_nrfx_v3.3.0/group__nrf__uicr__hal.html

There are additonal predefined configurations that we will not likely define ourselves  See : https://content.u-blox.com/sites/default/files/BMD-300_DataSheet_UBX-19033350.pdf
Section 4.4
- OUI BLE Address

## Markings/Labeling

TODO: look up the labelling for XIAO chips

The following additional markings/labels will be provided as a part of the manufacturing process:
- Serial # TODO define the sequence
- QR Code: TODO define the encoding

## Tools

- The MagNET App for iOS/Android will serve as the manufacturing tool
- nRF Connect iOS/Android will be used for testing DFU upgrades
- nrfutil will be used for bench based programming (as part of nRFCOnnect SDK as well)
