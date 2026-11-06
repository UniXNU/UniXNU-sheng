/* SPDX-License-Identifier: GPL-2.0-only
 * Qualcomm eUSB2 PHY and PMIC repeater bring-up sequences for 19.2 MHz, as in Linux
 * drivers/phy/phy-snps-eusb2.c and drivers/phy/qualcomm/phy-qcom-eusb2-repeater.c. The reference is TCSR_USB2_CLKREF_EN, whose parent
 * RPMH_CXO_CLK is xo_board (76.8 MHz) divided by 2 on SM8550. */
#ifndef QCOM_EUSB2_PHY_H
#define QCOM_EUSB2_PHY_H
#include <stdint.h>
struct QcomMaskedWrite {uint16_t offset; uint8_t mask, value;};
/* Order of qcom_snps_eusb2_hsphy_init(); the last writes drop the override once the PHY runs. */
static const struct QcomMaskedWrite kQcomEusb2Init[] = {
    {0x94, 0x02, 0x02},                 /* CFG0: CMN_CTRL_OVERRIDE_EN */
    {0x50, 0x02, 0x02},                 /* UTMI_CTRL5: POR */
    {0x54, 0x09, 0x09},                 /* HS_PHY_CTRL_COMMON0: PHY_ENABLE, RETENABLEN */
    {0x130, 0x04, 0x04},                /* APB_ACCESS_CMD: APB_LOGIC_RESET */
    {0x98, 0x40, 0x00},                 /* UTMI_PHY_CMN_CTRL0: TESTBURNIN */
    {0xb8, 0x01, 0x01},                 /* FSEL_SEL */
    {0x54, 0x70, 0x40},                 /* FSEL 38.4 MHz */
    {0x5c, 0xff, 0xc8},                 /* CFG_CTRL_2: PLL_FB_DIV_7_0 */
    {0x60, 0x0f, 0x00},                 /* CFG_CTRL_3: PLL_FB_DIV_11_8 */
    {0x60, 0xf0, 0x00},                 /* CFG_CTRL_3: PLL_REF_DIV */
    {0x58, 0xfe, 0x00},                 /* CFG_CTRL_1: PLL_CPBIAS_CNTRL */
    {0x68, 0xfc, 0x20},                 /* CFG_CTRL_4: PLL_INT_CNTRL 8 */
    {0x68, 0x03, 0x01},                 /* CFG_CTRL_4: PLL_GMP_CNTRL */
    {0x6c, 0x1f, 0x10},                 /* CFG_CTRL_5: PLL_PROP_CNTRL */
    {0x70, 0x07, 0x00},                 /* CFG_CTRL_6: PLL_VCO_CNTRL */
    {0x6c, 0xc0, 0x40},                 /* CFG_CTRL_5: PLL_VREF_TUNE */
    {0x64, 0x10, 0x10},                 /* HS_PHY_CTRL2: VBUS_DET_EXT_SEL */
    {0x7c, 0x07, 0x00},                 /* CFG_CTRL_9: TX pre-emphasis */
    {0x7c, 0x60, 0x40},                 /* CFG_CTRL_9: TX rise and fall time */
    {0x7c, 0x18, 0x08},                 /* CFG_CTRL_9: source impedance */
    {0x78, 0x38, 0x18},                 /* CFG_CTRL_8: HS DC voltage */
    {0x78, 0xc0, 0x00},                 /* CFG_CTRL_8: HS crossover */
    {0x64, 0x0c, 0x0c},                 /* HS_PHY_CTRL2: USB2_SUSPEND_N_SEL, USB2_SUSPEND_N */
    {0x3c, 0x01, 0x01},                 /* UTMI_CTRL0: SLEEPM */
    {0x54, 0x02, 0x02},                 /* COMMON0: SIDDQ_SEL */
    {0x54, 0x04, 0x00},                 /* COMMON0: SIDDQ */
    {0x50, 0x02, 0x00},                 /* UTMI_CTRL5: POR */
    {0x64, 0x08, 0x00},                 /* HS_PHY_CTRL2: USB2_SUSPEND_N_SEL */
    {0x94, 0x02, 0x00},                 /* CFG0: CMN_CTRL_OVERRIDE_EN */
};
/* PM8550B repeater, offsets from its peripheral base (SID 7, 0xfd00): enable, then the PMIC tuning table. */
enum {kQcomRepeaterStatus = 0x08, kQcomRepeaterOk = 0x80, kQcomRepeaterEnable = 0x46, kQcomRepeaterEnableBit = 0x80,
      kQcomRepeaterForceEn5 = 0xe8, kQcomRepeaterForceVal5 = 0xed};
static const struct {uint8_t offset, value;} kQcomRepeaterTune[] = {{0x51, 0x08}, {0x54, 0x03}, {0x57, 0x05}};
#endif
