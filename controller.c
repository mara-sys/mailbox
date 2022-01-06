#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/pm_wakeirq.h>

#define CPU2DSP_INT_EN          0x00
#define CPU2DSP_INT_SET         0x04
#define CPU2DSP_INT_CLEAR       0x08
#define CPU2DSP_INT_STATUS      0x0c
#define CPU2DSP_INT_ERR         0x10
#define DSP2CPU_INT_SET         0x14
#define DSP2CPU_INT_CLEAR       0x18
#define DSP2CPU_INT_EN          0x1c
#define DSP2CPU_INT_STATUS      0x20
#define DSP2CPU_INT_ERR         0x24

#define MAILBOX_0               0xb0
#define MAILBOX_1               0xb4
#define MAILBOX_2               0xb8
#define MAILBOX_3               0xbc
#define MAILBOX_4               0xc0
#define MAILBOX_5               0xc4               
#define MAILBOX_6               0xc8
#define MAILBOX_7               0xcc
#define MAILBOX_8               0xd0
#define MAILBOX_9               0xd4
#define MAILBOX_10              0xd8
#define MAILBOX_11              0xdc
#define MAILBOX_12              0xe0
#define MAILBOX_13              0xe4
#define MAILBOX_14              0xe8
#define MAILBOX_15              0xec

#define MAILBOX_INTERRUPT_NUMBER    16
#define MAILBOX_INT_EN              (1 << 0)
#define MAILBOX_INT_RST             (1 << 1)
#define MAILBOX_RAW_EN              (0xFFFF << 16)

#define DSP_SEND_INT_MASK           (0x0000FFFF)
#define DSP_REPLY_INT_MASK          (0xFFFF0000)

#define SINGLE_DIR_CHAN_NUM     8

struct canaan_mailbox {
    struct device *dev;
    void __iomem *base;
    struct mbox_chan chan[SINGLE_DIR_CHAN_NUM*2];
    struct mbox_controller controller;
    struct clk *clk;
    int irq;
    spinlock_t lock;
};

static struct canaan_mailbox *to_canaan_mailbox(struct mbox_controller *mbox)
{
    return container_of(mbox, struct canaan_mailbox, controller);
}

static void mailbox_cpu2dsp_int_enable(struct canaan_mailbox *mbox)
{
    writel(MAILBOX_RAW_EN | MAILBOX_INT_EN, mbox->base + CPU2DSP_INT_EN);
}

static void mailbox_dsp2cpu_int_enable(struct canaan_mailbox *mbox)
{
    writel(MAILBOX_RAW_EN | MAILBOX_INT_EN, mbox->base + DSP2CPU_INT_EN);
}

static u32 get_chan_number(u32 reg_value)
{
    int i;
    for (i = 0; i < MAILBOX_INTERRUPT_NUMBER; i++)
    {
        if((reg_value >> (i*2)) & 0x3)
            return i;
    }
}

static irqreturn_t canaan_mailbox_irq(int irq, void *data)
{
    struct canaan_mailbox *mbox = data;
    unsigned int chan_number;
    u32 reg_value;
    
    reg_value = readl(mbox->base + DSP2CPU_INT_STATUS);
    chan_number = get_chan_number(reg_value);
    writel(chan_number, mbox->base + DSP2CPU_INT_CLEAR);
    writel(chan_number, mbox->base + DSP2CPU_INT_CLEAR);
    writel(chan_number, mbox->base + DSP2CPU_INT_CLEAR);
    // printk("[%s,%d], reg_value: %x, chan_number: %d", __func__, __LINE__, reg_value, chan_number);

    if (chan_number >= SINGLE_DIR_CHAN_NUM)
    {
        if (!mbox->chan[chan_number - SINGLE_DIR_CHAN_NUM].cl)
        {
            dev_err(mbox->dev, "illegal tx channel\n");
            return -ENODEV;
        }
        // printk("[%s,%d], chan_number: %d", __func__, __LINE__, chan_number);
        mbox_chan_txdone(&mbox->chan[chan_number - SINGLE_DIR_CHAN_NUM], 0);
    }
    else
    {
        if (!mbox->chan[chan_number + SINGLE_DIR_CHAN_NUM].cl)
        {
            dev_err(mbox->dev, "illegal rx channel\n");
            return IRQ_HANDLED;
        }
        mbox_chan_received_data(&mbox->chan[chan_number + SINGLE_DIR_CHAN_NUM], NULL);
        writel(chan_number + SINGLE_DIR_CHAN_NUM, mbox->base + CPU2DSP_INT_SET);
    }

    return IRQ_HANDLED;
}

static int canaan_mailbox_send_data(struct mbox_chan *chan, void *data)
{
    unsigned int chan_number = (unsigned int)chan->con_priv;
    struct canaan_mailbox *mbox = to_canaan_mailbox(chan->mbox);

    if (chan_number > SINGLE_DIR_CHAN_NUM)
    {
        dev_err(mbox->dev, "tx channel: 0-8, current channel number: %d\n", chan_number);
        return -ENODEV;
    }
    /* Notify that the transmission is complete */
    writel(chan_number, mbox->base + CPU2DSP_INT_SET);
    // printk("[%s,%d], reg_value:%lx", __func__, __LINE__, readl(mbox->base + CPU2DSP_INT_STATUS));

    return 0;
}

static int canaan_mailbox_startup(struct mbox_chan *chan)
{

    return 0;
}

static void canaan_mailbox_shutdown(struct mbox_chan *chan)
{

}

static struct mbox_chan *canaan_mailbox_xlate(struct mbox_controller *controller,
                        const struct of_phandle_args *spec)
{
    struct canaan_mailbox *mbox = to_canaan_mailbox(controller);
    unsigned int ch = spec->args[0];

    if (ch >= SINGLE_DIR_CHAN_NUM * 2)
    {
        dev_err(mbox->dev, "Invalid channel index %d\n", ch);
        return ERR_PTR(-EINVAL);
    }

    return &mbox->chan[ch];
}

static const struct mbox_chan_ops canaan_mailbox_ops = {
    .send_data  = canaan_mailbox_send_data,
    .startup    = canaan_mailbox_startup,
    .shutdown   = canaan_mailbox_shutdown,    
};

static int canaan_mailbox_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct device_node *np = dev->of_node;
    struct canaan_mailbox *priv;
    struct resource *res;
    unsigned int i;
    int ret;

    if (!np)
    {
        dev_err(dev, "No DT found\n");
        return -ENOMEM;
    }

    priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    spin_lock_init(&priv->lock);

    priv->dev = dev;

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    priv->base = devm_ioremap_resource(dev, res);
    if (IS_ERR(priv->base))
        return PTR_ERR(priv->base);

    priv->clk = devm_clk_get(dev, NULL);
    if (IS_ERR(priv->clk))
        return PTR_ERR(priv->clk);
    
    ret = clk_prepare_enable(priv->clk);
    if (ret)
    {
        dev_err(dev, "can not enable the clock\n");
        return ret;
    }

    priv->irq = platform_get_irq(pdev, 0);
    if (priv->irq < 0)
    {
        ret = priv->irq;
        goto err_clk;
    }

    ret = devm_request_irq(&pdev->dev, priv->irq, canaan_mailbox_irq,
			       0, dev_name(&pdev->dev), priv);
    if (ret)
    {
        dev_err(dev, "failed to request irq %d \n", ret);
        goto err_clk;
    }

    priv->controller.dev = dev;
    priv->controller.ops = &canaan_mailbox_ops;
    priv->controller.chans = priv->chan;
    priv->controller.num_chans = SINGLE_DIR_CHAN_NUM * 2;
    priv->controller.txdone_irq = true;
    priv->controller.of_xlate = canaan_mailbox_xlate;

    /* initialize mailbox channel data */
    for (i = 0; i < priv->controller.num_chans; i++)
        priv->chan[i].con_priv = (void *)i;

    ret = mbox_controller_register(&priv->controller);
    if (ret)
    {
        dev_err(dev, "Failed to register mailbox %d\n", ret);
        return ret;
    }

    /* enable irq */
    mailbox_cpu2dsp_int_enable(priv);
    mailbox_dsp2cpu_int_enable(priv);


    platform_set_drvdata(pdev, priv);
    dev_info(dev, "Mailbox enabled\n");

    return 0;


err_clk:
    clk_disable_unprepare(priv->clk);
    return ret;
}

static int canaan_mailbox_remove(struct platform_device *pdev)
{
    struct canaan_mailbox *priv = platform_get_drvdata(pdev);

    mbox_controller_unregister(&priv->controller);
    clk_disable_unprepare(priv->clk);
    dev_info(&pdev->dev, "Mailbox disabled\n");

    return 0;
}

static const struct of_device_id canaan_mailbox_dt_ids[] = {
    { .compatible = "canaan,k510-mailbox" },
    {   },
};


static struct platform_driver canaan_mailbox_driver = {
    .probe = canaan_mailbox_probe,
    .remove = canaan_mailbox_remove,
    .driver = {
        .name = "canaan_mailbox",
        .of_match_table = canaan_mailbox_dt_ids,
    },
};
module_platform_driver(canaan_mailbox_driver);

MODULE_AUTHOR("lst");
MODULE_DESCRIPTION("mailbox driver for canaan k510");
MODULE_LICENSE("GPL");