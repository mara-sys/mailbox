#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sched/signal.h>
#include <linux/cdev.h>
#include <linux/device.h>

#define MBOX_CHAN_0_TX          _IOW('m', 0, unsigned long)
#define MBOX_CHAN_1_TX          _IOW('m', 1, unsigned long)
#define MBOX_CHAN_2_TX          _IOW('m', 2, unsigned long)
#define MBOX_CHAN_3_TX          _IOW('m', 3, unsigned long)
#define MBOX_CHAN_4_TX          _IOW('m', 4, unsigned long)
#define MBOX_CHAN_5_TX          _IOW('m', 5, unsigned long)
#define MBOX_CHAN_6_TX          _IOW('m', 6, unsigned long)
#define MBOX_CHAN_7_TX          _IOW('m', 7, unsigned long)
#define MBOX_CHAN_0_RX          _IOR('m', 0, unsigned long)
#define MBOX_CHAN_1_RX          _IOR('m', 1, unsigned long)
#define MBOX_CHAN_2_RX          _IOR('m', 2, unsigned long)
#define MBOX_CHAN_3_RX          _IOR('m', 3, unsigned long)
#define MBOX_CHAN_4_RX          _IOR('m', 4, unsigned long)
#define MBOX_CHAN_5_RX          _IOR('m', 5, unsigned long)
#define MBOX_CHAN_6_RX          _IOR('m', 6, unsigned long)
#define MBOX_CHAN_7_RX          _IOR('m', 7, unsigned long)

#define MBOX_MAX_MSG_LEN        32
#define SINGLE_DIR_CHAN_NUM     8

#define TIMEOUT                 500 /* 50 millisecond */ 
#define MBOX_NAME               "mailbox-client"  
#define MBOX_CNT                1 

char *channel_name[] = {
    "tx_chan_0",
    "tx_chan_1",
    "tx_chan_2",
    "tx_chan_3",
    "tx_chan_4",
    "tx_chan_5",
    "tx_chan_6",
    "tx_chan_7",
    "rx_chan_0",
    "rx_chan_1",
    "rx_chan_2",
    "rx_chan_3",
    "rx_chan_4",
    "rx_chan_5",
    "rx_chan_6",
    "rx_chan_7",
};

static bool mbox_data_ready;

struct mbox_canaan_chan {
    struct mbox_client  client;
    void __iomem        *mmio;
    struct mbox_chan    *channel;
};

struct mbox_canaan_client_device {
    struct device               *dev;
    struct mbox_canaan_chan     tx_channel[SINGLE_DIR_CHAN_NUM];
    struct mbox_canaan_chan     rx_channel[SINGLE_DIR_CHAN_NUM];
    char                        *rx_buffer[SINGLE_DIR_CHAN_NUM];
    char                        *message;
    spinlock_t                  lock;
    wait_queue_head_t           waitq;
    struct fasync_struct        *async_queue;
    dev_t                       devid;
    struct cdev                 cdev;
    struct class                *class;
};


static struct mbox_canaan_chan *to_canaan_chan(struct mbox_client *client)
{
    return container_of(client, struct mbox_canaan_chan, client);
}

static int mbox_canaan_message_fasync(int fd, struct file *filp, int on)
{
    struct mbox_canaan_client_device *client_dev = filp->private_data;

    return fasync_helper(fd, filp, on, &client_dev->async_queue);
}

/* ioctl function */
static int mbox_canaan_message_copy_send(struct file *filp, int chan_index, unsigned long arg)
{
    struct mbox_canaan_client_device *client_dev = filp->private_data;
    void *data;
    int ret;

    // printk("[%s,%d]", __func__, __LINE__);
    if(!client_dev->tx_channel[chan_index].channel)
    {
        dev_err(client_dev->dev, "Channel cannot do Tx\n");
        return -EINVAL;
    }

    client_dev->message = kzalloc(MBOX_MAX_MSG_LEN, GFP_KERNEL);
    if (!client_dev->message)
        return -ENOMEM;

    ret = copy_from_user(client_dev->message, (char *)arg, MBOX_MAX_MSG_LEN);
    if (ret) 
    {
        ret = -EFAULT;
        goto out;
    }

    data = client_dev->message;
    // print_hex_dump(KERN_INFO, "Client: send [MMIO]: ", DUMP_PREFIX_ADDRESS, 16, 1,
	// 				data, MBOX_MAX_MSG_LEN, true);

    ret = mbox_send_message(client_dev->tx_channel[chan_index].channel, data);
    if (ret < 0)
        dev_err(client_dev->dev, "Failed to send message via mailbox\n");

out:
    kfree(client_dev->message);

    return ret < 0 ? ret : 0;
}

static int mbox_canaan_message_copy_received(struct file *filp, int chan_index, unsigned long arg)
{
    struct mbox_canaan_client_device *client_dev = filp->private_data;
    int ret;

    // printk("[%s,%d]", __func__, __LINE__);
    if(!client_dev->rx_channel[chan_index].channel)
    {
        dev_err(client_dev->dev, "Channel cannot do Rx\n");
        return -EINVAL;
    }

    ret = copy_to_user((char *)arg, client_dev->rx_buffer[chan_index], MBOX_MAX_MSG_LEN);
    mbox_data_ready = false;
    if (ret) 
        return -EFAULT;
    
    return 0;
}

/* client callback */

static void mbox_canaan_receive_message(struct mbox_client *client, void *message)
{
    struct mbox_canaan_client_device *client_dev = dev_get_drvdata(client->dev);
    struct mbox_canaan_chan *chan = to_canaan_chan(client);
    unsigned long flags;
    int chan_index = (int)chan->channel->con_priv;
    chan_index -= SINGLE_DIR_CHAN_NUM;

    // printk("[%s,%d], chan_index:%d", __func__, __LINE__, chan_index);

    spin_lock_irqsave(&client_dev->lock, flags);
    memcpy_fromio(client_dev->rx_buffer[chan_index], client_dev->rx_channel[chan_index].mmio, MBOX_MAX_MSG_LEN);
    // print_hex_dump(KERN_INFO, "Client: Received [MMIO]: ", DUMP_PREFIX_ADDRESS, 16, 1,
	// 				client_dev->rx_buffer[chan_index], MBOX_MAX_MSG_LEN, true);
    // print_hex_dump(KERN_INFO, "Client: Received [MMIO]: ", DUMP_PREFIX_ADDRESS, 16, 1,
	// 				client_dev->rx_channel[chan_index].mmio, MBOX_MAX_MSG_LEN, true);
    mbox_data_ready = true;
    spin_unlock_irqrestore(&client_dev->lock, flags);

    // wake_up_interruptible
    kill_fasync(&client_dev->async_queue, SIGIO, POLL_IN);
}

static void mbox_canaan_prepare_message(struct mbox_client *client, void *message)
{
    struct mbox_canaan_client_device *client_dev = dev_get_drvdata(client->dev);
    struct mbox_canaan_chan *chan = to_canaan_chan(client);
    int chan_index = (int)chan->channel->con_priv;

    // printk("[%s,%d], chan_index:%d", __func__, __LINE__, chan_index);

    memcpy_toio(client_dev->tx_channel[chan_index].mmio, client_dev->message, MBOX_MAX_MSG_LEN);

    // print_hex_dump(KERN_INFO, "Client: Send [MMIO]: ", DUMP_PREFIX_ADDRESS, 16, 1,
	// 				client_dev->tx_channel[chan_index].mmio, MBOX_MAX_MSG_LEN, true);
}

static void mbox_canaan_message_sent(struct mbox_client *client,
                    void *message, int r)
{
    if (r)
        dev_warn(client->dev, 
            "Client: Message could not be sent: %d\n", r);
    else
        dev_dbg(client->dev, 
            "Client: Message sent\n");

}

static void mbox_canaan_request_channel(struct platform_device *pdev, 
                                    struct mbox_canaan_chan *canaan_chan, 
                                    int chan_index)
{
    canaan_chan->client.dev             = &pdev->dev;
    canaan_chan->client.rx_callback     = mbox_canaan_receive_message;
    canaan_chan->client.tx_prepare      = mbox_canaan_prepare_message;
    canaan_chan->client.tx_done         = mbox_canaan_message_sent;
    canaan_chan->client.tx_block        = true;
    canaan_chan->client.knows_txdone    = false;
    canaan_chan->client.tx_tout         = TIMEOUT;

    canaan_chan->channel = mbox_request_channel_byname(&canaan_chan->client, channel_name[chan_index]);
    if (IS_ERR(canaan_chan->channel))
        dev_warn(&pdev->dev, "Failed to request %s channel\n", channel_name[chan_index]);
}

/* file_operations */

static int mbox_canaan_client_open(struct inode *inode, struct file *filp)
{
    struct mbox_canaan_client_device *client_dev;
    // printk("[%s,%d]", __func__, __LINE__);

    client_dev = container_of(inode->i_cdev, struct mbox_canaan_client_device, cdev);
    filp->private_data = client_dev;

    return 0;
}

static int mbox_canaan_client_release(struct inode *inode, struct file *filp)
{
    mbox_canaan_message_fasync(-1, filp, 0);
    return 0;
}

static bool mbox_canaan_data_ready(struct mbox_canaan_client_device *client_dev)
{
    bool data_ready;
    unsigned long flags;

	spin_lock_irqsave(&client_dev->lock, flags);
	data_ready = mbox_data_ready;
	spin_unlock_irqrestore(&client_dev->lock, flags);

	return data_ready;
}

static long mbox_canaan_client_ioctl(struct file *filp, unsigned int cmd, 
                                    unsigned long arg)
{
    switch (cmd)
    {
        case MBOX_CHAN_0_TX :
            mbox_canaan_message_copy_send(filp, 0, arg);
            break;
        case MBOX_CHAN_1_TX :
            mbox_canaan_message_copy_send(filp, 1, arg);
            break;
        case MBOX_CHAN_2_TX :
            mbox_canaan_message_copy_send(filp, 2, arg);
            break;
        case MBOX_CHAN_3_TX :
            mbox_canaan_message_copy_send(filp, 3, arg);
            break;
        case MBOX_CHAN_4_TX :
            mbox_canaan_message_copy_send(filp, 4, arg);
            break;
        case MBOX_CHAN_5_TX :
            mbox_canaan_message_copy_send(filp, 5, arg);
            break;
        case MBOX_CHAN_6_TX :
            mbox_canaan_message_copy_send(filp, 6, arg);
            break;
        case MBOX_CHAN_7_TX :
            mbox_canaan_message_copy_send(filp, 7, arg);
            break;
        case MBOX_CHAN_0_RX :
            if (mbox_canaan_message_copy_received(filp, 0, arg) < 0)
                return -EFAULT;
            break;
        case MBOX_CHAN_1_RX :
            mbox_canaan_message_copy_received(filp, 1, arg);
            break;
        case MBOX_CHAN_2_RX :
            mbox_canaan_message_copy_received(filp, 2, arg);
            break;
        case MBOX_CHAN_3_RX :
            mbox_canaan_message_copy_received(filp, 3, arg);
            break;
        case MBOX_CHAN_4_RX :
            mbox_canaan_message_copy_received(filp, 4, arg);
            break;
        case MBOX_CHAN_5_RX :
            mbox_canaan_message_copy_received(filp, 5, arg);
            break;
        case MBOX_CHAN_6_RX :
            mbox_canaan_message_copy_received(filp, 6, arg);
            break;
        case MBOX_CHAN_7_RX :
            mbox_canaan_message_copy_received(filp, 7, arg);
            break;
        default :
            return -EINVAL;        
    }

    return 0;
}

static __poll_t
mbox_canaan_client_poll(struct file *filp, struct poll_table_struct *wait)
{
    struct mbox_canaan_client_device *client_dev = filp->private_data;

    poll_wait(filp, &client_dev->waitq, wait);

    if (mbox_canaan_data_ready(client_dev))
        return EPOLLIN | EPOLLRDNORM;

    return 0;
}

static struct file_operations canaan_client_fops = {
    .owner          = THIS_MODULE,
    .open           = mbox_canaan_client_open,
    .release        = mbox_canaan_client_release,
    .unlocked_ioctl = mbox_canaan_client_ioctl,
    .fasync         = mbox_canaan_message_fasync,
    .poll           = mbox_canaan_client_poll,
};

static int create_module_class(struct mbox_canaan_client_device *client_dev)
{
    alloc_chrdev_region(&client_dev->devid, 0, MBOX_CNT, MBOX_NAME);

    client_dev->cdev.owner = THIS_MODULE;
    cdev_init(&client_dev->cdev, &canaan_client_fops);

    cdev_add(&client_dev->cdev, client_dev->devid, MBOX_CNT);

    client_dev->class = class_create(THIS_MODULE, MBOX_NAME);
    client_dev->dev = device_create(client_dev->class, NULL, 
        client_dev->devid, NULL, MBOX_NAME);

    return 0;
}

static void destroy_module_class(struct mbox_canaan_client_device *client_dev)
{
    cdev_del(&client_dev->cdev);
    unregister_chrdev_region(client_dev->devid, MBOX_CNT);

    device_destroy(client_dev->class, client_dev->devid);
    class_destroy(client_dev->class);
}



static int mbox_canaan_client_probe(struct platform_device *pdev)
{
    struct mbox_canaan_client_device *client_dev;
    struct resource *res;
    resource_size_t size;
    int i;

    // printk("[%s,%d]", __func__, __LINE__);

    client_dev = devm_kzalloc(&pdev->dev, sizeof(*client_dev), GFP_KERNEL);
    if (!client_dev)
        return -ENOMEM;

    for (i = 0; i < SINGLE_DIR_CHAN_NUM; i++)
    {
        res = platform_get_resource(pdev, IORESOURCE_MEM, i);
        size = resource_size(res);
        client_dev->tx_channel[i].mmio = devm_ioremap_resource(&pdev->dev, res);
        if (PTR_ERR(client_dev->tx_channel[i].mmio) == -EBUSY)
            client_dev->tx_channel[i].mmio = devm_ioremap(&pdev->dev, res->start, size);
        else if (IS_ERR(client_dev->tx_channel[i].mmio))
            client_dev->tx_channel[i].mmio = NULL;

        res = platform_get_resource(pdev, IORESOURCE_MEM, i + SINGLE_DIR_CHAN_NUM);
        size = resource_size(res);
        client_dev->rx_channel[i].mmio = devm_ioremap_resource(&pdev->dev, res);
        if (PTR_ERR(client_dev->rx_channel[i].mmio) == -EBUSY)
            client_dev->rx_channel[i].mmio = devm_ioremap(&pdev->dev, res->start, size);
        else if (IS_ERR(client_dev->rx_channel[i].mmio))
            client_dev->rx_channel[i].mmio = NULL;
    }

    for (i = 0; i < SINGLE_DIR_CHAN_NUM; i++)
    {
        mbox_canaan_request_channel(pdev, &client_dev->tx_channel[i], i);
        mbox_canaan_request_channel(pdev, &client_dev->rx_channel[i], i + SINGLE_DIR_CHAN_NUM);
    
        if(!client_dev->tx_channel[i].channel && !client_dev->rx_channel[i].channel)
            return -EPROBE_DEFER;
    }

    client_dev->dev = &pdev->dev;
    platform_set_drvdata(pdev, client_dev);

    spin_lock_init(&client_dev->lock);

    for (i = 0; i < SINGLE_DIR_CHAN_NUM; i++)
    {
        if (client_dev->rx_channel)
        {
            client_dev->rx_buffer[i] = devm_kzalloc(&pdev->dev,
                                        MBOX_MAX_MSG_LEN, GFP_KERNEL);
            if (!client_dev->rx_buffer)
                return -ENOMEM;
        }
    }

    init_waitqueue_head(&client_dev->waitq);

    create_module_class(client_dev);

    dev_info(&pdev->dev, "Successfully registered\n");

    return 0;
}

static int mbox_canaan_client_remove(struct platform_device *pdev)
{
    int i;
    struct mbox_canaan_client_device *client_dev = platform_get_drvdata(pdev);

    for (i = 0; i < SINGLE_DIR_CHAN_NUM; i++)
    {
        if (client_dev->tx_channel[i].channel)
            mbox_free_channel(client_dev->tx_channel[i].channel);
        if (client_dev->rx_channel[i].channel)
            mbox_free_channel(client_dev->rx_channel[i].channel);
    }

    destroy_module_class(client_dev);

    // printk("[%s,%d]", __func__, __LINE__);

    return 0;
}

static const struct of_device_id mbox_canaan_client_match[] = {
    { .compatible = "mailbox-client" },
    {},
};
MODULE_DEVICE_TABLE(of, mbox_canaan_client_match);

static struct platform_driver mbox_canaan_client_driver = {
    .driver = {
        .name = "mailbox_client",
        .of_match_table = mbox_canaan_client_match,
    },
    .probe = mbox_canaan_client_probe,
    .remove = mbox_canaan_client_remove,
};
module_platform_driver(mbox_canaan_client_driver);

// static int __init mbox_canaan_client_init(void)
// {
//     int ret;
//     ret = platform_driver_register(&mbox_canaan_client_driver);
//     return 0;
// }

// static void __exit mbox_canaan_client_exit(void)
// {
//     platform_driver_unregister(&mbox_canaan_client_driver);
// }

// module_init(mbox_canaan_client_init);
// module_exit(mbox_canaan_client_exit);

MODULE_DESCRIPTION("Canaan mailbox client driver");
MODULE_AUTHOR("lst");
MODULE_LICENSE("GPL v2");

