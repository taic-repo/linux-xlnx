/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Xilinx SYSMON for Versal
 *
 * Copyright (C) 2019 - 2022, Xilinx, Inc.
 * Copyright (C) 2022 - 2024, Advanced Micro Devices, Inc.
 *
 * Description:
 * This driver is developed for SYSMON on Versal. The driver supports INDIO Mode
 * and supports voltage and temperature monitoring via IIO sysfs interface.
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iio/buffer.h>
#include <linux/iio/driver.h>
#include <linux/iio/events.h>
#include <linux/iio/machine.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/adc/versal-sysmon-events.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/of_address.h>

/* Channel IDs for Temp Channels */
/* TEMP_MAX gives the current temperature for Production
 * silicon.
 * TEMP_MAX gives the current maximum temperature for ES1
 * silicon.
 */
#define TEMP_MAX	160

/* TEMP_MIN is not applicable for Production silicon.
 * TEMP_MIN gives the current minimum temperature for ES1 silicon.
 */
#define TEMP_MIN	161

#define TEMP_MAX_MAX	162
#define TEMP_MIN_MIN	163
#define TEMP_EVENT	164
#define OT_EVENT	165
#define TEMP_HBM	166

/* Register Unlock Code */
#define NPI_UNLOCK	0xF9E8D7C6

/* Register Offsets */
#define SYSMON_NPI_LOCK		0x000C
#define SYSMON_ISR		0x0044
#define SYSMON_CONFIG		0x0100
#define SYSMON_TEMP_MASK	0x300
#define SYSMON_IMR		0x0048
#define SYSMON_IER		0x004C
#define SYSMON_IDR		0x0050
#define SYSMON_ALARM_FLAG	0x1018
#define SYSMON_TEMP_MAX		0x1030
#define SYSMON_TEMP_MIN		0x1034
#define SYSMON_SUPPLY_BASE	0x1040
#define SYSMON_ALARM_REG	0x1940
#define SYSMON_TEMP_TH_LOW	0x1970
#define SYSMON_TEMP_TH_UP	0x1974
#define SYSMON_OT_TH_LOW	0x1978
#define SYSMON_OT_TH_UP		0x197C
#define SYSMON_SUPPLY_TH_LOW	0x1980
#define SYSMON_SUPPLY_TH_UP	0x1C80
#define SYSMON_TEMP_MAX_MAX	0x1F90
#define SYSMON_TEMP_MIN_MIN	0x1F8C
#define SYSMON_TEMP_HBM	0x0000
#define SYSMON_TEMP_EV_CFG	0x1F84
#define SYSMON_NODE_OFFSET	0x1FAC
#define SYSMON_STATUS_RESET	0x1F94
#define SYSMON_SUPPLY_EN_AVG_OFFSET	0x1958
#define SYSMON_TEMP_SAT_EN_AVG_OFFSET	0x24B4

/* Average Sampling Rate macros */
#define SYSMON_AVERAGE_FULL_SAMPLE_RATE	0 /* Full sample rate */
#define SYSMON_AVERAGE_2_SAMPLE_RATE	1 /* Full sample rate/2 */
#define SYSMON_AVERAGE_4_SAMPLE_RATE	2 /* Full sample rate/4 */
#define SYSMON_AVERAGE_8_SAMPLE_RATE	4 /* Full sample rate/8 */
#define SYSMON_AVERAGE_16_SAMPLE_RATE	8 /* Full sample rate/16 */

#define SYSMON_TEMP_SAT_IDX_FIRST	1
#define SYSMON_TEMP_SAT_IDX_MAX		64
#define SYSMON_TEMP_SAT_COUNT		64
#define SYSMON_SUPPLY_IDX_MAX		159

#define SYSMON_SUPPLY_CONFIG_MASK	GENMASK(17, 14)
#define SYSMON_SUPPLY_CONFIG_SHIFT	14
#define SYSMON_TEMP_SAT_CONFIG_MASK	GENMASK(27, 24)
#define SYSMON_TEMP_SAT_CONFIG_SHIFT	24

#define SYSMON_NO_OF_EVENTS	32

/* Supply Voltage Conversion macros */
#define SYSMON_MANTISSA_MASK		0xFFFF
#define SYSMON_FMT_MASK			0x10000
#define SYSMON_FMT_SHIFT		16
#define SYSMON_MODE_MASK		0x60000
#define SYSMON_MODE_SHIFT		17
#define SYSMON_MANTISSA_SIGN_SHIFT	15
#define SYSMON_UPPER_SATURATION_SIGNED	32767
#define SYSMON_LOWER_SATURATION_SIGNED	-32768
#define SYSMON_UPPER_SATURATION		65535
#define SYSMON_LOWER_SATURATION		0

#define SYSMON_MILLI_SCALE		1000

#define SYSMON_CHAN_TEMP_EVENT(_address, _ext, _events) { \
	.type = IIO_TEMP, \
	.indexed = 1, \
	.address = _address, \
	.channel = _address, \
	.event_spec = _events, \
	.num_event_specs = ARRAY_SIZE(_events), \
	.scan_type = { \
		.sign = 's', \
		.realbits = 15, \
		.storagebits = 16, \
		.endianness = IIO_CPU, \
	}, \
	.extend_name = _ext, \
	}

#define SYSMON_CHAN_TEMP(_address, _ext) { \
	.type = IIO_TEMP, \
	.indexed = 1, \
	.address = _address, \
	.channel = _address, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | \
		BIT(IIO_CHAN_INFO_PROCESSED), \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO), \
	.info_mask_shared_by_type_available = \
		BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO), \
	.scan_type = { \
		.sign = 's', \
		.realbits = 15, \
		.storagebits = 16, \
		.endianness = IIO_CPU, \
	}, \
	.extend_name = _ext, \
	.datasheet_name = _ext,\
}

#define SYSMON_CHAN_TEMP_HBM(_address, _ext) { \
	.type = IIO_TEMP, \
	.indexed = 1, \
	.address = _address, \
	.channel = _address, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | \
			BIT(IIO_CHAN_INFO_PROCESSED), \
	.scan_type = { \
		.sign = 's', \
		.realbits = 15, \
		.storagebits = 16, \
		.endianness = IIO_CPU, \
	}, \
	.extend_name = _ext, \
	.datasheet_name = _ext, \
}

#define twoscomp(val) ((((val) ^ 0xFFFF) + 1) & 0x0000FFFF)
#define REG32_OFFSET(address) (4 * ((address) / 32))
#define REG32_SHIFT(address) ((address) % 32)

#define compare(val, thresh) (((val) & 0x8000) || ((thresh) & 0x8000) ? \
			      ((val) < (thresh)) : ((val) > (thresh)))  \

enum sysmon_alarm_bit {
	SYSMON_BIT_ALARM0 = 0,
	SYSMON_BIT_ALARM1 = 1,
	SYSMON_BIT_ALARM2 = 2,
	SYSMON_BIT_ALARM3 = 3,
	SYSMON_BIT_ALARM4 = 4,
	SYSMON_BIT_ALARM5 = 5,
	SYSMON_BIT_ALARM6 = 6,
	SYSMON_BIT_ALARM7 = 7,
	SYSMON_BIT_OT = 8,
	SYSMON_BIT_TEMP = 9,
};

static const unsigned int sysmon_oversampling_avail[5] = {
	SYSMON_AVERAGE_FULL_SAMPLE_RATE,
	SYSMON_AVERAGE_2_SAMPLE_RATE,
	SYSMON_AVERAGE_4_SAMPLE_RATE,
	SYSMON_AVERAGE_8_SAMPLE_RATE,
	SYSMON_AVERAGE_16_SAMPLE_RATE,
};

/**
 * struct sysmon - Driver data for Sysmon
 * @base: physical base address of device
 * @dev: pointer to device struct
 * @indio_dev: pointer to the iio device
 * @client: pointer to the i2c client
 * @mutex: to handle multiple user interaction
 * @lock: to help manage interrupt registers correctly
 * @irq: interrupt number of the sysmon
 * @region_list: list of the regions of sysmon
 * @list: list of sysmon instances
 * @masked_temp: currently masked due to alarm
 * @temp_mask: temperature based interrupt configuration
 * @sysmon_unmask_work: re-enables event once the event condition disappears
 * @sysmon_events_work: poll for events on SSIT slices
 * @ops: read write operations for sysmon registers
 * @pm_info: plm address of sysmon
 * @master_slr: to keep master sysmon info
 * @hbm_slr: flag if HBM slr is present
 * @temp_oversampling: current oversampling ratio for temperature satellites
 * @supply_oversampling: current oversampling ratio for supply nodes
 * @oversampling_avail: list of available overampling ratios
 * @oversampling_num: total number of available oversampling ratios
 * @num_supply_chan: number of supply channels that are enabled
 * @supply_avg_en_attrs: dynamic array of supply averaging enable attributes
 * @temp_avg_en_attrs: dynamic array of temp. sat. averaging enable attributes
 * @avg_attrs: dynamic array of pointers to averaging attributes
 * @avg_attr_group: attribute group for averaging
 * @temp_read: function pointer for the special temperature read
 *
 * This structure contains necessary state for Sysmon driver to operate
 */
struct sysmon {
	void __iomem *base;
	struct device *dev;
	struct iio_dev *indio_dev;
	struct i2c_client *client;
	/* kernel doc above */
	struct mutex mutex;
	/* kernel doc above*/
	spinlock_t lock;
	int irq;
	struct list_head region_list;
	struct list_head list;
	unsigned int masked_temp;
	unsigned int temp_mask;
	struct delayed_work sysmon_unmask_work;
	struct delayed_work sysmon_events_work;
	struct sysmon_ops *ops;
	u32 pm_info;
	bool master_slr;
	bool hbm_slr;
	unsigned int temp_oversampling;
	unsigned int supply_oversampling;
	const unsigned int *oversampling_avail;
	unsigned int oversampling_num;
	unsigned int num_supply_chan;
	struct iio_dev_attr *supply_avg_en_attrs;
	struct iio_dev_attr *temp_avg_en_attrs;
	struct attribute **avg_attrs;
	struct attribute_group avg_attr_group;
	int (*temp_read)(struct sysmon *sysmon, int offset);
};

struct sysmon_ops {
	int (*read_reg)(struct sysmon *sysmon, u32 offset, u32 *data);
	void (*write_reg)(struct sysmon *sysmon, u32 offset, u32 data);
	void (*update_reg)(struct sysmon *sysmon, u32 offset,
			   u32 mask, u32 data);
};

int sysmon_register_temp_ops(void (*cb)(void *data, struct regional_node *node),
			     void *data, enum sysmon_region region_id);
int sysmon_unregister_temp_ops(enum sysmon_region region_id);
struct list_head *sysmon_nodes_by_region(enum sysmon_region region_id);
int sysmon_get_node_value(int sat_id);
int sysmon_parse_dt(struct iio_dev *indio_dev, struct device *dev);
int sysmon_init_interrupt(struct sysmon *sysmon);
int sysmon_read_reg(struct sysmon *sysmon, u32 offset, u32 *data);
void sysmon_write_reg(struct sysmon *sysmon, u32 offset, u32 data);
void sysmon_set_iio_dev_info(struct iio_dev *indio_dev);
int sysmon_create_avg_en_sysfs_entries(struct iio_dev *indio_dev);
