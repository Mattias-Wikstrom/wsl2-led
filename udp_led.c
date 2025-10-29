// Notes:
//   1. Install by specifying the IP and the UDP port:
//        sudo insmod udp_led.ko udp_ip="192.168.1.50" udp_port=9000
//   2. Run 'dmesg | grep "Sending UDP messages to"' to verify that the IP and PORT are as expected.
//   3. Run 'ip route | grep default' to see what IP to use.
//   4. You will likely need to use a custom WSL2 Linux kernel. WSL2 does not support LED devices by default. 
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/socket.h>
#include <linux/slab.h>
#include <net/sock.h>
#include <linux/leds.h>
#include <linux/moduleparam.h>

static int udp_port = 8888;                 // default port
static char *udp_ip = "172.18.208.1";      // default IP

module_param(udp_port, int, 0444);
MODULE_PARM_DESC(udp_port, "UDP port to send LED messages to");

module_param(udp_ip, charp, 0444);
MODULE_PARM_DESC(udp_ip, "UDP IP address to send LED messages to");

static struct socket *udp_sock = NULL;

static int send_udp_message(const char *msg)
{
    struct sockaddr_in addr;
    struct msghdr msg_hdr;
    struct kvec iov;
    int ret;
    struct in_addr ip_addr;

    if (!udp_sock) {
        pr_err("UDP socket not initialized\n");
        return -ENOTCONN;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(udp_port);

    if (!in4_pton(udp_ip, -1, (u8 *)&ip_addr, -1, NULL)) {
        pr_err("Invalid IP address: %s\n", udp_ip);
        return -EINVAL;
    }
    addr.sin_addr = ip_addr;

    memset(&msg_hdr, 0, sizeof(msg_hdr));
    msg_hdr.msg_name = &addr;
    msg_hdr.msg_namelen = sizeof(addr);

    iov.iov_base = (char *)msg;
    iov.iov_len = strlen(msg);

    ret = kernel_sendmsg(udp_sock, &msg_hdr, &iov, 1, iov.iov_len);
    if (ret < 0)
        pr_err("Failed to send UDP message: %d\n", ret);
    else
        pr_info("Sent UDP message: %s\n", msg);

    return ret;
}

/* LED brightness callback */
static void udp_led_set_brightness(struct led_classdev *led_cdev,
                                   enum led_brightness brightness)
{
    char msg[16];
    const char *led_name = led_cdev->name;

    snprintf(msg, sizeof(msg), "%s:%s", led_name,
             brightness ? "ON" : "OFF");

    send_udp_message(msg);
}

/* Three LED devices */
static struct led_classdev led_r = {
    .name = "R",
    .max_brightness = 1,
    .brightness_set = udp_led_set_brightness,
};

static struct led_classdev led_g = {
    .name = "G",
    .max_brightness = 1,
    .brightness_set = udp_led_set_brightness,
};

static struct led_classdev led_b = {
    .name = "B",
    .max_brightness = 1,
    .brightness_set = udp_led_set_brightness,
};

static int __init udp_led_init(void)
{
    int ret;

    pr_info("Initializing UDP LED module\n");

    /* Create UDP socket */
    ret = sock_create_kern(&init_net, AF_INET, SOCK_DGRAM, IPPROTO_UDP, &udp_sock);
    if (ret < 0) {
        pr_err("Failed to create UDP socket: %d\n", ret);
        return ret;
    }

    /* Register LED devices */
    ret = led_classdev_register(NULL, &led_r);
    if (ret) goto err_r;
    ret = led_classdev_register(NULL, &led_g);
    if (ret) goto err_g;
    ret = led_classdev_register(NULL, &led_b);
    if (ret) goto err_b;

    pr_info("UDP LED module loaded successfully\n");
    pr_info("Sending UDP messages to IP: %s, Port: %d\n", udp_ip, udp_port);
    return 0;

err_b:
    led_classdev_unregister(&led_g);
err_g:
    led_classdev_unregister(&led_r);
err_r:
    if (udp_sock) {
        sock_release(udp_sock);
        udp_sock = NULL;
    }
    return ret;
}

static void __exit udp_led_exit(void)
{
    pr_info("Unloading UDP LED module\n");

    led_classdev_unregister(&led_r);
    led_classdev_unregister(&led_g);
    led_classdev_unregister(&led_b);

    if (udp_sock) {
        sock_release(udp_sock);
        udp_sock = NULL;
    }

    pr_info("UDP LED module unloaded\n");
}

module_init(udp_led_init);
module_exit(udp_led_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mattias");
MODULE_DESCRIPTION("Kernel module with LED devices that send UDP messages");
