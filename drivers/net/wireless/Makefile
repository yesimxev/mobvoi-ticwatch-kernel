#
# Makefile for the Linux Wireless network device drivers.
#

obj-$(CONFIG_WLAN_VENDOR_ADMTEK) += admtek/
obj-$(CONFIG_WLAN_VENDOR_ATH) += ath/
obj-$(CONFIG_WLAN_VENDOR_ATMEL) += atmel/
obj-$(CONFIG_WLAN_VENDOR_BROADCOM) += broadcom/
obj-$(CONFIG_WLAN_VENDOR_CISCO) += cisco/
obj-$(CONFIG_WLAN_VENDOR_INTEL) += intel/
obj-$(CONFIG_WLAN_VENDOR_INTERSIL) += intersil/
obj-$(CONFIG_WLAN_VENDOR_MARVELL) += marvell/
obj-$(CONFIG_WLAN_VENDOR_MEDIATEK) += mediatek/
obj-$(CONFIG_WLAN_VENDOR_RALINK) += ralink/
obj-$(CONFIG_WLAN_VENDOR_REALTEK) += realtek/
obj-$(CONFIG_WLAN_VENDOR_RSI) += rsi/
obj-$(CONFIG_WLAN_VENDOR_ST) += st/
obj-$(CONFIG_WLAN_VENDOR_TI) += ti/
obj-$(CONFIG_WLAN_VENDOR_ZYDAS) += zydas/
obj-$(CONFIG_IPW2100) += ipw2x00/
obj-$(CONFIG_IPW2200) += ipw2x00/

obj-$(CONFIG_HERMES)		+= orinoco/

obj-$(CONFIG_AIRO)		+= airo.o
obj-$(CONFIG_AIRO_CS)		+= airo_cs.o airo.o

obj-$(CONFIG_ATMEL)             += atmel.o
obj-$(CONFIG_PCI_ATMEL)         += atmel_pci.o
obj-$(CONFIG_PCMCIA_ATMEL)      += atmel_cs.o

obj-$(CONFIG_PRISM54)		+= prism54/

obj-$(CONFIG_HOSTAP)		+= hostap/
obj-$(CONFIG_B43)		+= b43/
obj-$(CONFIG_B43LEGACY)		+= b43legacy/

# 16-bit wireless PCMCIA client drivers
obj-$(CONFIG_PCMCIA_RAYCS)	+= ray_cs.o
obj-$(CONFIG_PCMCIA_WL3501)	+= wl3501_cs.o

obj-$(CONFIG_USB_NET_RNDIS_WLAN)	+= rndis_wlan.o

obj-$(CONFIG_LIBERTAS)		+= libertas/

obj-$(CONFIG_LIBERTAS_THINFIRM)	+= libertas_tf/

obj-$(CONFIG_ADM8211)	+= adm8211.o

obj-$(CONFIG_MWL8K)	+= mwl8k.o

obj-$(CONFIG_IWLWIFI)	+= iwlwifi/
obj-$(CONFIG_IWLEGACY)	+= iwlegacy/

obj-$(CONFIG_P54_COMMON)	+= p54/

obj-$(CONFIG_ATH_CARDS)		+= ath/
obj-$(CONFIG_MAC80211_HWSIM)	+= mac80211_hwsim.o

obj-$(CONFIG_WL_TI)	+= ti/

obj-$(CONFIG_MWIFIEX)	+= mwifiex/

obj-y    += bcmdhd/
obj-$(CONFIG_BRCMFMAC)	+= brcm80211/
obj-$(CONFIG_BRCMSMAC)	+= brcm80211/

obj-$(CONFIG_CW1200)	+= cw1200/
obj-$(CONFIG_RSI_91X)	+= rsi/
obj-$(CONFIG_CNSS2)	+= cnss2/
obj-$(CONFIG_CNSS)              += cnss/
obj-n += cnss_prealloc/
obj-$(CONFIG_CNSS_UTILS) += cnss_utils/
obj-$(CONFIG_CNSS_GENL) += cnss_genl/
obj-n  += cnss_crypto/
obj-$(CONFIG_QCA402X) += qca402x/
obj-$(CONFIG_VIRT_WIFI)	+= virt_wifi.o
