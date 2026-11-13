/* lwipopts.h - lwIP configuration for the Raspberry Pi Zero 2 W Wi-Fi tests.
 *
 * No OS layer: the raw (callback) API only, driven by the one thread that
 * owns the radio, through net::poll(). IPv4, ARP, DHCP client, DNS
 * client, ICMP, UDP, TCP. The same choices as the Mango Pi Wi-Fi tests.
 */
#ifndef RPIZ2W_LWIPOPTS_H
#define RPIZ2W_LWIPOPTS_H

#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_TIMERS                 1

#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define ETHARP_SUPPORT_STATIC_ENTRIES 1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1      /* ping */
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   1
#define LWIP_DNS                    1
#define LWIP_AUTOIP                 0
#define LWIP_ACD                    0
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_IGMP                   0

#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_SINGLE_NETIF           1

/* Memory: the board has gigabytes, so be generous. */
#define MEM_ALIGNMENT               8
#define MEM_SIZE                    (512 * 1024)
#define MEMP_NUM_PBUF               64
#define MEMP_NUM_RAW_PCB            4
#define MEMP_NUM_UDP_PCB            8
#define MEMP_NUM_TCP_PCB            8
#define MEMP_NUM_TCP_PCB_LISTEN     4
#define MEMP_NUM_TCP_SEG            256
#define MEMP_NUM_SYS_TIMEOUT        16
#define PBUF_POOL_SIZE              256
#define PBUF_POOL_BUFSIZE           1600

#define TCP_MSS                     1460
#define TCP_WND                     (16 * TCP_MSS)
#define TCP_SND_BUF                 (16 * TCP_MSS)
#define TCP_SND_QUEUELEN            64
#define TCP_QUEUE_OOSEQ             1
#define LWIP_TCP_KEEPALIVE          1

#define ETH_PAD_SIZE                0
#define LWIP_STATS                  0
#define LWIP_PROVIDE_ERRNO          1
#define LWIP_NO_CTYPE_H             1

/* The radio has no checksum offload. */
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_GEN_ICMP           1
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_UDP          1
#define CHECKSUM_CHECK_TCP          1
#define CHECKSUM_CHECK_ICMP         1

#endif
