/*
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version 2 as published
 *  by the Free Software Foundation.
 *
 *  Copyright (C) 2015 victor yeo <s.yeo.ee@lantiq.com>
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <lantiq_soc.h>
#include <linux/cdev.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/err.h>

#define PMI_NGI_NAME "pmi-ngi"

/* ioctl definition */
#define TYPE 0xF0
#define IOCTL_PMI_READ				_IOR(TYPE, 0, struct ioctl_arg*)
#define IOCTL_PMI_WRITE				_IOW(TYPE, 1, struct ioctl_arg*)
#define IOCTL_PMI_SRAM_TEST			_IO(TYPE, 2)
#define IOCTL_PMI_SRAM_INTR_TEST	_IO(TYPE, 3)
#define IOCTL_PMI_SRAM_DDR_TEST		_IO(TYPE, 4)

/* pmi register offset */
#define PMI_COUNTER00	0x0
#define PMI_COUNTER01	0x4
#define PMI_COUNTER02	0x8
#define PMI_COUNTER03	0xC
#define PMI_COUNTER04	0x10
#define PMI_F0_MASK0	0x20
#define PMI_F0_MASK1	0x24
#define PMI_F0_PATN0	0x28
#define PMI_F0_PATN1	0x2C
#define PMI_F0_RCNF0	0x30
#define PMI_F0_RCNF1	0x34
#define PMI_COUNTER10	0x40
#define PMI_COUNTER11	0x44
#define PMI_COUNTER12	0x48
#define PMI_COUNTER13	0x4C
#define PMI_COUNTER14	0x50
#define PMI_F1_MASK0	0x60
#define PMI_F1_MASK1	0x64
#define PMI_F1_PATN0	0x68
#define PMI_F1_PATN1	0x6C
#define PMI_F1_RCNF0	0x70
#define PMI_F1_RCNF1	0x74
#define PMI_CONTROL		0x80
#define PMI_INT_CTL		0x90

/* PMI CTL bit settings */
#define EN_COUNT00		0x1
#define EN_COUNT01		0x2
#define EN_COUNT02		0x4
#define EN_COUNT03		0x8
#define EN_COUNT10		0x10
#define EN_COUNT11		0x20
#define EN_COUNT12		0x40
#define EN_COUNT13		0x80
#define EN_PS0_LN10		0x0
#define EN_PS0_LN20		0x100
#define EN_PS0_LN30		0x200
#define EN_PS0_LN06		0x0
#define EN_PS0_EX70		0x400
#define EN_PS0_SRAM		0x800

/* PMI defines */
#define WRITE			0x1
#define READ			0x0
#define WRNP			0x2
#define GROUP0			0x0
#define GROUP1			0x1
#define DDR				0x00
#define SRAM			0x2a
#define SSB				0x2a
#define LN10			0x10
#define LN20			0x15
#define LN30			0x1a
#define EX70			0x25
#define LN06			0x20
#define EN_ALL_PMI_COUNTER 0x803000ff

/* SSB defines */
#define BASE_SSB_BLK3 0x706000

#define MAX_LINE		11
#define COUNTER_GROUP_0	5

struct ioctl_arg {
	u32 address;
	u32 value;
};

struct pmi_ngi_ctrl {
	void __iomem	*membase;
	u32				pmi_ngi_irq;
	u32				phybase;
	struct device	*dev;
	struct proc_dir_entry *proc;
	char			proc_name[64];
	u32				test_result;
	unsigned int	test_cmd;
};

static struct pmi_ngi_ctrl ltq_pmi_ngi_ctrl;
static dev_t dev_num;		/* first device number */
static struct cdev c_dev;	/* character device structure */
static struct class *cl;	/* the device class */

/*
 * handy register accessor
 */
static inline u32 ltq_pmi_r32(struct pmi_ngi_ctrl *pctrl, u32 offset)
{
	return ltq_r32((u32)pctrl->membase + offset);
}

static inline void ltq_pmi_w32(u32 value, struct pmi_ngi_ctrl *pctrl,
	u32 offset)
{
	ltq_w32(value, (u32)pctrl->membase + offset);
}

/* code for test cases */
static void pmi_configure(u32 group, u32 rd_wr, u32 id)
{
	u32 offset;
	struct pmi_ngi_ctrl *pctrl;

	pctrl = &ltq_pmi_ngi_ctrl;
	if (group == 0)
		offset = 0x0;
	else
		offset = 0x40;

	if (rd_wr == WRITE) { /* monitor write events */
		/* control mask makes only first control byte and bit 6 valid
		(MCmd and SCmdAccept) */
		ltq_w32(0xff40, (u32)pctrl->membase + offset + PMI_F0_MASK0);
		/* id mask makes only MinitID valid */
		ltq_w32(0x1f00, (u32)pctrl->membase + offset + PMI_F0_MASK1);
		/* write events (MCmd=WR, SCmdAccept=1, MRespAccept=1,
		SResp=DVA; not all used) */
		ltq_w32(0x0261, (u32)pctrl->membase + offset + PMI_F0_PATN0);
		/* ID is given */
		ltq_w32(0x0000 + (id << 8) + id, (u32)pctrl->membase
			+ offset + PMI_F0_PATN1);
		/* config block added to corresponding mask block should add
		up to ffff */
		ltq_w32(0x00bf, (u32)pctrl->membase + offset + PMI_F0_RCNF0);
		/* config block added to corresponding mask block should add
		up to ffff */
		ltq_w32(0xe0ff, (u32)pctrl->membase + offset + PMI_F0_RCNF1);

	} else if (rd_wr == WRNP) { /* monitor write np events */
		/* control mask makes only first control byte and bit 6 valid
		(MCmd and SCmdAccept). */
		ltq_w32(0x002f, (u32)pctrl->membase + offset + PMI_F0_MASK0);
		/* id mask makes only MinitID valid */
		ltq_w32(0x001f, (u32)pctrl->membase + offset + PMI_F0_MASK1);
		/* write events (MCmd=WR, SCmdAccept=1, MRespAccept=1,
		SResp=DVA; not all used) */
		ltq_w32(0x2062, (u32)pctrl->membase + offset + PMI_F0_PATN0);
		ltq_w32(0x0000 + (id << 8) + id, (u32)pctrl->membase
			+ offset + PMI_F0_PATN1);
		ltq_w32(0xffd0, (u32)pctrl->membase + offset + PMI_F0_RCNF0);
		ltq_w32(0xffe0, (u32)pctrl->membase + offset + PMI_F0_RCNF1);
	} else { /* monitor read events */
		/* control mask makes only bits 5 and 3-0 valid
		(MRespAccept and SResp) */
		ltq_w32(0x002f, (u32)pctrl->membase + offset + PMI_F0_MASK0);
		/* id mask makes only SinitID valid */
		ltq_w32(0x001f, (u32)pctrl->membase + offset + PMI_F0_MASK1);
		ltq_w32(0x0462, (u32)pctrl->membase + offset + PMI_F0_PATN0);
		ltq_w32(0x0000 + (id << 8) + id, (u32)pctrl->membase
			+ offset + PMI_F0_PATN1);
		ltq_w32(0xffd0, (u32)pctrl->membase + offset + PMI_F0_RCNF0);
		ltq_w32(0xffe0, (u32)pctrl->membase + offset + PMI_F0_RCNF1);
	}
}

static void write_to_ssb()
{
	struct pmi_ngi_ctrl *pctrl;

	pctrl = &ltq_pmi_ngi_ctrl;
	ltq_w32(0xb0011234, (u32)pctrl->membase + BASE_SSB_BLK3 + 0x0);
	ltq_w32(0xb0025678, (u32)pctrl->membase + BASE_SSB_BLK3 + 0x4);
	ltq_w32(0xb0039abc, (u32)pctrl->membase + BASE_SSB_BLK3 + 0x8);
	ltq_w32(0xb0041234, (u32)pctrl->membase + BASE_SSB_BLK3 + 0x0100C);
	ltq_w32(0xb0055678, (u32)pctrl->membase + BASE_SSB_BLK3 + 0x01010);
	ltq_w32(0xb0069abc, (u32)pctrl->membase + BASE_SSB_BLK3 + 0x01014);
	ltq_w32(0xb0071234, (u32)pctrl->membase + BASE_SSB_BLK3 + 0x01FF4);
	ltq_w32(0xb0085678, (u32)pctrl->membase + BASE_SSB_BLK3 + 0x01FF8);
	ltq_w32(0xb0099abc, (u32)pctrl->membase + BASE_SSB_BLK3 + 0x01FFC);
}

static void read_from_ssb()
{
	struct pmi_ngi_ctrl *pctrl;

	pctrl = &ltq_pmi_ngi_ctrl;
	ltq_r32((u32)pctrl->membase + BASE_SSB_BLK3 + 0x0);
	ltq_r32((u32)pctrl->membase + BASE_SSB_BLK3 + 0x4);
	ltq_r32((u32)pctrl->membase + BASE_SSB_BLK3 + 0x8);
	ltq_r32((u32)pctrl->membase + BASE_SSB_BLK3 + 0x0100C);
	ltq_r32((u32)pctrl->membase + BASE_SSB_BLK3 + 0x01010);
	ltq_r32((u32)pctrl->membase + BASE_SSB_BLK3 + 0x01014);
	ltq_r32((u32)pctrl->membase + BASE_SSB_BLK3 + 0x01FF4);
	ltq_r32((u32)pctrl->membase + BASE_SSB_BLK3 + 0x01FF8);
	ltq_r32((u32)pctrl->membase + BASE_SSB_BLK3 + 0x01FFC);
}

/* end code for test cases */

/**
 * seq_start() takes a position as an argument and returns an iterator which
 * will start reading at that position.
 */
static void *seq_start(struct seq_file *s, loff_t *pos)
{
	uint32_t *lines;

	if (*pos >= MAX_LINE)
		return NULL; /* no more data to read */

	lines = kzalloc(sizeof(uint32_t), GFP_KERNEL);
	if (!lines)
		return NULL;

	*lines = *pos + 1;

	return lines;
}

/**
 * move the iterator forward to the next position in the sequence
 */
static void *seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	uint32_t *lines = v;
	*pos = ++(*lines);
	if (*pos > MAX_LINE)
		return NULL; /* no more data to read */

	return lines;
}

/**
 * stop() is called when iteration is complete (clean up)
 */
static void seq_stop(struct seq_file *s, void *v)
{
	kfree(v);
	v = NULL;
}

/**
 * success return 0, otherwise return error code
 */
static int seq_show(struct seq_file *s, void *v)
{
	u32 index = *((uint32_t *)v) - 1;
	u32 offset = 0;
	struct pmi_ngi_ctrl *pctrl;
	u32 prefix = 0;
	u32 digit;

	pctrl = s->private;
	digit = index;

	if (index >= COUNTER_GROUP_0) {
		offset = 0x2C;
		prefix = 1;
		digit = index - COUNTER_GROUP_0;
	}

	if ((index % 2) == 0)
		seq_printf(s, "Counter %d%d 0x%02x\t", prefix, digit,
			ltq_r32((u32)pctrl->membase + offset + index*4));
	else
		seq_printf(s, "Counter %d%d 0x%02x\n", prefix, digit,
			ltq_r32((u32)pctrl->membase + offset + index*4));

	if (index >= MAX_LINE-1) {
		seq_printf(s, "\nPMI_F0_MASK0 0x%04x\n",
			ltq_r32((u32)pctrl->membase + PMI_F0_MASK0));
		seq_printf(s, "PMI_F0_MASK1 0x%04x\n",
			ltq_r32((u32)pctrl->membase + PMI_F0_MASK1));
		seq_printf(s, "PMI_F0_PATN0 0x%04x\n",
			ltq_r32((u32)pctrl->membase + PMI_F0_PATN0));
		seq_printf(s, "PMI_F0_PATN1 0x%04x\n",
			ltq_r32((u32)pctrl->membase + PMI_F0_PATN1));
		seq_printf(s, "PMI_F0_RCNF0 0x%04x\n",
			ltq_r32((u32)pctrl->membase + PMI_F0_RCNF0));
		seq_printf(s, "PMI_F0_RCNF1 0x%04x\n",
			ltq_r32((u32)pctrl->membase + PMI_F0_RCNF1));
		seq_printf(s, "PMI_F1_MASK0 0x%04x\n",
			ltq_r32((u32)pctrl->membase + PMI_F1_MASK0));
		seq_printf(s, "PMI_F1_MASK1 0x%04x\n",
			ltq_r32((u32)pctrl->membase + PMI_F1_MASK1));
		seq_printf(s, "PMI_F1_PATN0 0x%04x\n",
			ltq_r32((u32)pctrl->membase + PMI_F1_PATN0));
		seq_printf(s, "PMI_F1_PATN1 0x%04x\n",
			ltq_r32((u32)pctrl->membase + PMI_F1_PATN1));
		seq_printf(s, "PMI_F1_RCNF0 0x%04x\n",
			ltq_r32((u32)pctrl->membase + PMI_F1_RCNF0));
		seq_printf(s, "PMI_F1_RCNF1 0x%04x\n",
			ltq_r32((u32)pctrl->membase + PMI_F1_RCNF1));
	}
	return 0;
}

static const struct seq_operations seq_ops = {
	.start = seq_start,
	.next  = seq_next,
	.stop  = seq_stop,
	.show  = seq_show
};

static int pmi_ngi_proc_open(struct inode *inode, struct file *file)
{
	int ret = seq_open(file, &seq_ops);

	if (ret == 0) {
		struct seq_file *m = file->private_data;
		m->private = PDE_DATA(inode);
	}
	return ret;
}

static const struct file_operations pmi_ngi_proc_fops = {
	.open = pmi_ngi_proc_open,
	.read = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release
};

static int pmi_ngi_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int pmi_ngi_read(struct file *file, char *buf, size_t count,
	loff_t *f_pc)
{
	struct device_data {
		u32 data[count/4];
		u32 buffer_size;
		u32 block_size;
	} dev;
	int i = 0;
	int retval;
	struct pmi_ngi_ctrl *pctrl;

	pctrl = &ltq_pmi_ngi_ctrl;

	dev_info(pctrl->dev, "count %d\n", count);
	for (i = 0; i < count/4; i++) {
		dev.data[i] = ltq_r32(pctrl->membase + i*4);
		if (i == COUNTER_GROUP_0)
			dev.data[i] = ltq_r32(pctrl->membase + PMI_F0_MASK0);
	}

	if (copy_to_user(buf, (dev.data), count) != 0)
		retval = -EFAULT;

	retval = count;
	return retval;
}

static int pmi_ngi_ioctl(struct file *file,	unsigned int cmd,
	unsigned long arg)
{
	int ret = 0;
	struct ioctl_arg *write_param;
	u32 data = 0x0;
	u32 offset = 0x0;
	int cnt;
	struct pmi_ngi_ctrl *pctrl;

	pctrl = &ltq_pmi_ngi_ctrl;
	pctrl->test_cmd = cmd;
	switch (cmd) {
	case IOCTL_PMI_READ:
		if (get_user(offset, (u32 __user *) arg) == 0) {
			/* check offset value */
			if (offset <= PMI_INT_CTL && offset >= PMI_COUNTER00) {
				data = ltq_r32(pctrl->membase + offset);
				put_user(data, (u32 __user *) (arg+0x4));
			} else
				ret = -EINVAL;
		} else {
			dev_err(pctrl->dev, "get 0x%x from user space\n", arg);
			ret = -EINVAL;
		}
		break;
	case IOCTL_PMI_WRITE:
		if (get_user(offset, (u32 __user *) arg) == 0) {
			if (get_user(data, (u32 __user *) (arg+0x4)) == 0) {
				if (offset <= PMI_INT_CTL
					&& offset >= PMI_COUNTER00) {
					ltq_w32(data, pctrl->membase + offset);
				} else
					ret = -EINVAL;
			} else
				ret = -EINVAL;
		} else {
			dev_err(pctrl->dev, "get 0x%x from user space\n", arg);
			ret = -EINVAL;
		}
		break;
	case IOCTL_PMI_SRAM_TEST:
		/* enable all PMI counters */
		ltq_w32(EN_ALL_PMI_COUNTER + (SRAM << 14) + (SRAM << 8),
			(u32)pctrl->membase + PMI_CONTROL);
		/* configure PMI groups to WR/RD and ID = 0 */
		pmi_configure(GROUP0, WRITE, 0);
		pmi_configure(GROUP1, READ, 0);

		/* set counter to 0 */
		ltq_w32(0x0, (u32)pctrl->membase + 0x0);
		/* write to ssb */
		write_to_ssb();
		/* read from ssb */
		read_from_ssb();

		ret = ltq_r32((u32)pctrl->membase + 0x0);
		dev_info(pctrl->dev, "counter 00: 0x%x\n", ret);
		break;
	case IOCTL_PMI_SRAM_INTR_TEST:
		/* enable all PMI counters */
		ltq_w32(EN_ALL_PMI_COUNTER + (SRAM << 14) + (SRAM << 8),
			(u32)pctrl->membase + PMI_CONTROL);
		/* configure PMI groups to WR/RD and ID = 0 */
		pmi_configure(GROUP0, WRITE, 0);
		pmi_configure(GROUP1, READ, 0);

		/* set counter to almost overflow */
		ltq_w32(0xfffffffd, (u32)pctrl->membase + 0x0);
		/* write to ssb */
		write_to_ssb();
		/* read from ssb */
		read_from_ssb();

		ret = ltq_r32((u32)pctrl->membase + 0x0);
		dev_info(pctrl->dev, "counter 00: 0x%x\n", ret);

		ret  = pctrl->test_result;
		break;
	}

	return ret;
}

static const struct file_operations pmi_ngi_fops = {
	.owner = THIS_MODULE,
	.open = pmi_ngi_open,
	.read = pmi_ngi_read,
	.unlocked_ioctl = pmi_ngi_ioctl,
};

static int pmi_ngi_proc_init(struct pmi_ngi_ctrl *pctrl)
{
	struct proc_dir_entry *entry;

	strcpy(pctrl->proc_name, "driver/pmi-ngi");

	pctrl->proc = proc_mkdir(pctrl->proc_name, NULL);
	if (!pctrl->proc)
		return -ENOMEM;

	entry = proc_create_data("pmi_ngi_info", 0, pctrl->proc,
		&pmi_ngi_proc_fops, pctrl);
	if (!entry)
		goto __port_proc_err;

	return 0;

__port_proc_err:
	remove_proc_entry(pctrl->proc_name, NULL);
	return -ENOMEM;
}

static int pmi_ngi_remove(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "pmi-ngi driver: Remove Done !!");

	return 0;
}

static irqreturn_t pmi_ngi_interrupt(int irq, void *dev_id)
{
	struct pmi_ngi_ctrl *pctrl = (struct pmi_ngi_ctrl *)dev_id;
	u32 value;
	int index;
	int offset = 0;

	/* disable PMI */
	ltq_w32(0x0, (u32)pctrl->membase + PMI_CONTROL);

	/* check which counter triggered intr */
	for (index = 0; index < MAX_LINE-1; index++) {
		if (index >= COUNTER_GROUP_0)
			offset = 0x2C;
		value = ltq_r32((u32)pctrl->membase + offset + index*4);

		/* clear intr */
		ltq_w32(0x7FFFFFFF & value,
			(u32)pctrl->membase + offset + index*4);
	}

	if (pctrl->test_cmd == IOCTL_PMI_SRAM_INTR_TEST) {
		/* enable all PMI counters and sets both MUX to SRAM */
		ltq_w32(EN_ALL_PMI_COUNTER + (SRAM << 14) + (SRAM << 8),
			(u32)pctrl->membase + PMI_CONTROL);

		pctrl->test_result = 0;
	} else
		ltq_w32(EN_ALL_PMI_COUNTER, (u32)pctrl->membase + PMI_CONTROL);

	return IRQ_HANDLED;
}

static int pmi_ngi_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct resource *res;
	struct device_node *node = pdev->dev.of_node;
	struct pmi_ngi_ctrl *pctrl;
	struct resource irqres;

	struct device *pmi_device = NULL;

	pctrl = &ltq_pmi_ngi_ctrl;
	memset(pctrl, 0, sizeof(ltq_pmi_ngi_ctrl));

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		panic("Failed to get pmi-ngi resources\n");

	pctrl->phybase = res->start;
	pctrl->membase = devm_ioremap_resource(&pdev->dev, res);

	if (!pctrl->membase)
		panic("Failed to remap pmi-ngi resources\n");

	ret = of_irq_to_resource_table(node, &irqres, 1);
	if (ret != 1)
		panic("Failed to get irq NO");

	pctrl->pmi_ngi_irq = irqres.start;

	dev_info(&pdev->dev, "pmi-ngi intr %d\t", pctrl->pmi_ngi_irq);
	dev_info(&pdev->dev, "base address: 0x%x\t", (u32)pctrl->membase);
	dev_info(&pdev->dev, "PHY base address: 0x%x\n", pctrl->phybase);

	pctrl->dev = &pdev->dev;

    /* Link platform with driver data for retrieving */
	platform_set_drvdata(pdev, pctrl);
	pmi_ngi_proc_init(pctrl);

	/* register char dev */
	ret = alloc_chrdev_region(&dev_num, 0, 1, PMI_NGI_NAME);
	if (ret < 0) {
		pr_err("pmi-ngi alloc_chrdev_region failed\n");
		return ret;
	} else
		pr_alert("device number :%d\n", MAJOR(dev_num));

	cl = class_create(THIS_MODULE, PMI_NGI_NAME);
	if (IS_ERR(cl))  {
		pr_err("pmi-ngi Class Create failed\n");
		return -ENODEV;
	}

	pmi_device = device_create(cl, NULL /*no parent device*/, dev_num,
		NULL, PMI_NGI_NAME"%d", MINOR(dev_num));
	if (IS_ERR(pmi_device)) {
		pr_err("pmi-ngi Device Registration failed\n");
		return -ENODEV;
	}
	cdev_init(&c_dev, &pmi_ngi_fops);

	ret = cdev_add(&c_dev, dev_num, 1);
	if (ret)
		pr_err("pmi-ngi Device addition failed\n");

	/* register irq */
	if (pctrl->pmi_ngi_irq != 0) {
		dev_info(pctrl->dev, "pmi-ngi : register IRQ '%d'\n",
			pctrl->pmi_ngi_irq);
		ret = devm_request_irq(pctrl->dev, pctrl->pmi_ngi_irq,
			pmi_ngi_interrupt, 0, PMI_NGI_NAME, (void *)pctrl);
		if (ret) {
			dev_err(pctrl->dev,
				"pmi-ngi : cannot register IRQ '%d'\n",
				pctrl->pmi_ngi_irq);
			return ret;
		}
	}
	/* enable all interrupt */
	ltq_w32(0x7ff, (u32)pctrl->membase + PMI_INT_CTL);

	dev_info(pctrl->dev, "pmi-ngi driver : init done !!\n");

	return 0;
}

static const struct of_device_id pmi_xrx500_match[] = {
	{ .compatible = "lantiq,pmi-xrx500" },
	{},
};

static struct platform_driver pmi_ngi_driver = {
	.remove     = pmi_ngi_remove,
	.probe      = pmi_ngi_probe,
	.driver = {
		.name   = "pmi-ngi",
		.owner = THIS_MODULE,
		.of_match_table = pmi_xrx500_match,
	},
};

static int __init pmi_ngi_init(void)
{
	int rc;
	int ret = 0;
	struct device *pmi_device = NULL;
	struct pmi_ngi_ctrl *pctrl;

	pr_alert("try to register pmi-ngi driver");

	rc = platform_driver_register(&pmi_ngi_driver);
	if (!rc)
		pr_alert("pmi-ngi driver registered\n");

	return rc;
}
module_init(pmi_ngi_init);

static void __exit pmi_ngi_exit(void)
{
	struct pmi_ngi_ctrl *pctrl;

	cdev_del(&c_dev);
	device_destroy(cl, dev_num);
	class_destroy(cl);
	pctrl = &ltq_pmi_ngi_ctrl;
	remove_proc_entry("pmi_ngi_info", pctrl->proc);
	remove_proc_entry(pctrl->proc_name, NULL);

	unregister_chrdev_region(dev_num, 1);

	if (pctrl->pmi_ngi_irq != 0)
		devm_free_irq(pctrl->dev, pctrl->pmi_ngi_irq, (void *)pctrl);

	platform_driver_unregister(&pmi_ngi_driver);
}
module_exit(pmi_ngi_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PMI NGI driver");
MODULE_AUTHOR("lantiq");
MODULE_ALIAS("platform : pmi-ngi");
