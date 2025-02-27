/******************************************************************************
 * Copyright (C) 2015 Broadcom Corporation
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 ******************************************************************************/

#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/sysrq.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>
#include <linux/kthread.h>
#include <linux/circ_buf.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/of_gpio.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
//#include <linux/pm_wakeup.h>
#include <linux/suspend.h>
#include <linux/kernel.h>
//#include <linux/ssp_platformdata.h>
#include <linux/gpio.h>
//#include <plat/gpio-cfg.h>
#include <linux/of_gpio.h>
#include <linux/miscdevice.h>
#include <linux/time.h>
#include <linux/io.h>
#include <asm/irq.h>
#include <linux/kernel_stat.h>
#include <linux/pm_runtime.h>
// #include <linux/spi/spi-geni-qcom.h>



#include "bbd.h"
#include "bcm_gps_spi.h"


extern void bbd_parse_asic_data(unsigned char *pucData, unsigned short usLen,
		void (*to_gpsd)(unsigned char *packet, unsigned short len, void *priv), void *priv);

void bcm477x_debug_info(void);
bool ssi_dbg = false;
bool ssi_dbg_pzc = true;    // SHOULD BE TRUE
bool ssi_dbg_rng = false;

// SPI Streaming Protocol Control
int  g_bcm_bitrate = 12000;       // TODO: Need to read bitrate from bus driver spi.c. Just for startup info notification.
int  ssi_mode = 1;                //  0 - Half Duplex, 1 - Full Duplex (not supported yet)
int  ssi_len = 2;                 //  1 = 1B, 2 = 2B
int  ssi_tx_fail = 0;            // Calculating TX transfer failure operation in bus driver, reset in bcm_ssi_open()
int  ssi_tx_pzc_retries = 0;      // Calculating TX pzc retries, reset in bcm_ssi_open()
int  ssi_tx_pzc_retry_delays = 0; // Calculating TX pzc retry delays, reset in bcm_ssi_open()
int  ssi_pm_semaphore = 0;        // Suspend/Resume semaphore
unsigned long g_rx_buffer_avail_bytes = HSI_PZC_MAX_RX_BUFFER; // Should be more MAX_SPI_FRAME_LEN. See below

static struct bcm_spi_priv *g_bcm_gps;

struct bcm_spi_priv *bcm_get_bcm_gps(void)
{
	return g_bcm_gps;
}

#ifdef CONFIG_TRANSFER_STAT

static ssize_t bcm_4775_nstandby_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
    int value = 0;
    struct spi_device *spi = to_spi_device(dev);
	struct bcm_spi_priv *priv = (struct bcm_spi_priv*) spi_get_drvdata(spi);
	value = gpio_get_value(priv->nstandby);
	return snprintf(buf, 20, "%d\n", value);
}

static ssize_t bcm_4775_nstandby_store(struct device *dev, struct device_attribute *attr,
		const char *buf,size_t count)
{
    struct spi_device *spi = to_spi_device(dev);
	struct bcm_spi_priv *priv = (struct bcm_spi_priv*) spi_get_drvdata(spi);
	pr_debug("[SSPBBD} bcm_4775_nstandby, buf is %s\n", buf);

	if (!strncmp("0",buf,1))
	   gpio_set_value(priv->nstandby, 0);
	else
	   gpio_set_value(priv->nstandby, 1);
	return count;
}

static DEVICE_ATTR(nstandby, 0660, bcm_4775_nstandby_show, bcm_4775_nstandby_store);

void bcm_ssi_clear_trans_stat(struct bcm_spi_priv *priv)
{
	memset(priv->trans_stat,0,sizeof(priv->trans_stat));
}

void bcm_ssi_print_trans_stat(struct bcm_spi_priv *priv)
{
	char buf[512];
	char *p = buf;

	struct bcm_spi_transfer_stat *trans = &priv->trans_stat[0];
	p = buf;

	p += sprintf(p,"[SSPBBD]: DBG SPI @ TX: <255B = %d, <1K = %d, <2K = %d, <4K = %d, <8K = %d, <16K = %d, <32K = %d, <64K = %d, total = %ld, min = %ld, max = %ld ",
			trans->len_255,trans->len_1K,trans->len_2K,trans->len_4K,trans->len_8K,trans->len_16K,trans->len_32K,trans->len_64K,trans->len_total,trans->len_min,trans->len_max);
	pr_debug("%s\n",buf);

	trans = &priv->trans_stat[1];
	p = buf;
	p += sprintf(p,"[SSPBBD]: DBG SPI @ RX: <255B = %d, <1K = %d, <2K = %d, <4K = %d, <8K = %d, <16K = %d, <32K = %d, <64K = %d, total = %ld, min = %ld, max = %ld ",
			trans->len_255,trans->len_1K,trans->len_2K,trans->len_4K,trans->len_8K,trans->len_16K,trans->len_32K,trans->len_64K,trans->len_total,trans->len_min,trans->len_max);

	pr_debug("%s\n",buf);

    p = buf;
	p += sprintf(p,"[SSPBBD]: DBG SPI @ PZC: retries = %d, delays = %d ",
				     ssi_tx_pzc_retries,ssi_tx_pzc_retry_delays);
	pr_debug("%s\n",buf);
}

static void bcm_ssi_calc_trans_stat(struct bcm_spi_transfer_stat *trans,unsigned short length)
{
	if ( length <= 255 ) {
		trans->len_255++;
	} else if ( length <=  1024 ) {
		trans->len_1K++;
	} else if ( length <=  (2*1024) ) {
		trans->len_2K++;
	}  else if ( length <= (4*1024) ) {
		trans->len_4K++;
	}  else if ( length <= (8*1024) ) {
		trans->len_8K++;
	}  else if ( length <= (16*1024) ) {
		trans->len_16K++;
	}   else if ( length <= (32*1024) ) {
		trans->len_32K++;
	} else {
		trans->len_64K++;
	}

    if (length > trans->len_max )
        trans->len_max = length;

    if ( trans->len_min == 0 || length < trans->len_min )
        trans->len_min = length;

    trans->len_total += length;
}
#endif // CONFIG_TRANSFER_STAT

unsigned long m_ulRxBufferBlockSize[4] = {32,256,1024 * 2, 1024 *16};

static unsigned long bcm_ssi_chk_pzc(struct bcm_spi_priv *priv, unsigned char stat_byte, bool bprint)
{
	char acB[512] = {0};
	char *p = acB;

	unsigned long rx_buffer_blk_bytes  =  m_ulRxBufferBlockSize[(unsigned short)((stat_byte & HSI_F_MOSI_CTRL_SZE_MASK) >> HSI_F_MOSI_CTRL_SZE_SHIFT)];
	unsigned long rx_buffer_blk_counts = ((unsigned long)((stat_byte & HSI_F_MOSI_CTRL_CNT_MASK) >> HSI_F_MOSI_CTRL_CNT_SHIFT));
	acB[0] = 0;

	g_rx_buffer_avail_bytes = rx_buffer_blk_bytes * rx_buffer_blk_counts;

	if (bprint) {
		if ( stat_byte & HSI_F_MOSI_CTRL_PE_MASK ) {
			p += snprintf(p,sizeof(acB) - (p - acB),"[SSPBBD]: DBG SPI @ PZC: rx stat 0x%02x%s avail %lu",
					stat_byte, stat_byte & HSI_F_MOSI_CTRL_PE_MASK ? "(PE)": "   ",g_rx_buffer_avail_bytes);
		}

#ifdef CONFIG_REG_IO
		if ( stat_byte & HSI_F_MOSI_CTRL_PE_MASK ) {
			unsigned char regval;
			RegRead( priv,  "HSI_ERROR_STATUS(R) ", HSI_ERROR_STATUS, &regval, 1 );

			if ( regval & HSI_ERROR_STATUS_STRM_FIFO_OVFL )
				p += snprintf(p,sizeof(acB) - (p - acB), ", (rx_strm_fifo_ovfl)");

			regval = HSI_ERROR_STATUS_ALL_ERRORS;
			RegWrite( priv, "HSI_ERROR_STATUS(W) ", HSI_ERROR_STATUS, &regval, 1 );
		}
#endif // CONFIG_REG_IO

		if ( acB[0] )
			pr_debug("%s\n",acB);
	}

	return g_rx_buffer_avail_bytes;
}


//--------------------------------------------------------------
//
//			   File Operations
//
//--------------------------------------------------------------
static int bcm_spi_open(struct inode *inode, struct file *filp)
{
	/* Initially, file->private_data points device itself and we can get our priv structs from it. */
	struct bcm_spi_priv *priv = container_of(filp->private_data, struct bcm_spi_priv, misc);
	struct bcm_spi_strm_protocol *strm;
	unsigned long int flags;
	unsigned char fc_mask, len_mask, duplex_mask;
	char buf[512];
	char *p = buf;

	pr_debug("%s++\n", __func__);

	if (priv->busy)
		return -EBUSY;

	priv->busy = true;

	/* Reset circ buffer */
	priv->read_buf.head = priv->read_buf.tail = 0;
	priv->write_buf.head = priv->write_buf.tail = 0;

	priv->packet_received = 0;

	/* Enable irq */
	spin_lock_irqsave(&priv->irq_lock, flags);
	if (!atomic_xchg(&priv->irq_enabled, 1))
		enable_irq(priv->spi->irq);

	spin_unlock_irqrestore(&priv->irq_lock, flags);

	enable_irq_wake(priv->spi->irq);

	filp->private_data = priv;
#ifdef DEBUG_1HZ_STAT
	bbd_enable_stat();
#endif


	strm = &priv->tx_strm;
	strm->pckt_len = ssi_len == 2 ? 2:1;
	len_mask = ssi_len == 2 ? SSI_PCKT_2B_LENGTH:SSI_PCKT_1B_LENGTH;
	duplex_mask = ssi_mode != 0 ? SSI_MODE_FULL_DUPLEX: SSI_MODE_HALF_DUPLEX;

	fc_mask = SSI_FLOW_CONTROL_DISABLED;
	strm->fc_len = 0;
	if ( ssi_mode == 0 ) {
		// SSI_MODE_HALF_DUPLEX;
		strm->pckt_len = 0;
	}
	strm->ctrl_len = strm->pckt_len + strm->fc_len + 1; // 1 for tx cmd byte

	strm->frame_len = ssi_len == 2 ? MAX_SPI_FRAME_LEN : MIN_SPI_FRAME_LEN;
	strm->ctrl_byte = duplex_mask | SSI_MODE_STREAM | len_mask | SSI_WRITE_TRANS | fc_mask;

	// TX SPI Streaming Protocol in details
	p = buf;
	sprintf(p,"[SSPBBD]: tx ctrl %02X: total %d = len %d + fc %d + cmd 1",
			strm->ctrl_byte, strm->ctrl_len, strm->pckt_len, strm->fc_len);
	pr_debug("%s\n",buf);

	strm = &priv->rx_strm;
	strm->pckt_len = ssi_len == 2 ? 2:1;
	strm->fc_len = 0;
	strm->ctrl_len = strm->pckt_len + strm->fc_len + 1; // 1 for rx stat byte
	strm->frame_len = ssi_len == 2 ? MAX_SPI_FRAME_LEN : MIN_SPI_FRAME_LEN;

    // Because this rx ‘ctrl_byte’ is being used in rx thread ‘bcm_ssi_rx’ only then SSI_READ_TRANS is correct value for that thread.
    // Selecting  between full and half duplex is in tx thread ‘‘bcm_ssi_tx’ only.
	// strm->ctrl_byte = duplex_mask | SSI_MODE_STREAM | len_mask | (ssi_mode == SSI_MODE_FULL_DUPLEX ? SSI_WRITE_TRANS : SSI_READ_TRANS);
    strm->ctrl_byte = duplex_mask | SSI_MODE_STREAM | len_mask | SSI_READ_TRANS;

	// RX SPI Streaming Protocol in details
	p = buf;
	sprintf(p,"[SSPBBD]: rx ctrl %02X: total %d = len %d + fc %d + stat 1",
			strm->ctrl_byte, strm->ctrl_len, strm->pckt_len, strm->fc_len);
	pr_debug("%s\n",buf);

	{
		p = buf;
		sprintf(p,"[SSPBBD]: SPI @ %d: %s Duplex Strm mode, %dB Len, %s FC, Frame Len %u : tx ctrl %02X, rx ctrl %02X",
				g_bcm_bitrate,
				ssi_mode  != 0 ? "Full": "Half",
				ssi_len       == 2 ? 2:1,
				"w/o",
				strm->frame_len,
				priv->tx_strm.ctrl_byte, priv->rx_strm.ctrl_byte);
		pr_debug("%s\n",buf);

#ifdef CONFIG_REG_IO
		// bcm477x_data_dump(priv);
		{
            unsigned char regval8[2];
            unsigned regval32[16];
            RegRead( priv,"HSI_STATUS     " ,HSI_STATUS, regval8,     1 );
            RegRead(priv, "HSI_ERROR_STATUS(R) ", HSI_ERROR_STATUS, regval8, 1);
            bcm_reg32Iread( priv,"INTR_MASK/STAT " ,HSI_INTR_MASK, regval32,2 );
			bcm_reg32Iread( priv,"RNGDMA_RX      " ,HSI_RNGDMA_RX_BASE_ADDR, regval32,8 );
			bcm_reg32Iread( priv,"RNGDMA_TX      " ,HSI_RNGDMA_TX_BASE_ADDR, regval32,8 );
			bcm_reg32Iread( priv,"HSI_CTRL       " ,HSI_CTRL, regval32,4 );
			bcm_reg32Iread( priv,"ADL_ABR        " ,HSI_ADL_ABR_CONTROL    , regval32,4 );
            bcm_reg32Iread( priv,"RSTN/STBY/EN   " ,HSI_RESETN, regval32,4 );
            bcm_reg32Iread( priv,"STRM/CMND      " ,HSI_STRM_FIFO_STATUS, regval32,2 );
		}
        bcm_ssi_print_trans_stat(priv);
#endif
	}

#ifdef CONFIG_TRANSFER_STAT
	bcm_ssi_clear_trans_stat(priv);
#endif
	ssi_tx_fail       = 0;
	ssi_tx_pzc_retries = 0;
	ssi_tx_pzc_retry_delays = 0;
	ssi_pm_semaphore = 0;
	g_rx_buffer_avail_bytes = HSI_PZC_MAX_RX_BUFFER;

	pr_debug("%s--\n", __func__);
	return 0;
}

static int bcm_spi_release(struct inode *inode, struct file *filp)
{
	struct bcm_spi_priv *priv = filp->private_data;
	unsigned long int flags;

	pr_debug("%s++\n", __func__);
	priv->busy = false;

#ifdef CONFIG_TRANSFER_STAT
	bcm_ssi_print_trans_stat(priv);
#endif


#ifdef DEBUG_1HZ_STAT
	bbd_disable_stat();
#endif
	/* Disable irq */
	spin_lock_irqsave(&priv->irq_lock, flags);
	if (atomic_xchg(&priv->irq_enabled, 0))
		disable_irq_nosync(priv->spi->irq);

	spin_unlock_irqrestore(&priv->irq_lock, flags);

	disable_irq_wake(priv->spi->irq);

	pr_debug("%s--\n", __func__);
	return 0;
}

static ssize_t bcm_spi_read(struct file *filp, char __user *buf, size_t size, loff_t *ppos)
{
	struct bcm_spi_priv *priv = filp->private_data;
	struct circ_buf *circ = &priv->read_buf;
	size_t rd_size = 0;

	//pr_debug("[SSPBBD] %s++\n", __func__);
	mutex_lock(&priv->rlock);

	/* Copy from circ buffer to user
	 * We may require 2 copies from [tail..end] and [end..head]
	 */
	do {
		size_t cnt_to_end = CIRC_CNT_TO_END(circ->head, circ->tail, BCM_SPI_READ_BUF_SIZE);
		size_t copied = min(cnt_to_end, size);

		WARN_ON(copy_to_user(buf + rd_size, (void *) circ->buf + circ->tail, copied));
		size -= copied;
		rd_size += copied;
		circ->tail = (circ->tail + copied) & (BCM_SPI_READ_BUF_SIZE-1);

	} while (size > 0 && CIRC_CNT(circ->head, circ->tail, BCM_SPI_READ_BUF_SIZE));
	mutex_unlock(&priv->rlock);

#ifdef DEBUG_1HZ_STAT
	bbd_update_stat(STAT_RX_LHD, rd_size);
#endif

	pr_debug("[SSPBBD] %s--(%zd bytes)\n", __func__, rd_size);
	return rd_size;
}

static ssize_t bcm_spi_write(struct file *filp, const char __user *buf, size_t size, loff_t *ppos)
{
	struct bcm_spi_priv *priv = filp->private_data;
	struct circ_buf *circ = &priv->write_buf;
	size_t wr_size = 0;

	pr_debug("[SSPBBD] %s++(%zd bytes)\n", __func__, size);

	mutex_lock(&priv->wlock);
	/* Copy from user into circ buffer
	 * We may require 2 copies from [tail..end] and [end..head]
	 */
	do {
		size_t space_to_end = CIRC_SPACE_TO_END(circ->head, circ->tail, BCM_SPI_WRITE_BUF_SIZE);
		size_t copied = min(space_to_end, size);


		WARN_ON(copy_from_user((void *) circ->buf + circ->head, buf + wr_size, copied));
		size -= copied;
		wr_size += copied;
		circ->head = (circ->head + copied) & (BCM_SPI_WRITE_BUF_SIZE - 1);
	} while (size > 0 && CIRC_SPACE(circ->head, circ->tail, BCM_SPI_WRITE_BUF_SIZE));

	mutex_unlock(&priv->wlock);

	pr_debug("[SSPBBD] %s--(%zd bytes)\n", __func__, wr_size);

	/* kick start rxtx thread */
	/* we don't want to queue work in suspending and shutdown */
	if (!atomic_read(&priv->suspending))
		queue_work(priv->serial_wq, (struct work_struct *)&priv->tx_work);

#ifdef DEBUG_1HZ_STAT
	bbd_update_stat(STAT_TX_LHD, wr_size);
#endif
	return wr_size;
}

static unsigned int bcm_spi_poll(struct file *filp, poll_table *wait)
{
	struct bcm_spi_priv *priv = filp->private_data;
	struct circ_buf *rd_circ = &priv->read_buf;
	struct circ_buf *wr_circ = &priv->write_buf;
	unsigned int mask = 0;

	poll_wait(filp, &priv->poll_wait, wait);

	if (CIRC_CNT(rd_circ->head, rd_circ->tail, BCM_SPI_READ_BUF_SIZE))
		mask |= POLLIN;

	if (CIRC_SPACE(wr_circ->head, wr_circ->tail, BCM_SPI_WRITE_BUF_SIZE))
		mask |= POLLOUT;

	return mask;
}


static const struct file_operations bcm_spi_fops = {
	.owner		  =  THIS_MODULE,
	.open		   =  bcm_spi_open,
	.release		=  bcm_spi_release,
	.read		   =  bcm_spi_read,
	.write		  =  bcm_spi_write,
	.poll		   =  bcm_spi_poll,
};



//--------------------------------------------------------------
//
//			   Misc. functions
//
//--------------------------------------------------------------

/*
 * bcm4773_hello - wakeup chip by toggling mcu_req while monitoring mcu_resp to check if awake
 *
 */
static unsigned long init_time = 0;
unsigned long bcm_clock_get_ms(void)
{
	struct timeval t;
	unsigned long now;

	do_gettimeofday(&t);
	now = t.tv_usec / 1000 + t.tv_sec * 1000;
	if (init_time == 0)
		init_time = now;

	return now - init_time;
}

void wait1secDelay(unsigned int count)
{
	if (count <= 100)
		mdelay(1);
	else
		msleep(1);
}


static bool bcm477x_hello(struct bcm_spi_priv *priv)
{
	int count = 0; //, retries = 0;
#define MAX_RESP_CHECK_COUNT 100 // 100msec

	unsigned long start_time, delta;
	start_time = bcm_clock_get_ms();

    //pr_debug("[SSPBBD} bcm477x_hello\n");
	gpio_set_value(priv->mcu_req, 1);
	while (!gpio_get_value(priv->mcu_resp)) {
		if (count++ > MAX_RESP_CHECK_COUNT) {
			gpio_set_value(priv->mcu_req, 0);
			pr_debug("[SSPBBD] MCU_REQ_RESP timeout. MCU_RESP(gpio%d) not responding to MCU_REQ(gpio%d)\n",
					priv->mcu_resp, priv->mcu_req);
			// #ifdef CONFIG_REG_IO
			//            bcm477x_data_dump(priv);
			// #endif
			return false;
		}

		wait1secDelay(count);

		/*if awake, done */
		if (gpio_get_value(priv->mcu_resp))
			break;

		if (count % 20 == 0) {
			gpio_set_value(priv->mcu_req, 0);
			mdelay(1);
			gpio_set_value(priv->mcu_req, 1);
			mdelay(1);
		}
	}

	delta = bcm_clock_get_ms() - start_time;

	if (count > 100)
		printk("[SSPBBD] hello consumed %lu = clock_get_ms() - start_time; msec", delta);

	return true;
}

/*
 * bcm4773_bye - set mcu_req low to let chip go to sleep
 *
 */
static void bcm477x_bye(struct bcm_spi_priv *priv)
{
	gpio_set_value(priv->mcu_req, 0);
}


static void pk_log(char *dir, unsigned char *data, int len)
{
	char acB[960];
	char *p = acB;
	int  the_trans = len;
	int i, j, n;

	char ic = 'D';
	char xc = dir[0] == 'r' || dir[0] == 'w' ? 'x':'X';

	if (likely(!ssi_dbg))
		return;

	//FIXME: There is print issue. Printing 7 digits instead of 6 when clock is over 1000000. "% 1000000" added
	// E.g.
	//#999829D w 0x68,  1: A2
	//#999829D r 0x68, 34: 8D 00 01 52 5F B0 01 B0 00 8E 00 01 53 8B B0 01 B0 00 8F 00 01 54 61 B0 01 B0 00 90 00 01 55 B5
	//		 r		  B0 01
	//#1000001D w 0x68, 1: A1
	//#1000001D r 0x68, 1: 00
	n = len;
	p += snprintf(acB, sizeof(acB), "#%06ld%c %2s,	  %5d: ",
			bcm_clock_get_ms() % 1000000, ic, dir, n);

	for (i = 0, n = 32; i < len; i = j, n = 32) {
		for (j = i; j < (i + n) && j < len && the_trans != 0; j++, the_trans--)
			p += snprintf(p, sizeof(acB) - (p - acB), "%02X ", data[j]);

		pr_debug("%s\n", acB);
		if (j < len) {
			p = acB;
			if (the_trans == 0) {
				dir[0] = xc;
				the_trans--;
			}
			p += snprintf(acB, sizeof(acB), "         %2s              ", dir);
		}
	}

	if (i == 0)
		pr_debug("%s\n", acB);
}



//--------------------------------------------------------------
//
//			   SSI tx/rx functions
//
//--------------------------------------------------------------

static unsigned short bcm_ssi_get_len(unsigned char ctrl_byte, unsigned char *data)
{
	unsigned short len;
	if ( ctrl_byte & SSI_PCKT_2B_LENGTH ) {
		len = ((unsigned short)data[0] + ((unsigned short)data[1] << 8));
	} else {
		len = (unsigned short)data[0];
	}

	return len;
}

static void bcm_ssi_set_len(unsigned char ctrl_byte, unsigned char *data,unsigned short len)
{
	if ( ctrl_byte & SSI_PCKT_2B_LENGTH ) {
		data[0] = (unsigned char)(len & 0xff);
		data[1] = (unsigned char)((len >> 8)  & 0xff);
	} else {
		data[0] = (unsigned char)len;
	}
}

static void bcm_ssi_clr_len(unsigned char ctrl_byte, unsigned char *data)
{
	bcm_ssi_set_len(ctrl_byte, data,0);
}


int bcm_spi_sync(struct bcm_spi_priv *priv, void *tx_buf, void *rx_buf, int len, int bits_per_word)
{
	struct spi_message msg;
	struct spi_transfer xfer;
	int ret;

	/* Init */
	spi_message_init(&msg);
	memset(&xfer, 0, sizeof(xfer));
	spi_message_add_tail(&xfer, &msg);

	/* Setup */
	msg.spi = priv->spi;
	xfer.len = len;
	xfer.tx_buf = tx_buf;
	xfer.rx_buf = rx_buf;

	/* Sync */
	pk_log("w", (unsigned char *)xfer.tx_buf, len);
	ret = spi_sync(msg.spi, &msg);
	pk_log("r", (unsigned char *)xfer.rx_buf, len);

	if (ret)
		pr_debug("[SSPBBD] spi_sync error for cmd:0x%x, return=%d\n",
			((struct bcm_ssi_tx_frame *)xfer.tx_buf)->cmd, ret);

	return ret;
}


static int bcm_ssi_tx(struct bcm_spi_priv *priv, int length)
{
	struct bcm_ssi_tx_frame *tx = priv->tx_buf;
	struct bcm_ssi_rx_frame *rx = priv->rx_buf;
	struct bcm_spi_strm_protocol *strm = &priv->tx_strm;
	int bits_per_word = (length >= 255) ? CONFIG_SPI_DMA_BITS_PER_WORD : 8;
	int ret;
	unsigned short Mwrite;
	unsigned short bytes_to_write = (unsigned short)length;
    unsigned short bytes_written = 0;
	unsigned short Nread = 0;  // for Full Duplex only
    unsigned short frame_data_size = strm->frame_len - strm->ctrl_len;

    (void)bytes_written;    // To get rid of warnings

	pr_debug("[SSPBBD] %s ++ (%d bytes) @ %s\n", __func__,
	 	bytes_to_write,strm->ctrl_byte & SSI_FLOW_CONTROL_ENABLED ? "FC" : "");

	Mwrite = bytes_to_write;

	do {
		tx->cmd = strm->ctrl_byte;  // SSI_WRITE_HD etc.

		bytes_to_write = max(Mwrite,Nread);

		// -- if ( (strm->ctrl_byte & SSI_FLOW_CONTROL_ENABLED) || (strm->ctrl_byte & SSI_MODE_FULL_DUPLEX)  )
		if ( strm->pckt_len !=0 ) {
			bcm_ssi_set_len(strm->ctrl_byte, tx->data,Mwrite);
		}

		ret = bcm_spi_sync(priv, tx, rx, bytes_to_write + strm->ctrl_len, bits_per_word);  // ctrl_len is for tx len + fc

		if ( ret ) {
			ssi_tx_fail++;
			break;   // TODO: failure, operation should gets 0 to continue
		}

		// Condition was added after Jun Lu's code review
		if (strm->pckt_len != 0) {
			unsigned short m_write = bcm_ssi_get_len(strm->ctrl_byte, tx->data);  // Just for understanding SPI Streaming Protocol
            if (m_write > frame_data_size ) {                                     // The h/w malfunctioned ?
				pr_debug("[SSPBBD]: %s @ TX Mwrite %d is h/w overflowed of frame %d...Fail\n", __func__,m_write,frame_data_size);
            }
		}

		if ( strm->ctrl_byte & SSI_MODE_FULL_DUPLEX  ) {
			unsigned char *data_p = rx->data + strm->pckt_len;
			Nread = bcm_ssi_get_len(strm->ctrl_byte, rx->data);
            if ( Nread > frame_data_size ) {
				pr_debug("[SSPBBD]: %s @ FD Nread %d is h/w overflowed of frame %d...Fail\n", __func__,Nread,frame_data_size);
                Nread = frame_data_size;
            }

			if ( Mwrite < Nread ) {
				/* Call BBD */
				bbd_parse_asic_data(data_p, Mwrite, NULL, priv);  // 1/2 bytes for len
				Nread -= Mwrite;
				bytes_to_write -= Mwrite;
				data_p += (Mwrite + strm->fc_len);
#ifdef CONFIG_TRANSFER_STAT
			    bcm_ssi_calc_trans_stat(&priv->trans_stat[1],Mwrite);
#endif
			} else {
                bytes_to_write = Nread;                             // No data available next time
                Nread = 0;
            }

			/* Call BBD */
			if ( bytes_to_write != 0 ) {
				bbd_parse_asic_data(data_p, bytes_to_write, NULL, priv);  // 1/2 bytes for len
#ifdef CONFIG_TRANSFER_STAT
			    bcm_ssi_calc_trans_stat(&priv->trans_stat[1],bytes_to_write);
#endif
			}
		}

		// -- if ( strm->ctrl_byte & SSI_FLOW_CONTROL_ENABLED )
        bytes_written += bytes_to_write;
	} while ( 0 );

#ifdef CONFIG_TRANSFER_STAT
	bcm_ssi_calc_trans_stat(&priv->trans_stat[0],bytes_written);
#endif

	pr_debug("[SSPBBD] %s -- ret %d\n", __func__,ret);
	bcm_ssi_chk_pzc(priv, rx->status, ssi_dbg_pzc);

	return ret;
}

int bcm_ssi_rx(struct bcm_spi_priv *priv, size_t *length)
{
	struct bcm_ssi_tx_frame *tx = priv->tx_buf;
	struct bcm_ssi_rx_frame *rx = priv->rx_buf;
	struct bcm_spi_strm_protocol *strm = &priv->rx_strm;
	unsigned short ctrl_len = strm->pckt_len+1;  // +1 for rx status
	unsigned short payload_len;

	pr_debug("[SSPBBD]: %s ++\n", __func__);

#ifdef CONFIG_REG_IO
	if (likely(ssi_dbg_rng) &&  (priv->packet_received > CONFIG_PACKET_RECEIVED) ) {
		unsigned regval32[8];
		bcm_reg32Iread( priv,"RNGDMA_TX      " ,HSI_RNGDMA_TX_SW_ADDR_OFFSET, regval32,3 );
	}
#endif

	// TODO:: Check 1B or 2B mode
	// memset(tx, 0, MAX_SPI_FRAME_LEN);

	bcm_ssi_clr_len(strm->ctrl_byte, tx->data);      // tx and rx ctrl_byte(s) are same
	tx->cmd = strm->ctrl_byte;                       // SSI_READ_HD etc.
	rx->status = 0;

	if (bcm_spi_sync(priv, tx, rx, ctrl_len, 8))
		return -1;

    bcm_ssi_chk_pzc(priv, rx->status, ssi_dbg_pzc );

	payload_len = bcm_ssi_get_len(strm->ctrl_byte, rx->data);

	pr_debug("[SSPBBD]: %s ++, len %u\n", __func__,payload_len);
	if (payload_len == 0) {
		// -- TODO --  payload_len = MIN_SPI_FRAME_LEN;  // Needn't to use MAX_SPI_FRAME_LEN because don't know how many bytes is ready to really read
        // -- TODO --  pr_debug("[SSPBBD]: %s @ RX length is still read to 0. Set %d\n", __func__, payload_len);
        return -1;
	}

	/* TODO: limit max payload to 254 because of exynos3 bug */
	{
		*length = min((unsigned short)(strm->frame_len-ctrl_len), payload_len);  // MAX_SPI_FRAME_LEN
		memset(tx->data, 0, *length + ctrl_len - 1);   // -1 for rx status

		if (bcm_spi_sync(priv, tx, rx, *length+ctrl_len, 8))
			return -1;
	}

	payload_len = bcm_ssi_get_len(strm->ctrl_byte, rx->data);
	if (payload_len < *length) {
		*length = payload_len;
	}

#ifdef DEBUG_1HZ_STAT
	bbd_update_stat(STAT_RX_SSI, *length);
#endif

#ifdef CONFIG_TRANSFER_STAT
	bcm_ssi_calc_trans_stat(&priv->trans_stat[1],*length);
#endif

	return 0;
}

void bcm_on_packet_received(void *_priv, unsigned char *data, unsigned int size)
{
	struct bcm_spi_priv *priv = (struct bcm_spi_priv *)_priv;
	struct circ_buf *rd_circ = &priv->read_buf;
	size_t written = 0, avail = size;

	pr_debug("[SSPBBD]: %s ++\n", __func__);
	/* Copy into circ buffer */
	mutex_lock(&priv->rlock);
	do {
		size_t space_to_end = CIRC_SPACE_TO_END(rd_circ->head, rd_circ->tail, BCM_SPI_READ_BUF_SIZE);
		size_t copied = min(space_to_end, avail);

		memcpy((void *) rd_circ->buf + rd_circ->head, data + written, copied);
		avail -= copied;
		written += copied;
		rd_circ->head = (rd_circ->head + copied) & (BCM_SPI_READ_BUF_SIZE - 1);
	} while (avail > 0 && CIRC_SPACE(rd_circ->head, rd_circ->tail, BCM_SPI_READ_BUF_SIZE));

	priv->packet_received+=size;
	mutex_unlock(&priv->rlock);
	wake_up(&priv->poll_wait);

	if (avail > 0)
		pr_debug("[SSPBBD]: input overrun error by %zd bytes!\n", avail);

	pr_debug("[SSPBBD]: %s received -- (%d bytes)\n", __func__, size);
}

static void bcm_rxtx_work_func(struct work_struct *work)
{
#ifdef DEBUG_1HZ_STAT
	u64 ts_rx_start = 0;
	u64 ts_rx_end = 0;
#endif
	struct bcm_rxtx_work *bcm_work = (struct bcm_rxtx_work *)work;
	struct bcm_spi_priv *priv = bcm_work->priv;
	struct circ_buf *wr_circ = &priv->write_buf;
	struct bcm_spi_strm_protocol *strm = &priv->tx_strm;
	unsigned short rx_pckt_len = priv->rx_strm.pckt_len;
    int    wait_for_pzc = 0;

	if (!bcm477x_hello(priv)) {
		pr_debug("[SSPBBD]: %s[%s] timeout!!\n", __func__,
				bcm_work->type == WORK_TYPE_RX ? "RX WORK" : "TX WORK");
		bcm477x_debug_info();
		bbd_mcu_reset();
		return;
	}

	do {
		int    ret = 0;
		size_t avail = 0;

		/* Read first */
		ret = gpio_get_value(priv->host_req);

		pr_debug("[SSPBBD] %s - host_req %d\n", __func__,ret);
		if ( ret || wait_for_pzc ) {
            wait_for_pzc = 0;
#ifdef DEBUG_1HZ_STAT
			struct timespec ts;
			if (stat1hz.ts_irq) {
				ts = ktime_to_timespec(ktime_get_boottime());
				ts_rx_start = ts.tv_sec * 1000000000ULL
					+ ts.tv_nsec;
			}
#endif
			/* Receive SSI frame */
			if (bcm_ssi_rx(priv, &avail))
				break;

#ifdef DEBUG_1HZ_STAT
			bbd_update_stat(STAT_RX_SSI, avail);
			if (ts_rx_start && !gpio_get_value(priv->host_req)) {
				ts = ktime_to_timespec(ktime_get_boottime());
				ts_rx_end = ts.tv_sec * 1000000000ULL
					+ ts.tv_nsec;
			}
#endif
			/* Call BBD */
			bbd_parse_asic_data(priv->rx_buf->data + rx_pckt_len, avail, NULL, priv);  // 1/2 bytes for len
		}

		/* Next, write */
		avail = CIRC_CNT(wr_circ->head, wr_circ->tail, BCM_SPI_WRITE_BUF_SIZE);
		if ( avail ) {
			size_t written=0;

			mutex_lock(&priv->wlock);
			/* For big packet, we should align xfer size to DMA word size and burst size.
			 * That is, SSI payload + one byte command should be multiple of (DMA word size * burst size)
			 */

			if (avail > (strm->frame_len - strm->ctrl_len) )
				avail = strm->frame_len - strm->ctrl_len;

			written = 0; ret = 0;

			// SWGNSSGLL-15521 : Sometimes LHD does not write data because the following code blocks sending data to MCU
			//                   Code is commented out because 'g_rx_buffer_avail_bytes'(PZC) is calculated in bcm_ssi_tx()
			//                   inside loop in work queue (bcm_rxtx_work_func) below this code.
			//                   It means 'g_rx_buffer_avail_bytes' doesn't reflect real available bytes in RX DMA RING buffer when work queue will be restarted
			//                   because MCU is working independently from host.
			//                   The 'g_rx_buffer_avail_bytes' can be tested inside bcm_ssi_tx but it may not guarantee correct condition also.
            // SWGNSSGLL-16290 : FC detecting was broken when buffer is overflow
            //                   Using PZC for a software workaround to not get into fifo overflow condition.
			//                   if (strm->fc_len !=0 && avail > g_rx_buffer_avail_bytes ) {
            if ( avail > g_rx_buffer_avail_bytes ) {

                g_rx_buffer_avail_bytes ? ssi_tx_pzc_retries++:ssi_tx_pzc_retry_delays++;
				pr_debug("[SSPBBD] %s - %d PZC %s, wr CIRC_CNT %u, RNGDMA_RX %lu \n",
						__func__, g_rx_buffer_avail_bytes ? ssi_tx_pzc_retries:ssi_tx_pzc_retry_delays, g_rx_buffer_avail_bytes ? "writes":"delays", avail, g_rx_buffer_avail_bytes);
                if ( g_rx_buffer_avail_bytes == 0 )  // RNGDMA_RX is full ? If it's YES keep reading
                {
                   unsigned regval32[8];
                   bcm_reg32Iread( priv,"RNGDMA_RX      " ,HSI_RNGDMA_RX_SW_ADDR_OFFSET, regval32,3 );
                }
                avail = g_rx_buffer_avail_bytes;
                mdelay(1);         // TODO: increase delay for waiting for draining RNGDMA_RX  on MCU side ?
                wait_for_pzc = 1;  // This case is for when RNGDMA_RX is full and HOST_REQ is low
			}

			/* Copy from wr_circ the data */
			while (avail>0) {
				size_t cnt_to_end = CIRC_CNT_TO_END(wr_circ->head, wr_circ->tail, BCM_SPI_WRITE_BUF_SIZE);
				size_t copied = min(cnt_to_end, avail);

//				pr_debug("[SSPBBD]: %s writing ++ (%d bytes)\n", __func__, copied);

				memcpy(priv->tx_buf->data + strm->pckt_len + written, wr_circ->buf + wr_circ->tail, copied);
				avail -= copied;
				written += copied;
				wr_circ->tail = (wr_circ->tail + copied) & (BCM_SPI_WRITE_BUF_SIZE-1);
				pr_debug("[SSPBBD]: %s writing --\n", __func__);

			} //  while (avail>0);

			/* Transmit SSI frame */
			if ( written )
				ret = bcm_ssi_tx(priv, written);
			mutex_unlock(&priv->wlock);

			if (ret)
				break;

			wake_up(&priv->poll_wait);
#ifdef DEBUG_1HZ_STAT
			bbd_update_stat(STAT_TX_SSI, written);
#endif
		}

	} while (gpio_get_value(priv->host_req) || CIRC_CNT(wr_circ->head, wr_circ->tail, BCM_SPI_WRITE_BUF_SIZE));

	bcm477x_bye(priv);

	/* Enable irq */
	{
		unsigned long int flags;

		spin_lock_irqsave(&priv->irq_lock, flags);

		/* we dont' want to enable irq when going to suspending */
		if (!atomic_read(&priv->suspending))
			if (!atomic_xchg(&priv->irq_enabled, 1))
				enable_irq(priv->spi->irq);

		spin_unlock_irqrestore(&priv->irq_lock, flags);
	}

#ifdef DEBUG_1HZ_STAT
	if (stat1hz.ts_irq && ts_rx_start && ts_rx_end) {
		u64 lat = ts_rx_start - stat1hz.ts_irq;
		u64 dur = ts_rx_end - ts_rx_start;

		stat1hz.min_rx_lat = (lat < stat1hz.min_rx_lat) ?
			lat : stat1hz.min_rx_lat;
		stat1hz.max_rx_lat = (lat > stat1hz.max_rx_lat) ?
			lat : stat1hz.max_rx_lat;
		stat1hz.min_rx_dur = (dur < stat1hz.min_rx_dur) ?
			dur : stat1hz.min_rx_dur;
		stat1hz.max_rx_dur = (dur > stat1hz.max_rx_dur) ?
			dur : stat1hz.max_rx_dur;
		stat1hz.ts_irq = 0;
	}
#endif
}


//--------------------------------------------------------------
//
//			   IRQ Handler
//
//--------------------------------------------------------------
static irqreturn_t bcm_irq_handler(int irq, void *pdata)
{
	struct bcm_spi_priv *priv = (struct bcm_spi_priv *) pdata;

	if (!gpio_get_value(priv->host_req))
		return IRQ_HANDLED;
#ifdef DEBUG_1HZ_STAT
	{
		struct timespec ts;

		ts = ktime_to_timespec(ktime_get_boottime());
		stat1hz.ts_irq = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	}
#endif
	/* Disable irq */
	spin_lock(&priv->irq_lock);
	if (atomic_xchg(&priv->irq_enabled, 0))
		disable_irq_nosync(priv->spi->irq);

	spin_unlock(&priv->irq_lock);

	/* we don't want to queue work in suspending and shutdown */
	if (!atomic_read(&priv->suspending))
		queue_work(priv->serial_wq, (struct work_struct *)&priv->rx_work);

	return IRQ_HANDLED;
}


//--------------------------------------------------------------
//
//               SPI driver operations
//
//--------------------------------------------------------------
static int bcm_spi_suspend(struct device *dev, pm_message_t state)
{
	struct spi_device *spi = to_spi_device(dev);
	struct bcm_spi_priv *priv = (struct bcm_spi_priv *)spi_get_drvdata(spi);
	unsigned long int flags;

	pr_debug("[SSPBBD]: %s ++ \n", __func__);

	atomic_set(&priv->suspending, 1);

	/* Disable irq */
	spin_lock_irqsave(&priv->irq_lock, flags);
	if (atomic_xchg(&priv->irq_enabled, 0))
		disable_irq_nosync(spi->irq);

	spin_unlock_irqrestore(&priv->irq_lock, flags);

	if (priv->serial_wq)
		flush_workqueue(priv->serial_wq);

    ssi_pm_semaphore++;
	pr_debug("[SSPBBD]: %s -- \n", __func__);
	return 0;
}

static int bcm_spi_resume(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct bcm_spi_priv *priv = (struct bcm_spi_priv *)spi_get_drvdata(spi);
	unsigned long int flags;

	pr_debug("[SSPBBD]: %s ++ \n", __func__);
	atomic_set(&priv->suspending, 0);

	/* Enable irq */
	spin_lock_irqsave(&priv->irq_lock, flags);
	if (!atomic_xchg(&priv->irq_enabled, 1))
		enable_irq(spi->irq);

	spin_unlock_irqrestore(&priv->irq_lock, flags);

    ssi_pm_semaphore--;
	pr_debug("[SSPBBD]: %s -- \n", __func__);
	return 0;
}

static void bcm_spi_shutdown(struct spi_device *spi)
{
	struct bcm_spi_priv *priv = (struct bcm_spi_priv *) spi_get_drvdata(spi);
	unsigned long int flags;

	pr_debug("[SSPBBD]: %s ++\n", __func__);


#ifdef CONFIG_TRANSFER_STAT
	bcm_ssi_print_trans_stat(priv);
#endif

	atomic_set(&priv->suspending, 1);

	/* Disable irq */
	spin_lock_irqsave(&priv->irq_lock, flags);
	if (atomic_xchg(&priv->irq_enabled, 0))
		disable_irq_nosync(spi->irq);

	spin_unlock_irqrestore(&priv->irq_lock, flags);

	pr_debug("[SSPBBD]: %s **\n", __func__);

	flush_workqueue(priv->serial_wq);
	destroy_workqueue(priv->serial_wq);
	priv->serial_wq = NULL;

	pr_debug("[SSPBBD]: %s --\n", __func__);
}

static int bcm_spi_probe(struct spi_device *spi)
{
	int host_req, mcu_req, mcu_resp,gps_power,nstandby;
	struct bcm_spi_priv *priv;
	struct gpio_desc *desc;
	bool skip_sanity = false;
	bool legacy_patch = false;
	int ret;

	pr_debug("[SSPBBD]: gps bcm_spi_probe\n");

	/* Check GPIO# */
#ifndef CONFIG_OF
	pr_debug("[SSPBBD]: Check platform_data for bcm device\n");
#else
	if (!spi->dev.of_node) {
		pr_debug("[SSPBBD]: Failed to find of_node\n");
		goto err_exit;
	}
#endif

	host_req = of_get_named_gpio(spi->dev.of_node, "ssp-host-req", 0);
	mcu_req  = of_get_named_gpio(spi->dev.of_node, "ssp-mcu-req", 0);
	mcu_resp = of_get_named_gpio(spi->dev.of_node, "ssp-mcu-resp", 0);
	gps_power = of_get_named_gpio(spi->dev.of_node, "power_enable", 0);
	nstandby = of_get_named_gpio(spi->dev.of_node, "ssp-nstandby", 0);
	//skip_sanity = of_property_read_bool(spi->dev.of_node,
	//				"ssp-skip-sanity");
#ifdef CONFIG_SENSORS_BBD_LEGACY_PATCH
	legacy_patch = of_property_read_bool(spi->dev.of_node,
					"ssp-legacy-patch");
#endif

	pr_debug("[SSPBBD] ssp-host-req=%d, ssp-mcu_req=%d, ssp-mcu-resp=%d\n",host_req, mcu_req, mcu_resp);
	pr_debug("[SSPBBD] [Option] skip_sanity=%u, use_legacy_patch=%u\n",skip_sanity, legacy_patch);
	pr_debug("[SSPBBD] ssp-host-req=%d, ssp-mcu_req=%d, ssp-mcu-resp=%d nstandby=%d\n", host_req, mcu_req, mcu_resp,nstandby);
	//if (host_req<0 || mcu_req<0 || mcu_resp<0 ||gps_power<0||nstandby<0) {
	if (host_req<0 || mcu_req<0 || mcu_resp<0 || nstandby<0) {
		pr_debug("[SSPBBD]: GPIO value not correct\n");
		goto err_exit;
	}

	/* Check IRQ# */
	ret = gpio_request(host_req, "HOST REQ");
	if (ret) {
		pr_debug("[SSPBBD]: failed to request HOST REQ, ret:%d", ret);
		goto err_exit;
	}
    ret = gpio_direction_input(host_req);
	if (ret) {
		pr_debug("[SSPBBD]: failed set HOST REQ as input mode, ret:%d", ret);
		goto err_exit;
	}
    ret=gpio_get_value(host_req);
    pr_debug("[SSPBBD][M]: 2. ssp-host-req=%d, gpio_get_value=%d\n", host_req, ret);  // __DEBUG__

	/* Check IRQ# */
	spi->irq = gpio_to_irq(host_req);
	if (spi->irq < 0) {
		pr_debug("[SSPBBD]: irq=%d for host_req=%d not correct\n", spi->irq, host_req);
		goto err_exit;
	}

	/* Config GPIO */
	ret = gpio_request(mcu_req, "MCU REQ");
	if (ret) {
		pr_debug("[SSPBBD]: failed to request MCU REQ, ret:%d", ret);
		goto err_exit;
	}
	ret = gpio_direction_output(mcu_req, 0);
	if (ret) {
		pr_debug("[SSPBBD]: failed set MCU REQ as input mode, ret:%d", ret);
		goto err_exit;
	}
	ret = gpio_request(mcu_resp, "MCU RESP");
	if (ret) {
		pr_debug("[SSPBBD]: failed to request MCU RESP, ret:%d", ret);
		goto err_exit;
	}
	ret = gpio_direction_input(mcu_resp);
	if (ret) {
		pr_debug("[SSPBBD]: failed set MCU RESP as input mode, ret:%d", ret);
		goto err_exit;
	}

	/*loadswitch config start*/
	desc = gpio_to_desc(gps_power);
	if (!desc) {
		pr_debug("[SSPBBD]: vdd_pmu_in invalid gpio_todesc!\n");
		goto err_exit;
	}
	ret = gpio_request(gps_power, "gpio43");
	if (ret) {
		pr_debug("[SSPBBD]: vdd_pmu_in gpio_request failed!\n");
		goto err_exit;
	}
	ret = gpiod_export(desc, true);
	if (ret) {
		pr_debug("[SSPBBD]: vdd_pum_in gpoid_export failed!\n");
		goto err_exit;
	}
	ret = gpio_direction_output(gps_power, 1);
	if (ret) {
		pr_debug("[SSPBBD]: failed set GPS POWER as input mode, ret:%d", ret);
		goto err_exit;
	}

	/*nstandby config start*/
	desc = gpio_to_desc(nstandby);
	if (!desc) {
		pr_warn("[SSPBBD]: nstandby invalid gpio_todesc!");
		goto err_exit;
	}
	ret = gpio_request(nstandby, "gpio42");
	if (ret) {
		pr_warn("[SSPBBD]: nstandby gpio_request failed!\n");
		goto err_exit;
	}
	ret = gpiod_export(desc, true);
	if (ret) {
		pr_warn("[SSPBBD]: nstandby gpoid_export failed!");
		goto err_exit;
	}
	ret = gpio_direction_output(nstandby, 0);
	if (ret) {
		pr_debug("[SSPBBD]: failed set GPS NSTANDBY as out mode, ret:%d", ret);
		goto err_exit;
	}
		/*nstandby config end*/
		gpio_set_value(nstandby, 1);


	/* Alloc everything */
	priv = (struct bcm_spi_priv*) kmalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		pr_debug("[SSPBBD]: Failed to allocate memory for the BCM SPI driver\n");
		goto err_exit;
	}

	memset(priv, 0, sizeof(*priv));

	priv->skip_sanity = skip_sanity;
	priv->spi = spi;

	priv->tx_buf = kmalloc(sizeof(struct bcm_ssi_tx_frame), GFP_KERNEL);
	priv->rx_buf = kmalloc(sizeof(struct bcm_ssi_rx_frame), GFP_KERNEL);
	if (!priv->tx_buf || !priv->rx_buf) {
		pr_debug("[SSPBBD]: Failed to allocate xfer buffer. tx_buf=%p, rx_buf=%p\n",
				priv->tx_buf, priv->rx_buf);
		goto free_mem;
	}

	priv->serial_wq = alloc_workqueue("bcm477x_wq", WQ_HIGHPRI|WQ_UNBOUND|WQ_MEM_RECLAIM, 1);
	if (!priv->serial_wq) {
		pr_debug("[SSPBBD]: Failed to allocate workqueue\n");
		goto free_mem;
	}

	/* Request IRQ */
	ret = request_irq(spi->irq, bcm_irq_handler, IRQF_TRIGGER_HIGH, "ttyBCM", priv);
	if (ret) {
		pr_debug("[SSPBBD]: Failed to register BCM477x SPI TTY IRQ %d.\n", spi->irq);
		goto free_wq;
	}

	disable_irq(spi->irq);

	pr_debug("[SSPBBD]: Probe OK. ssp-host-req=%d, irq=%d, priv=0x%p\n", host_req, spi->irq, priv);

	/* Register misc device */
	priv->misc.minor = MISC_DYNAMIC_MINOR;
	priv->misc.name = "ttyBCM";
	priv->misc.fops = &bcm_spi_fops;

	ret = misc_register(&priv->misc);
	if (ret) {
		pr_debug("[SSPBBD]: Failed to register bcm_gps_spi's misc dev. err=%d\n", ret);
		goto free_irq;
	}

	/* Set driver data */
	spi_set_drvdata(spi, priv);

	/* Init - miscdev stuff */
	init_waitqueue_head(&priv->poll_wait);
	priv->read_buf.buf = priv->_read_buf;
	priv->write_buf.buf = priv->_write_buf;
	mutex_init(&priv->rlock);
	mutex_init(&priv->wlock);
	priv->busy = false;

	/* Init - work */
	INIT_WORK((struct work_struct *)&priv->rx_work, bcm_rxtx_work_func);
	priv->rx_work.type = WORK_TYPE_RX;
	priv->rx_work.priv = priv;
	INIT_WORK((struct work_struct *)&priv->tx_work, bcm_rxtx_work_func);
	priv->tx_work.type = WORK_TYPE_TX;
	priv->tx_work.priv = priv;
	/* Init - irq stuff */
	spin_lock_init(&priv->irq_lock);
	atomic_set(&priv->irq_enabled, 0);
	atomic_set(&priv->suspending, 0);

	/* Init - gpios */
	priv->host_req = host_req;
	priv->mcu_req  = mcu_req;
	priv->mcu_resp = mcu_resp;
	priv->nstandby = nstandby;

	/* Init - etc */
	//wake_lock_init(&priv->bcm_wake_lock, WAKE_LOCK_SUSPEND, "bcm_spi_wake_lock");
	//wakeup_source_init(&priv->bcm_wake_lock, "bcm_spi_wake_lock");

	g_bcm_gps = priv;
	/* Init BBD & SSP */
	bbd_init(&spi->dev, legacy_patch);
	if (device_create_file(&priv->spi->dev, &dev_attr_nstandby))
        pr_debug( "Unable to create sysfs 4775 nstandby entry");

	return 0;

free_irq:
	if (spi->irq)
		free_irq(spi->irq, priv);
free_wq:
	if (priv->serial_wq)
		destroy_workqueue(priv->serial_wq);
free_mem:
	if (priv->tx_buf)
		kfree(priv->tx_buf);
	if (priv->rx_buf)
		kfree(priv->rx_buf);
	if (priv)
		kfree(priv);
err_exit:
	return -ENODEV;
}


static int bcm_spi_remove(struct spi_device *spi)
{
	struct bcm_spi_priv *priv = (struct bcm_spi_priv *)spi_get_drvdata(spi);
	unsigned long int flags;

	pr_debug("[SSPBBD]:  %s : called\n", __func__);

	atomic_set(&priv->suspending, 1);

	bbd_exit();

	/* Disable irq */
	spin_lock_irqsave(&priv->irq_lock, flags);
	if (atomic_xchg(&priv->irq_enabled, 0))
		disable_irq_nosync(spi->irq);

	spin_unlock_irqrestore(&priv->irq_lock, flags);

	/* Flush work */
	flush_workqueue(priv->serial_wq);
	destroy_workqueue(priv->serial_wq);

	/* Free everything */
	//wakeup_source_trash(&priv->bcm_wake_lock);
	free_irq(spi->irq, priv);
	device_remove_file(&priv->spi->dev, &dev_attr_nstandby);
	kfree(priv->tx_buf);
	kfree(priv->rx_buf);
	kfree(priv);

	g_bcm_gps = NULL;

	return 0;
}

void bcm477x_debug_info(void)
{
	int pin_ttyBCM, pin_MCU_REQ, pin_MCU_RESP;
	int irq_enabled, irq_count;

	pr_debug("[SSPBBD} bcm477x_debug_info\n");
	if (g_bcm_gps) {
		pin_ttyBCM = gpio_get_value(g_bcm_gps->host_req);
		pin_MCU_REQ = gpio_get_value(g_bcm_gps->mcu_req);
		pin_MCU_RESP = gpio_get_value(g_bcm_gps->mcu_resp);

		irq_enabled = atomic_read(&g_bcm_gps->irq_enabled);
		irq_count = kstat_irqs_cpu(g_bcm_gps->spi->irq, 0);

		pr_debug("[SSPBBD]: %s pin_ttyBCM:%d, pin_MCU_REQ:%d, pin_MCU_RESP:%d\n",
				__func__, pin_ttyBCM, pin_MCU_REQ, pin_MCU_RESP);
		pr_debug("[SSPBBD]: %s irq_enabled:%d, irq_count:%d\n",
				__func__, irq_enabled, irq_count);
	} else {
		//printk("[SSPBBD]:[%s] WARN!! bcm_gps didn't yet init, or removed\n", buf);
	}
}


static const struct spi_device_id bcm_spi_id[] = {
	{"ssp-spi", 0},
	{}
};
MODULE_DEVICE_TABLE(spi, bcm_spi_id);

#ifdef CONFIG_OF
static struct of_device_id match_table[] = {
	{ .compatible = "ssp,bcm4775",},
	{},
};
#endif

static struct spi_driver bcm_spi_driver = {
	.id_table = bcm_spi_id,
	.probe = bcm_spi_probe,
	.remove = bcm_spi_remove,
	.shutdown = bcm_spi_shutdown,
	.driver = {
		.name = "brcm gps spi",
		.owner = THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = match_table,
#endif
		.suspend = bcm_spi_suspend,
		.resume = bcm_spi_resume,
	},
};


//--------------------------------------------------------------
//
//               Module init/exit
//
//--------------------------------------------------------------
static int __init bcm_spi_init(void)
{
	return spi_register_driver(&bcm_spi_driver);
}

static void __exit bcm_spi_exit(void)
{
	spi_unregister_driver(&bcm_spi_driver);
}

module_init(bcm_spi_init);
module_exit(bcm_spi_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("BCM SPI/SSI Driver");


