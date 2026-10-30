# SPDX-License-Identifier: Apache-2.0
#
# Perf-V (PerfXLab / Seeed 102110277) -- Xilinx Artix-7 XC7A35T-1FTG256C
# Pin constraints for the turret motion controller.
#
# Every PACKAGE_PIN below was read off perfv_schematic.pdf (Altium, 10 sheets,
# 2018-05-28) on 2026-10-30, not from Arduino convention. See PLAN.md 3.3.1 for
# the method and the full header tables. PerfXLab shipped no .xdc, so this file
# is the project's own and is the only record of the mapping -- keep it with the
# archived bitstream (R2b).
#
# NOT YET VERIFIED ON HARDWARE. Nothing here has been synthesized.
#
# ===========================================================================
# READ THIS BEFORE CONNECTING A MOTOR  (PLAN.md R3)
# ===========================================================================
# PUDC_B is FPGA pin L15. On this board R49 (pull-up to 3V3) is NOT FITTED and
# R50 (1 kohm to GND) IS fitted, so PUDC_B is held LOW.
#
# PUDC_B low means the Artix-7 enables its INTERNAL PULL-UPS on every SelectIO
# pin for the whole of configuration. The stepper phase pins are therefore not
# floating during configuration -- they are actively pulled HIGH, which is the
# one direction that turns a ULN2003 on.
#
# Consequence: a 10 kohm external pull-down is NOT enough. Against a ~10 kohm
# internal pull-up it divides to ~1.7 V, which is well above the Darlington's
# turn-on. Fit 1 kohm pull-downs at the ULN2003 inputs (after the board's own
# 200 ohm series pack), which holds the node near 0.35 V worst case.
#
# Do not try to fix this by moving R49/R50. The board is discontinued and there
# is no spare.
# ===========================================================================

## ----------------------------------------------------------------- clock
# 50 MHz oscillator. Confirmed twice: the user manual, and net SYS_CLK on N14.
set_property -dict {PACKAGE_PIN N14 IOSTANDARD LVCMOS33} [get_ports clk]
create_clock -period 20.000 -name sys_clk -waveform {0.000 10.000} [get_ports clk]

## ------------------------------------------------------- reset and e-stop
# Board buttons. POLARITY IS UNVERIFIED -- check on the bench at T1 before
# trusting the e-stop, and treat the e-stop as unproven until you have watched
# it halt a real move (PLAN.md D9).
set_property -dict {PACKAGE_PIN L13 IOSTANDARD LVCMOS33} [get_ports rst_n]    ;# net "reset"
set_property -dict {PACKAGE_PIN M15 IOSTANDARD LVCMOS33} [get_ports estop_n]  ;# net KEY1

## --------------------------------------------------- UART to the Artemis
# P2 (Arduino digital header), bank 14. Both lines pass through a 200 ohm
# element of resistor pack Rp13 and an ESD array -- no extra series resistors.
#   P2 D0 / RX1 = net D0/RX1 = B14_L19N = N6
#   P2 D1 / TX1 = net D1/TX1 = B14_L19P = M6
set_property -dict {PACKAGE_PIN N6 IOSTANDARD LVCMOS33} [get_ports uart_rx]
set_property -dict {PACKAGE_PIN M6 IOSTANDARD LVCMOS33 DRIVE 8 SLEW SLOW} [get_ports uart_tx]

## ------------------------------------------- bench UART (J3, with the JTAG)
# A SECOND UART, independent of the Artemis link. Bring T2 up on this one with a
# USB-serial adapter and leave P2 D0/D1 alone; the two can coexist.
#   JTAG1_RX0 = N9, JTAG1_TX0 = P9
set_property -dict {PACKAGE_PIN N9 IOSTANDARD LVCMOS33} [get_ports dbg_rx]
set_property -dict {PACKAGE_PIN P9 IOSTANDARD LVCMOS33 DRIVE 8 SLEW SLOW} [get_ports dbg_tx]

## ------------------------------------------------ pan stepper (ULN2003 IN1-4)
# JP1 (PMOD), pins 1-4, bank 15, each through 200 ohm of pack Rp14.
#   JP1.1 = B15_L13P = E12    JP1.3 = B15_L14P = E11
#   JP1.2 = B15_L13N = E13    JP1.4 = B15_L14N = D11
#
# DRIVE 12 because each line sinks the 1 kohm pull-down (2.75 mA) as well as the
# ULN2003 input (~0.7 mA) when driven high.
#
# PULLDOWN TRUE is NOT the fix for the hazard at the top of this file -- it has
# no effect during configuration. It only covers the short window between DONE
# and the design's first clock. The external 1 kohm resistors are the fix.
set_property -dict {PACKAGE_PIN E12 IOSTANDARD LVCMOS33 DRIVE 12 SLEW SLOW PULLDOWN TRUE} [get_ports {phase[0]}]
set_property -dict {PACKAGE_PIN E13 IOSTANDARD LVCMOS33 DRIVE 12 SLEW SLOW PULLDOWN TRUE} [get_ports {phase[1]}]
set_property -dict {PACKAGE_PIN E11 IOSTANDARD LVCMOS33 DRIVE 12 SLEW SLOW PULLDOWN TRUE} [get_ports {phase[2]}]
set_property -dict {PACKAGE_PIN D11 IOSTANDARD LVCMOS33 DRIVE 12 SLEW SLOW PULLDOWN TRUE} [get_ports {phase[3]}]

## ------------------------------------------------------------- pan limits
# JP1 pins 7 and 8, through pack Rp15. Switches to ground, so the internal
# pull-up is all they need -- no external parts.
#   JP1.7 = B15_L15P = D14    JP1.8 = B15_L15N = D15
set_property -dict {PACKAGE_PIN D14 IOSTANDARD LVCMOS33 PULLUP TRUE} [get_ports lim_cw]
set_property -dict {PACKAGE_PIN D15 IOSTANDARD LVCMOS33 PULLUP TRUE} [get_ports lim_ccw]

## ----------------------------------------------------------- status LEDs
set_property -dict {PACKAGE_PIN M16 IOSTANDARD LVCMOS33} [get_ports {led[0]}]
set_property -dict {PACKAGE_PIN N16 IOSTANDARD LVCMOS33} [get_ports {led[1]}]
set_property -dict {PACKAGE_PIN P15 IOSTANDARD LVCMOS33} [get_ports {led[2]}]
set_property -dict {PACKAGE_PIN P16 IOSTANDARD LVCMOS33} [get_ports {led[3]}]

## ------------------------------------------------------------- bitstream
# Unused pins down rather than up. This does NOT override PUDC_B during
# configuration; it applies after it.
set_property BITSTREAM.CONFIG.UNUSEDPIN PULLDOWN [current_design]
set_property CFGBVS VCCO        [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]

## ---------------------------------------------------------------- spares
# Free on JP1, bank 15, if a tilt axis arrives (PLAN.md O6):
#   JP1.9  = B15_L16P = F12      JP1.10 = B15_L16N = F13
# Free on P2, bank 14 (D4-D7 of the Arduino header):
#   D4 = P13   D5 = N13   D6 = R11   D7 = R10
# Do not use P2 D2/D3 (T8/T7) -- they are the same nets as the dedicated
# SDA/SCL pins, and using them takes I2C away.
