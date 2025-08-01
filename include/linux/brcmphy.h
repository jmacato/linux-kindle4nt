/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_BRCMPHY_H
#define _LINUX_BRCMPHY_H

#include <linux/phy.h>

/* All Broadcom Ethernet switches have a pseudo-PHY at address 30 which is used
 * to configure the switch internal registers via MDIO accesses.
 */
#define BRCM_PSEUDO_PHY_ADDR           30

#define PHY_ID_BCM50610			0x0143bd60
#define PHY_ID_BCM50610M		0x0143bd70
#define PHY_ID_BCM5221			0x004061e0
#define PHY_ID_BCM5241			0x0143bc30
#define PHY_ID_BCMAC131			0x0143bc70
#define PHY_ID_BCM5481			0x0143bca0
#define PHY_ID_BCM5395			0x0143bcf0
#define PHY_ID_BCM53125			0x03625f20
#define PHY_ID_BCM53128			0x03625e10
#define PHY_ID_BCM54810			0x03625d00
#define PHY_ID_BCM54811			0x03625cc0
#define PHY_ID_BCM5482			0x0143bcb0
#define PHY_ID_BCM5411			0x00206070
#define PHY_ID_BCM5421			0x002060e0
#define PHY_ID_BCM54210E		0x600d84a0
#define PHY_ID_BCM5464			0x002060b0
#define PHY_ID_BCM5461			0x002060c0
#define PHY_ID_BCM54612E		0x03625e60
#define PHY_ID_BCM54616S		0x03625d10
#define PHY_ID_BCM54140			0xae025009
#define PHY_ID_BCM57780			0x03625d90
#define PHY_ID_BCM89610			0x03625cd0

#define PHY_ID_BCM72113			0x35905310
#define PHY_ID_BCM72116			0x35905350
#define PHY_ID_BCM72165			0x35905340
#define PHY_ID_BCM7250			0xae025280
#define PHY_ID_BCM7255			0xae025120
#define PHY_ID_BCM7260			0xae025190
#define PHY_ID_BCM7268			0xae025090
#define PHY_ID_BCM7271			0xae0253b0
#define PHY_ID_BCM7278			0xae0251a0
#define PHY_ID_BCM7364			0xae025260
#define PHY_ID_BCM7366			0x600d8490
#define PHY_ID_BCM7346			0x600d8650
#define PHY_ID_BCM7362			0x600d84b0
#define PHY_ID_BCM74165			0x359052c0
#define PHY_ID_BCM7425			0x600d86b0
#define PHY_ID_BCM7429			0x600d8730
#define PHY_ID_BCM7435			0x600d8750
#define PHY_ID_BCM74371			0xae0252e0
#define PHY_ID_BCM7439			0x600d8480
#define PHY_ID_BCM7439_2		0xae025080
#define PHY_ID_BCM7445			0x600d8510
#define PHY_ID_BCM7712			0x35905330

#define PHY_ID_BCM_CYGNUS		0xae025200
#define PHY_ID_BCM_OMEGA		0xae025100

#define PHY_BCM_OUI_MASK		0xfffffc00
#define PHY_BCM_OUI_1			0x00206000
#define PHY_BCM_OUI_2			0x0143bc00
#define PHY_BCM_OUI_3			0x03625c00
#define PHY_BCM_OUI_4			0x600d8400
#define PHY_BCM_OUI_5			0x03625e00
#define PHY_BCM_OUI_6			0xae025000

#define PHY_BRCM_AUTO_PWRDWN_ENABLE	0x00000001
#define PHY_BRCM_RX_REFCLK_UNUSED	0x00000002
#define PHY_BRCM_CLEAR_RGMII_MODE	0x00000004
#define PHY_BRCM_DIS_TXCRXC_NOENRGY	0x00000008
#define PHY_BRCM_EN_MASTER_MODE		0x00000010
#define PHY_BRCM_IDDQ_SUSPEND		0x00000020

/* Broadcom BCM7xxx specific workarounds */
#define PHY_BRCM_7XXX_REV(x)		(((x) >> 8) & 0xff)
#define PHY_BRCM_7XXX_PATCH(x)		((x) & 0xff)
#define PHY_BCM_FLAGS_VALID		0x80000000

/* Broadcom BCM54XX register definitions, common to most Broadcom PHYs */
#define MII_BCM54XX_ECR		0x10	/* BCM54xx extended control register */
#define MII_BCM54XX_ECR_IM	0x1000	/* Interrupt mask */
#define MII_BCM54XX_ECR_IF	0x0800	/* Interrupt force */
#define MII_BCM54XX_ECR_FIFOE	0x0001	/* FIFO elasticity */

#define MII_BCM54XX_ESR		0x11	/* BCM54xx extended status register */
#define MII_BCM54XX_ESR_IS	0x1000	/* Interrupt status */

#define MII_BCM54XX_EXP_DATA	0x15	/* Expansion register data */
#define MII_BCM54XX_EXP_SEL	0x17	/* Expansion register select */
#define MII_BCM54XX_EXP_SEL_TOP	0x0d00	/* TOP_MISC expansion register select */
#define MII_BCM54XX_EXP_SEL_SSD	0x0e00	/* Secondary SerDes select */
#define MII_BCM54XX_EXP_SEL_WOL	0x0e00	/* Wake-on-LAN expansion select register */
#define MII_BCM54XX_EXP_SEL_ER	0x0f00	/* Expansion register select */
#define MII_BCM54XX_EXP_SEL_ETC	0x0d00	/* Expansion register spare + 2k mem */

#define MII_BCM54XX_AUX_CTL	0x18	/* Auxiliary control register */
#define MII_BCM54XX_ISR		0x1a	/* BCM54xx interrupt status register */
#define MII_BCM54XX_IMR		0x1b	/* BCM54xx interrupt mask register */
#define MII_BCM54XX_INT_CRCERR	0x0001	/* CRC error */
#define MII_BCM54XX_INT_LINK	0x0002	/* Link status changed */
#define MII_BCM54XX_INT_SPEED	0x0004	/* Link speed change */
#define MII_BCM54XX_INT_DUPLEX	0x0008	/* Duplex mode changed */
#define MII_BCM54XX_INT_LRS	0x0010	/* Local receiver status changed */
#define MII_BCM54XX_INT_RRS	0x0020	/* Remote receiver status changed */
#define MII_BCM54XX_INT_SSERR	0x0040	/* Scrambler synchronization error */
#define MII_BCM54XX_INT_UHCD	0x0080	/* Unsupported HCD negotiated */
#define MII_BCM54XX_INT_NHCD	0x0100	/* No HCD */
#define MII_BCM54XX_INT_NHCDL	0x0200	/* No HCD link */
#define MII_BCM54XX_INT_ANPR	0x0400	/* Auto-negotiation page received */
#define MII_BCM54XX_INT_LC	0x0800	/* All counters below 128 */
#define MII_BCM54XX_INT_HC	0x1000	/* Counter above 32768 */
#define MII_BCM54XX_INT_MDIX	0x2000	/* MDIX status change */
#define MII_BCM54XX_INT_PSERR	0x4000	/* Pair swap error */

#define MII_BCM54XX_SHD		0x1c	/* 0x1c shadow registers */
#define MII_BCM54XX_SHD_WRITE	0x8000
#define MII_BCM54XX_SHD_VAL(x)	((x & 0x1f) << 10)
#define MII_BCM54XX_SHD_DATA(x)	((x & 0x3ff) << 0)

#define MII_BCM54XX_RDB_ADDR	0x1e
#define MII_BCM54XX_RDB_DATA	0x1f

/* legacy access control via rdb/expansion register */
#define BCM54XX_RDB_REG0087		0x0087
#define BCM54XX_EXP_REG7E		(MII_BCM54XX_EXP_SEL_ER + 0x7E)
#define BCM54XX_ACCESS_MODE_LEGACY_EN	BIT(15)

/*
 * AUXILIARY CONTROL SHADOW ACCESS REGISTERS.  (PHY REG 0x18)
 */
#define MII_BCM54XX_AUXCTL_SHDWSEL_AUXCTL	0x00
#define MII_BCM54XX_AUXCTL_ACTL_TX_6DB		0x0400
#define MII_BCM54XX_AUXCTL_ACTL_SMDSP_ENA	0x0800
#define MII_BCM54XX_AUXCTL_ACTL_EXT_PKT_LEN	0x4000

#define MII_BCM54XX_AUXCTL_SHDWSEL_MISC			0x07
#define MII_BCM54XX_AUXCTL_SHDWSEL_MISC_WIRESPEED_EN	0x0010
#define MII_BCM54XX_AUXCTL_SHDWSEL_MISC_RGMII_EN	0x0080
#define MII_BCM54XX_AUXCTL_SHDWSEL_MISC_RGMII_SKEW_EN	0x0100
#define MII_BCM54XX_AUXCTL_MISC_FORCE_AMDIX		0x0200
#define MII_BCM54XX_AUXCTL_MISC_WREN			0x8000

#define MII_BCM54XX_AUXCTL_SHDWSEL_READ_SHIFT	12
#define MII_BCM54XX_AUXCTL_SHDWSEL_MASK	0x0007

/*
 * Broadcom LED source encodings.  These are used in BCM5461, BCM5481,
 * BCM5482, and possibly some others.
 */
#define BCM_LED_SRC_LINKSPD1	0x0
#define BCM_LED_SRC_LINKSPD2	0x1
#define BCM_LED_SRC_XMITLED	0x2
#define BCM_LED_SRC_ACTIVITYLED	0x3
#define BCM_LED_SRC_FDXLED	0x4
#define BCM_LED_SRC_SLAVE	0x5
#define BCM_LED_SRC_INTR	0x6
#define BCM_LED_SRC_QUALITY	0x7
#define BCM_LED_SRC_RCVLED	0x8
#define BCM_LED_SRC_WIRESPEED	0x9
#define BCM_LED_SRC_MULTICOLOR1	0xa
#define BCM_LED_SRC_OPENSHORT	0xb
#define BCM_LED_SRC_OFF		0xe	/* Tied high */
#define BCM_LED_SRC_ON		0xf	/* Tied low */
#define BCM_LED_SRC_MASK	GENMASK(3, 0)

/*
 * Broadcom Multicolor LED configurations (expansion register 4)
 */
#define BCM_EXP_MULTICOLOR		(MII_BCM54XX_EXP_SEL_ER + 0x04)
#define BCM_LED_MULTICOLOR_IN_PHASE	BIT(8)
#define BCM_LED_MULTICOLOR_LINK_ACT	0x0
#define BCM_LED_MULTICOLOR_SPEED	0x1
#define BCM_LED_MULTICOLOR_ACT_FLASH	0x2
#define BCM_LED_MULTICOLOR_FDX		0x3
#define BCM_LED_MULTICOLOR_OFF		0x4
#define BCM_LED_MULTICOLOR_ON		0x5
#define BCM_LED_MULTICOLOR_ALT		0x6
#define BCM_LED_MULTICOLOR_FLASH	0x7
#define BCM_LED_MULTICOLOR_LINK		0x8
#define BCM_LED_MULTICOLOR_ACT		0x9
#define BCM_LED_MULTICOLOR_PROGRAM	0xa

/*
 * Broadcom Synchronous Ethernet Controls (expansion register 0x0E)
 */
#define BCM_EXP_SYNC_ETHERNET		(MII_BCM54XX_EXP_SEL_ER + 0x0E)
#define BCM_EXP_SYNC_ETHERNET_MII_LITE	BIT(11)

/*
 * BCM5482: Shadow registers
 * Shadow values go into bits [14:10] of register 0x1c to select a shadow
 * register to access.
 */

/* 00100: Reserved control register 2 */
#define BCM54XX_SHD_SCR2		0x04
#define  BCM54XX_SHD_SCR2_WSPD_RTRY_DIS	0x100
#define  BCM54XX_SHD_SCR2_WSPD_RTRY_LMT_SHIFT	2
#define  BCM54XX_SHD_SCR2_WSPD_RTRY_LMT_OFFSET	2
#define  BCM54XX_SHD_SCR2_WSPD_RTRY_LMT_MASK	0x7

/* 00101: Spare Control Register 3 */
#define BCM54XX_SHD_SCR3		0x05
#define  BCM54XX_SHD_SCR3_DEF_CLK125	0x0001
#define  BCM54XX_SHD_SCR3_DLLAPD_DIS	0x0002
#define  BCM54XX_SHD_SCR3_TRDDAPD	0x0004
#define  BCM54XX_SHD_SCR3_RXCTXC_DIS	0x0100

/* 01010: Auto Power-Down */
#define BCM54XX_SHD_APD			0x0a
#define  BCM_APD_CLR_MASK		0xFE9F /* clear bits 5, 6 & 8 */
#define  BCM54XX_SHD_APD_EN		0x0020
#define  BCM_NO_ANEG_APD_EN		0x0060 /* bits 5 & 6 */
#define  BCM_APD_SINGLELP_EN	0x0100 /* Bit 8 */

#define BCM54XX_SHD_LEDS1	0x0d	/* 01101: LED Selector 1 */
					/* LED3 / ~LINKSPD[2] selector */
#define BCM54XX_SHD_LEDS_SHIFT(led)	(4 * (led))
#define BCM54XX_SHD_LEDS1_LED3(src)	((src & 0xf) << 4)
					/* LED1 / ~LINKSPD[1] selector */
#define BCM54XX_SHD_LEDS1_LED1(src)	((src & 0xf) << 0)
#define BCM54XX_SHD_LEDS2	0x0e	/* 01110: LED Selector 2 */
#define BCM54XX_SHD_RGMII_MODE	0x0b	/* 01011: RGMII Mode Selector */
#define BCM5482_SHD_SSD		0x14	/* 10100: Secondary SerDes control */
#define BCM5482_SHD_SSD_LEDM	0x0008	/* SSD LED Mode enable */
#define BCM5482_SHD_SSD_EN	0x0001	/* SSD enable */

/* 10011: SerDes 100-FX Control Register */
#define BCM54616S_SHD_100FX_CTRL	0x13
#define	BCM54616S_100FX_MODE		BIT(0)	/* 100-FX SerDes Enable */

/* 11111: Mode Control Register */
#define BCM54XX_SHD_MODE		0x1f
#define BCM54XX_SHD_INTF_SEL_MASK	GENMASK(2, 1)	/* INTERF_SEL[1:0] */
#define BCM54XX_SHD_INTF_SEL_RGMII	0x02
#define BCM54XX_SHD_INTF_SEL_SGMII	0x04
#define BCM54XX_SHD_INTF_SEL_GBIC	0x06
#define BCM54XX_SHD_MODE_1000BX		BIT(0)	/* Enable 1000-X registers */

/*
 * EXPANSION SHADOW ACCESS REGISTERS.  (PHY REG 0x15, 0x16, and 0x17)
 */
#define MII_BCM54XX_EXP_AADJ1CH0		0x001f
#define  MII_BCM54XX_EXP_AADJ1CH0_SWP_ABCD_OEN	0x0200
#define  MII_BCM54XX_EXP_AADJ1CH0_SWSEL_THPF	0x0100
#define MII_BCM54XX_EXP_AADJ1CH3		0x601f
#define  MII_BCM54XX_EXP_AADJ1CH3_ADCCKADJ	0x0002
#define MII_BCM54XX_EXP_EXP08			0x0F08
#define  MII_BCM54XX_EXP_EXP08_RJCT_2MHZ	0x0001
#define  MII_BCM54XX_EXP_EXP08_EARLY_DAC_WAKE	0x0200
#define  MII_BCM54XX_EXP_EXP08_FORCE_DAC_WAKE	0x0100
#define MII_BCM54XX_EXP_EXP75			0x0f75
#define  MII_BCM54XX_EXP_EXP75_VDACCTRL		0x003c
#define  MII_BCM54XX_EXP_EXP75_CM_OSC		0x0001
#define MII_BCM54XX_EXP_EXP96			0x0f96
#define  MII_BCM54XX_EXP_EXP96_MYST		0x0010
#define MII_BCM54XX_EXP_EXP97			0x0f97
#define  MII_BCM54XX_EXP_EXP97_MYST		0x0c0c

/* Top-MISC expansion registers */
#define BCM54XX_TOP_MISC_IDDQ_CTRL		(MII_BCM54XX_EXP_SEL_TOP + 0x06)
#define BCM54XX_TOP_MISC_IDDQ_LP		(1 << 0)
#define BCM54XX_TOP_MISC_IDDQ_SD		(1 << 2)
#define BCM54XX_TOP_MISC_IDDQ_SR		(1 << 3)

#define BCM54XX_TOP_MISC_LED_CTL		(MII_BCM54XX_EXP_SEL_TOP + 0x0C)
#define  BCM54XX_LED4_SEL_INTR			BIT(1)

/*
 * BCM5482: Secondary SerDes registers
 */
#define BCM5482_SSD_1000BX_CTL		0x00	/* 1000BASE-X Control */
#define BCM5482_SSD_1000BX_CTL_PWRDOWN	0x0800	/* Power-down SSD */
#define BCM5482_SSD_SGMII_SLAVE		0x15	/* SGMII Slave Register */
#define BCM5482_SSD_SGMII_SLAVE_EN	0x0002	/* Slave mode enable */
#define BCM5482_SSD_SGMII_SLAVE_AD	0x0001	/* Slave auto-detection */

/* BroadR-Reach LRE Registers. */
#define MII_BCM54XX_LRECR		0x00	/* LRE Control Register                    */
#define MII_BCM54XX_LRESR		0x01	/* LRE Status Register                     */
#define MII_BCM54XX_LREPHYSID1		0x02	/* LRE PHYS ID 1                           */
#define MII_BCM54XX_LREPHYSID2		0x03	/* LRE PHYS ID 2                           */
#define MII_BCM54XX_LREANAA		0x04	/* LDS Auto-Negotiation Advertised Ability */
#define MII_BCM54XX_LREANAC		0x05	/* LDS Auto-Negotiation Advertised Control */
#define MII_BCM54XX_LREANPT		0x06	/* LDS Ability Next Page Transmit          */
#define MII_BCM54XX_LRELPA		0x07	/* LDS Link Partner Ability                */
#define MII_BCM54XX_LRELPNPM		0x08	/* LDS Link Partner Next Page Message      */
#define MII_BCM54XX_LRELPNPC		0x09	/* LDS Link Partner Next Page Control      */
#define MII_BCM54XX_LRELDSE		0x0a	/* LDS Expansion Register                  */
#define MII_BCM54XX_LREES		0x0f	/* LRE Extended Status                     */

/* LRE control register. */
#define LRECR_RESET			0x8000	/* Reset to default state      */
#define LRECR_LOOPBACK			0x4000	/* Internal Loopback           */
#define LRECR_LDSRES			0x2000	/* Restart LDS Process         */
#define LRECR_LDSEN			0x1000	/* LDS Enable                  */
#define LRECR_PDOWN			0x0800	/* Enable low power state      */
#define LRECR_ISOLATE			0x0400	/* Isolate data paths from MII */
#define LRECR_SPEED100			0x0200	/* Select 100 Mbps             */
#define LRECR_SPEED10			0x0000	/* Select 10 Mbps              */
#define LRECR_4PAIRS			0x0020	/* Select 4 Pairs              */
#define LRECR_2PAIRS			0x0010	/* Select 2 Pairs              */
#define LRECR_1PAIR			0x0000	/* Select 1 Pair               */
#define LRECR_MASTER			0x0008	/* Force Master when LDS disabled */
#define LRECR_SLAVE			0x0000	/* Force Slave when LDS disabled  */

/* LRE status register. */
#define LRESR_100_1PAIR			0x2000	/* Can do 100Mbps 1 Pair       */
#define LRESR_100_4PAIR			0x1000	/* Can do 100Mbps 4 Pairs      */
#define LRESR_100_2PAIR			0x0800	/* Can do 100Mbps 2 Pairs      */
#define LRESR_10_2PAIR			0x0400	/* Can do 10Mbps 2 Pairs       */
#define LRESR_10_1PAIR			0x0200	/* Can do 10Mbps 1 Pair        */
#define LRESR_ESTATEN			0x0100	/* Extended Status in R15      */
#define LRESR_RESV			0x0080	/* Unused...                   */
#define LRESR_MFPS			0x0040	/* Can suppress Management Frames Preamble */
#define LRESR_LDSCOMPLETE		0x0020	/* LDS Auto-negotiation complete */
#define LRESR_8023			0x0010	/* Has IEEE 802.3 Support      */
#define LRESR_LDSABILITY		0x0008	/* LDS auto-negotiation capable */
#define LRESR_LSTATUS			0x0004	/* Link status                 */
#define LRESR_JCD			0x0002	/* Jabber detected             */
#define LRESR_ERCAP			0x0001	/* Ext-reg capability          */

/* LDS Auto-Negotiation Advertised Ability. */
#define LREANAA_PAUSE_ASYM		0x8000	/* Can pause asymmetrically    */
#define LREANAA_PAUSE			0x4000	/* Can pause                   */
#define LREANAA_100_1PAIR		0x0020	/* Can do 100Mbps 1 Pair       */
#define LREANAA_100_4PAIR		0x0010	/* Can do 100Mbps 4 Pair       */
#define LREANAA_100_2PAIR		0x0008	/* Can do 100Mbps 2 Pair       */
#define LREANAA_10_2PAIR		0x0004	/* Can do 10Mbps 2 Pair        */
#define LREANAA_10_1PAIR		0x0002	/* Can do 10Mbps 1 Pair        */

#define LRE_ADVERTISE_FULL		(LREANAA_100_1PAIR | LREANAA_100_4PAIR | \
					 LREANAA_100_2PAIR | LREANAA_10_2PAIR | \
					 LREANAA_10_1PAIR)

#define LRE_ADVERTISE_ALL		LRE_ADVERTISE_FULL

/* LDS Link Partner Ability. */
#define LRELPA_PAUSE_ASYM		0x8000	/* Supports asymmetric pause   */
#define LRELPA_PAUSE			0x4000	/* Supports pause capability   */
#define LRELPA_100_1PAIR		0x0020	/* 100Mbps 1 Pair capable      */
#define LRELPA_100_4PAIR		0x0010	/* 100Mbps 4 Pair capable      */
#define LRELPA_100_2PAIR		0x0008	/* 100Mbps 2 Pair capable      */
#define LRELPA_10_2PAIR			0x0004	/* 10Mbps 2 Pair capable       */
#define LRELPA_10_1PAIR			0x0002	/* 10Mbps 1 Pair capable       */

/* LDS Expansion register. */
#define LDSE_DOWNGRADE			0x8000	/* Can do LDS Speed Downgrade  */
#define LDSE_MASTER			0x4000	/* Master / Slave              */
#define LDSE_PAIRS_MASK			0x3000	/* Pair Count Mask             */
#define LDSE_PAIRS_SHIFT		12
#define LDSE_4PAIRS			(2 << LDSE_PAIRS_SHIFT)	/* 4 Pairs Connection */
#define LDSE_2PAIRS			(1 << LDSE_PAIRS_SHIFT)	/* 2 Pairs Connection */
#define LDSE_1PAIR			(0 << LDSE_PAIRS_SHIFT)	/* 1 Pair  Connection */
#define LDSE_CABLEN_MASK		0x0FFF	/* Cable Length Mask           */

/* BCM54810 Registers */
#define BCM54810_EXP_BROADREACH_LRE_MISC_CTL	(MII_BCM54XX_EXP_SEL_ER + 0x90)
#define BCM54810_EXP_BROADREACH_LRE_MISC_CTL_EN	(1 << 0)
#define BCM54810_SHD_CLK_CTL			0x3
#define BCM54810_SHD_CLK_CTL_GTXCLK_EN		(1 << 9)

/* BCM54811 Registers */
#define BCM54811_EXP_BROADREACH_LRE_OVERLAY_CTL	(MII_BCM54XX_EXP_SEL_ER + 0x9A)
/* Access Control Override Enable */
#define BCM54811_EXP_BROADREACH_LRE_OVERLAY_CTL_EN		BIT(15)
/* Access Control Override Value */
#define BCM54811_EXP_BROADREACH_LRE_OVERLAY_CTL_OVERRIDE_VAL	BIT(14)
/* Access Control Value */
#define BCM54811_EXP_BROADREACH_LRE_OVERLAY_CTL_VAL		BIT(13)

/* BCM54612E Registers */
#define BCM54612E_EXP_SPARE0		(MII_BCM54XX_EXP_SEL_ETC + 0x34)
#define BCM54612E_LED4_CLK125OUT_EN	(1 << 1)


/* Wake-on-LAN registers */
#define BCM54XX_WOL_MAIN_CTL		(MII_BCM54XX_EXP_SEL_WOL + 0x80)
#define  BCM54XX_WOL_EN			BIT(0)
#define  BCM54XX_WOL_MODE_SINGLE_MPD	0
#define  BCM54XX_WOL_MODE_SINGLE_MPDSEC	1
#define  BCM54XX_WOL_MODE_DUAL		2
#define  BCM54XX_WOL_MODE_SHIFT		1
#define  BCM54XX_WOL_MODE_MASK		0x3
#define  BCM54XX_WOL_MP_MSB_FF_EN	BIT(3)
#define  BCM54XX_WOL_SECKEY_OPT_4B	0
#define  BCM54XX_WOL_SECKEY_OPT_6B	1
#define  BCM54XX_WOL_SECKEY_OPT_8B	2
#define  BCM54XX_WOL_SECKEY_OPT_SHIFT	4
#define  BCM54XX_WOL_SECKEY_OPT_MASK	0x3
#define  BCM54XX_WOL_L2_TYPE_CHK	BIT(6)
#define  BCM54XX_WOL_L4IPV4UDP_CHK	BIT(7)
#define  BCM54XX_WOL_L4IPV6UDP_CHK	BIT(8)
#define  BCM54XX_WOL_UDPPORT_CHK	BIT(9)
#define  BCM54XX_WOL_CRC_CHK		BIT(10)
#define  BCM54XX_WOL_SECKEY_MODE	BIT(11)
#define  BCM54XX_WOL_RST		BIT(12)
#define  BCM54XX_WOL_DIR_PKT_EN		BIT(13)
#define  BCM54XX_WOL_MASK_MODE_DA_FF	0
#define  BCM54XX_WOL_MASK_MODE_DA_MPD	1
#define  BCM54XX_WOL_MASK_MODE_DA_ONLY	2
#define  BCM54XX_WOL_MASK_MODE_MPD	3
#define  BCM54XX_WOL_MASK_MODE_SHIFT	14
#define  BCM54XX_WOL_MASK_MODE_MASK	0x3

#define BCM54XX_WOL_INNER_PROTO		(MII_BCM54XX_EXP_SEL_WOL + 0x81)
#define BCM54XX_WOL_OUTER_PROTO		(MII_BCM54XX_EXP_SEL_WOL + 0x82)
#define BCM54XX_WOL_OUTER_PROTO2	(MII_BCM54XX_EXP_SEL_WOL + 0x83)

#define BCM54XX_WOL_MPD_DATA1(x)	(MII_BCM54XX_EXP_SEL_WOL + 0x84 + (x))
#define BCM54XX_WOL_MPD_DATA2(x)	(MII_BCM54XX_EXP_SEL_WOL + 0x87 + (x))
#define BCM54XX_WOL_SEC_KEY_8B		(MII_BCM54XX_EXP_SEL_WOL + 0x8A)
#define BCM54XX_WOL_MASK(x)		(MII_BCM54XX_EXP_SEL_WOL + 0x8B + (x))
#define BCM54XX_SEC_KEY_STORE(x)	(MII_BCM54XX_EXP_SEL_WOL + 0x8E)
#define BCM54XX_WOL_SHARED_CNT		(MII_BCM54XX_EXP_SEL_WOL + 0x92)

#define BCM54XX_WOL_INT_MASK		(MII_BCM54XX_EXP_SEL_WOL + 0x93)
#define  BCM54XX_WOL_PKT1		BIT(0)
#define  BCM54XX_WOL_PKT2		BIT(1)
#define  BCM54XX_WOL_DIR		BIT(2)
#define  BCM54XX_WOL_ALL_INTRS		(BCM54XX_WOL_PKT1 | \
					 BCM54XX_WOL_PKT2 | \
					 BCM54XX_WOL_DIR)

#define BCM54XX_WOL_INT_STATUS		(MII_BCM54XX_EXP_SEL_WOL + 0x94)

/* BCM5221 Registers */
#define BCM5221_AEGSR			0x1C
#define BCM5221_AEGSR_MDIX_STATUS	BIT(13)
#define BCM5221_AEGSR_MDIX_MAN_SWAP	BIT(12)
#define BCM5221_AEGSR_MDIX_DIS		BIT(11)

#define BCM5221_SHDW_AM4_EN_CLK_LPM	BIT(2)
#define BCM5221_SHDW_AM4_FORCE_LPM	BIT(1)

/*****************************************************************************/
/* Fast Ethernet Transceiver definitions. */
/*****************************************************************************/

#define MII_BRCM_FET_INTREG		0x1a	/* Interrupt register */
#define MII_BRCM_FET_IR_MASK		0x0100	/* Mask all interrupts */
#define MII_BRCM_FET_IR_LINK_EN		0x0200	/* Link status change enable */
#define MII_BRCM_FET_IR_SPEED_EN	0x0400	/* Link speed change enable */
#define MII_BRCM_FET_IR_DUPLEX_EN	0x0800	/* Duplex mode change enable */
#define MII_BRCM_FET_IR_ENABLE		0x4000	/* Interrupt enable */

#define MII_BRCM_FET_BRCMTEST		0x1f	/* Brcm test register */
#define MII_BRCM_FET_BT_SRE		0x0080	/* Shadow register enable */


/*** Shadow register definitions ***/

#define MII_BRCM_FET_SHDW_MISCCTRL	0x10	/* Shadow misc ctrl */
#define MII_BRCM_FET_SHDW_MC_FAME	0x4000	/* Force Auto MDIX enable */

#define MII_BRCM_FET_SHDW_AUXMODE4	0x1a	/* Auxiliary mode 4 */
#define MII_BRCM_FET_SHDW_AM4_STANDBY	0x0008	/* Standby enable */
#define MII_BRCM_FET_SHDW_AM4_LED_MASK	0x0003
#define MII_BRCM_FET_SHDW_AM4_LED_MODE1 0x0001

#define MII_BRCM_FET_SHDW_AUXSTAT2	0x1b	/* Auxiliary status 2 */
#define MII_BRCM_FET_SHDW_AS2_APDE	0x0020	/* Auto power down enable */

#define BRCM_CL45VEN_EEE_CONTROL	0x803d
#define LPI_FEATURE_EN			0x8000
#define LPI_FEATURE_EN_DIG1000X		0x4000

#define BRCM_CL45VEN_EEE_LPI_CNT	0x803f

/* Core register definitions*/
#define MII_BRCM_CORE_BASE12	0x12
#define MII_BRCM_CORE_BASE13	0x13
#define MII_BRCM_CORE_BASE14	0x14
#define MII_BRCM_CORE_BASE1E	0x1E
#define MII_BRCM_CORE_EXPB0	0xB0
#define MII_BRCM_CORE_EXPB1	0xB1

/* Enhanced Cable Diagnostics */
#define BCM54XX_RDB_ECD_CTRL			0x2a0
#define BCM54XX_EXP_ECD_CTRL			(MII_BCM54XX_EXP_SEL_ER + 0xc0)

#define BCM54XX_ECD_CTRL_CABLE_TYPE_CAT3	1	/* CAT3 or worse */
#define BCM54XX_ECD_CTRL_CABLE_TYPE_CAT5	0	/* CAT5 or better */
#define BCM54XX_ECD_CTRL_CABLE_TYPE_MASK	BIT(0)	/* cable type */
#define BCM54XX_ECD_CTRL_INVALID		BIT(3)	/* invalid result */
#define BCM54XX_ECD_CTRL_UNIT_CM		0	/* centimeters */
#define BCM54XX_ECD_CTRL_UNIT_M			1	/* meters */
#define BCM54XX_ECD_CTRL_UNIT_MASK		BIT(10)	/* cable length unit */
#define BCM54XX_ECD_CTRL_IN_PROGRESS		BIT(11)	/* test in progress */
#define BCM54XX_ECD_CTRL_BREAK_LINK		BIT(12)	/* unconnect link
							 * during test
							 */
#define BCM54XX_ECD_CTRL_CROSS_SHORT_DIS	BIT(13)	/* disable inter-pair
							 * short check
							 */
#define BCM54XX_ECD_CTRL_RUN			BIT(15)	/* run immediate */

#define BCM54XX_RDB_ECD_FAULT_TYPE		0x2a1
#define BCM54XX_EXP_ECD_FAULT_TYPE		(MII_BCM54XX_EXP_SEL_ER + 0xc1)
#define BCM54XX_ECD_FAULT_TYPE_INVALID		0x0
#define BCM54XX_ECD_FAULT_TYPE_OK		0x1
#define BCM54XX_ECD_FAULT_TYPE_OPEN		0x2
#define BCM54XX_ECD_FAULT_TYPE_SAME_SHORT	0x3 /* short same pair */
#define BCM54XX_ECD_FAULT_TYPE_CROSS_SHORT	0x4 /* short different pairs */
#define BCM54XX_ECD_FAULT_TYPE_BUSY		0x9
#define BCM54XX_ECD_FAULT_TYPE_PAIR_D_MASK	GENMASK(3, 0)
#define BCM54XX_ECD_FAULT_TYPE_PAIR_C_MASK	GENMASK(7, 4)
#define BCM54XX_ECD_FAULT_TYPE_PAIR_B_MASK	GENMASK(11, 8)
#define BCM54XX_ECD_FAULT_TYPE_PAIR_A_MASK	GENMASK(15, 12)
#define BCM54XX_ECD_PAIR_A_LENGTH_RESULTS	0x2a2
#define BCM54XX_ECD_PAIR_B_LENGTH_RESULTS	0x2a3
#define BCM54XX_ECD_PAIR_C_LENGTH_RESULTS	0x2a4
#define BCM54XX_ECD_PAIR_D_LENGTH_RESULTS	0x2a5

#define BCM54XX_RDB_ECD_PAIR_A_LENGTH_RESULTS	0x2a2
#define BCM54XX_EXP_ECD_PAIR_A_LENGTH_RESULTS	(MII_BCM54XX_EXP_SEL_ER + 0xc2)
#define BCM54XX_RDB_ECD_PAIR_B_LENGTH_RESULTS	0x2a3
#define BCM54XX_EXP_ECD_PAIR_B_LENGTH_RESULTS	(MII_BCM54XX_EXP_SEL_ER + 0xc3)
#define BCM54XX_RDB_ECD_PAIR_C_LENGTH_RESULTS	0x2a4
#define BCM54XX_EXP_ECD_PAIR_C_LENGTH_RESULTS	(MII_BCM54XX_EXP_SEL_ER + 0xc4)
#define BCM54XX_RDB_ECD_PAIR_D_LENGTH_RESULTS	0x2a5
#define BCM54XX_EXP_ECD_PAIR_D_LENGTH_RESULTS	(MII_BCM54XX_EXP_SEL_ER + 0xc5)
#define BCM54XX_ECD_LENGTH_RESULTS_INVALID	0xffff

#endif /* _LINUX_BRCMPHY_H */
