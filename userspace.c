#include <stdio.h>
#include <stdlib.h>
#include <linux/rtc.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

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

#define MBOX_DEV                "/dev/mailbox-client"
int fd;

typedef enum __bool { false = 0, true = 1, } bool;

bool data_send;

unsigned char mem_set[32];
unsigned char mem_rec[32];

static void sigio_signal_func(int signum)
{
    int ret = 0;
    int i = 0;
    
    printf("\r\nsigio_signal_func\r\n");

    if (ioctl(fd, MBOX_CHAN_5_RX, mem_rec) < 0)
        printf("rx 0 error\r\n");
    else
        printf("rx 0 success\r\n");

    for (i = 0; i < 32; i++)
    {
        if (i % 8 == 0)
        {
            printf("\r\n");
            printf("received mem_rec[%2d]: ", i);
        }
        printf("%3x", mem_rec[i]);
    }

    // sleep(1);
    if (mem_rec[0] <= 0xfb && mem_rec[0] >= 4)
    {
        if (mem_rec[0] != (mem_set[0] + 4))
        {   
            printf("===================================error===================================");
            exit(0);
        }
    }
    data_send = true;
}

int main(void)
{
    int ret;

    int i;
    int m = 0;
    int flags = 0;

    data_send = true;

    fd = open(MBOX_DEV, O_RDWR);
    if (fd == -1)
    {
        printf("open failed %s\r\n", MBOX_DEV);
        return -1;
    }
    
    signal(SIGIO, sigio_signal_func);

    fcntl(fd, F_SETOWN, getpid());
    flags = fcntl(fd, F_GETFD);
    fcntl(fd, F_SETFL, flags | FASYNC);

    while(1)
    {
        while (!data_send);
            data_send = false;
        printf("\r\n****************begin*****************\r\n");
        m++;
        for (i = 0; i < 32; i++)
            mem_set[i] = i + m;
            
        for (i = 0; i < 32; i++)
        {
            if (i % 8 == 0)
            {
                printf("\r\n");
                printf("send mem_set[%2d]: ", i);
            }
            printf("%3x", mem_set[i]);
        }

        if (ioctl(fd, MBOX_CHAN_5_TX, mem_set) < 0)
        {
            printf("\r\ntx 0 error\r\n");
            ret = -1;
        }
        else
            printf("\r\ntx 0 success\r\n");
        printf("\r\n*****************end******************\r\n");
    }

    close(fd);

    return 0;
}