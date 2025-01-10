#define pr_fmt(fmt) "riscv-taic: " fmt

#include <linux/cpu.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#include <asm/smp.h>
#include <asm/uintr.h>

#define DEFAULT_GQ_NUM 4
#define DEFAULT_LQ_NUM 8
#define LQ_OFFSET 0x1000
#define LQ_SIZE 0x1000


struct taic_priv {
	struct cpumask smask;
	struct cpumask umask;
	void __iomem *regs;
	resource_size_t start;
	resource_size_t size;
    u8 lq_num;
    u8 gq_num;
	spinlock_t lock;
};

struct taic_handler {
	bool present;
	struct taic_priv *priv;
};

static DEFINE_PER_CPU(struct taic_handler, taic_shandlers);
static DEFINE_PER_CPU(struct taic_handler, taic_uhandlers);

static int __init __taic_init(struct device_node *node,
			       struct device_node *parent)
{
	int error = 0, nr_contexts, i, ret;
	struct taic_priv *priv;
	struct taic_handler *handler;
	struct resource taic_res;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	if (of_address_to_resource(node, 0, &taic_res)) {
		error = -EIO;
		goto out_free;
	}

	priv->start = taic_res.start;
	priv->size = resource_size(&taic_res);

	priv->regs = ioremap(taic_res.start, priv->size);
	if (WARN_ON(!priv->regs)) {
		error = -EIO;
		goto out_free;
	}

    ret = of_property_read_u8(node, "gq-num", &priv->gq_num);
    if (ret) {
        pr_warn("failed to parse gq-num, using default value %d\n", DEFAULT_GQ_NUM);
        priv->gq_num = DEFAULT_GQ_NUM;
    }
    ret = of_property_read_u8(node, "lq-num", &priv->lq_num);
    if (ret) {
        pr_warn("failed to parse lq-num, using default value %d\n", DEFAULT_LQ_NUM);
        priv->lq_num = DEFAULT_LQ_NUM;
    }

	spin_lock_init(&priv->lock);

	error = -EINVAL;
	nr_contexts = of_irq_count(node);
	if (WARN_ON(!nr_contexts))
		goto out_iounmap;

	for (i = 0; i < nr_contexts; i++) {
		struct of_phandle_args parent;
		int cpu;
		unsigned long hartid;

		if (of_irq_parse_one(node, i, &parent)) {
			pr_err("failed to parse parent for context %d.\n", i);
			continue;
		}

		if (parent.args[0] != IRQ_U_SOFT && parent.args[0] != IRQ_S_SOFT) {
			continue;
		}

		error = riscv_of_parent_hartid(parent.np, &hartid);
		if (error < 0) {
			pr_warn("failed to parse hart ID for context %d.\n", i);
			continue;
		}

		cpu = riscv_hartid_to_cpuid(hartid);
		if (cpu < 0) {
			pr_warn("invalid cpuid for context %d.\n", i);
			continue;
		}
        if(parent.args[0] == IRQ_U_SOFT) {
            handler = per_cpu_ptr(&taic_uhandlers, cpu);
			cpumask_set_cpu(cpu, &priv->umask);
        } else {
            handler = per_cpu_ptr(&taic_shandlers, cpu);
			cpumask_set_cpu(cpu, &priv->smask);
        }

		if (handler->present) {
			pr_warn("handler already present for context %d.\n", i);
			continue;
		}

		handler->present = true;
		handler->priv = priv;
	}

	pr_info("%pOFP: %d gq_num %d lq_num available\n", node, priv->gq_num, priv->lq_num);
	return 0;

out_iounmap:
	iounmap(priv->regs);
out_free:
	kfree(priv);
	return error;
}

IRQCHIP_DECLARE(riscv_taic, "riscv,taic0", __taic_init);


int taic_ulq_write_cpuid(unsigned long lq_idx, unsigned long hartid) {
    struct taic_handler *handler;
    u64 __iomem *reg;

	handler = this_cpu_ptr(&taic_uhandlers);

    if (!handler->present) {
        return -EINVAL;
    }
    uint64_t idx_high = lq_idx >> 32;
    uint64_t idx_low = lq_idx & 0xffffffff;
    uint64_t lq_num = handler->priv->lq_num;

    reg = handler->priv->regs + LQ_OFFSET + (idx_high * lq_num + idx_low) * LQ_SIZE + 0x38;

    writeq(hartid, reg);
    return 0;
}

int taic_free_lq(unsigned long lq_idx) {
	struct taic_handler *handler;
    u64 __iomem *reg;

	handler = this_cpu_ptr(&taic_uhandlers);

    if (!handler->present) {
        return -EINVAL;
    }

    reg = handler->priv->regs + 0x8;

    writeq(lq_idx, reg);
    return 0;
}
