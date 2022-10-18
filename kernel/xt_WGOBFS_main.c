/*
 * xt_WGOBFS kernel module
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/netfilter_ipv4/ip_tables.h>
#include <net/ip.h>
#include "xt_WGOBFS.h"
#include "wg.h"
#include "chacha8.h"

#define WG_HANDSHAKE_INIT       0x01
#define WG_HANDSHAKE_RESP       0x02
#define WG_COOKIE               0x03
#define WG_DATA                 0x04
#define OBFS_WG_HANDSHAKE_INIT  0x11
#define OBFS_WG_HANDSHAKE_RESP  0x12
#define WG_MIN_LEN           32
#define MAX_RND_LEN          32
#define MIN_RND_LEN           4

struct obfs_buf {
        u8 rnd[MAX_RND_LEN];
        u8 chacha_out[CHACHA8_OUTPUT_SIZE];
        u8 rnd_len;
};

/* 1) Fill the allocated @buf with random bytes.
 * 2) Return a number in [min_len, max_len] at random.
 *
 * get random from kernel is slightly faster than run chacha8 on seeds such as
 * timestamp or WG counter
 */
static u8 get_random_insert(u8 *buf, u8 min_len, u8 max_len)
{
        u8 r, i;

        r = 0;
        while (1) {
                get_random_bytes_wait(buf, MAX_RND_LEN);
                for (i = 0; i < MAX_RND_LEN; i++) {
                        if (buf[i] >= min_len && buf[i] <= max_len) {
                                r = buf[i];
                                break;
                        }
                }

                if (r > 0)
                        break;
        }

        return r;
}

/* Replace the all zeros mac2 with random bytes, then change the type field to
 * 0x11 or 0x12
 */
static void obfs_mac2(u8 *buf, const int data_len, struct obfs_buf *ob,
                      const u8 *k)
{
        u8 type;
        struct wg_message_handshake_initiation *hsi;
        struct wg_message_handshake_response *hsr;
        u32 *np;

        type = buf[0];
        if (type == WG_HANDSHAKE_INIT && data_len == 148) {
                hsi = (struct wg_message_handshake_initiation *) buf;
                /* highly unlikely the first 4 bytes of cookie are all zeros */
                np = (u32 *) hsi->macs.mac2;
                if (*np)
                        return;

                /* Generate pseudo-random bytes as mac2.
                 * - Use 8th - 16th byte of WG packet as input of chacha8
                 * - Write 128bits output to mac2
                 */
                chacha8_hash((const u64)(buf + 8), k, ob->chacha_out);
                memcpy(hsi->macs.mac2, ob->chacha_out, WG_COOKIE_LEN);

                /* mark the packet as need restore mac2 upon receiving */
                buf[0] |= 0x10;

        } else if (type == WG_HANDSHAKE_RESP && data_len == 92) {
                hsr = (struct wg_message_handshake_response *) buf;
                np = (u32 *) hsr->macs.mac2;
                if (*np)
                        return;

                chacha8_hash((const u64)(buf + 8), k, ob->chacha_out);
                memcpy(hsr->macs.mac2, ob->chacha_out, WG_COOKIE_LEN);
                buf[0] |= 0x10;
        }
}

static int random_drop_wg_keepalive(u8 *buf, const int len, const u8 *key)
{
        u8 type;
        u8 buf_prn[CHACHA8_OUTPUT_SIZE];

        type = *buf;
        if (type != WG_DATA || len != 32)
                return 0;

        /* generate a pseudo-random string by hashing last 8 bytes of keepalive
         * message. We can assume the probability of s[0] > 50 is 0.8 */
        chacha8_hash((const u64)(buf + len - CHACHA8_INPUT_SIZE), key,
                     buf_prn);
        if (buf_prn[0] > 50)
                return 1;
        else
                return 0;
}

/* The WG packet is obfuscated by:
 *
 *   - Replace the all zeros mac2 field with pseudo-random bytes.
 *
 *   - Obfs the first 16 bytes of WG message.
 *
 *   - Change the length of WG message by padding a variable length random
 *     string at beginning, such that:
 *
 *     B1 B2 B3 B4 ... Orig_WG_message
 *     Byte 1 stores length of the insertion.
 *
 */
static void obfs_wg(u8 *buf, const int len, struct obfs_buf *ob, const u8 *key)
{
        u8 *b, *tail;
        u8 rnd_len;
        int i;

        obfs_mac2(buf, len, ob, key);
        /* Generate pseudo-random string from last 8 bytes of WG packet. Use it
         * to XOR with the first 16 bytes of WG message. It has message type,
         * reserved field and counter. They look distinct.
         */
        chacha8_hash((const u64)(buf + len - CHACHA8_INPUT_SIZE), key,
                     ob->chacha_out);
        rnd_len = ob->rnd_len;
        /* set the first byte of random as its length */
        ob->rnd[0] = rnd_len ^ ob->chacha_out[16];
        b = buf;
        for (i = 0; i < 16; i++, b++)
                *b ^= ob->chacha_out[i];

        /* shift WG packet towards end, make room for padding */
        tail = buf + len - 1;
        b = tail + rnd_len;
        for (i = 0; i < len; i++, tail--, b--)
                *b = *tail;

        memcpy(buf, ob->rnd, rnd_len);
}

/* make a skb writable, and if necessary, expand it */
static int prepare_skb_for_insert(struct sk_buff *skb, int ntail)
{
        int extra_len;

        extra_len = ntail - skb_tailroom(skb);
        if (extra_len > 0) {
                if (pskb_expand_head(skb, 0, extra_len, GFP_ATOMIC))
                        return -1;
        } else {

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,3,0)
                if (unlikely(skb_ensure_writable(skb, skb->len)))
#else
                if (unlikely(!skb_make_writable(skb, skb->len)))
#endif
                        return -1;
        }

        skb_put(skb, ntail);
        return 0;
}

static unsigned int xt_obfs(struct sk_buff *skb,
                            const struct xt_wg_obfs_info *info)
{
        struct obfs_buf ob;
        struct iphdr *iph;
        struct udphdr *udph;
        int wg_data_len, max_rnd_len;
        u8 rnd_len;
        u8 *buf_udp;

        udph = udp_hdr(skb);
        buf_udp = (u8 *) udph + sizeof(struct udphdr);
        wg_data_len = ntohs(udph->len) - sizeof(struct udphdr);

        if (random_drop_wg_keepalive(buf_udp, wg_data_len, info->chacha_key))
                return NF_DROP;

        /* Insert a long pseudo-random string if the WG packet is small, or a
         * short string if WG packet is big.
         */
        max_rnd_len = (wg_data_len > 200) ? 8 : MAX_RND_LEN;
        rnd_len = get_random_insert(&(ob.rnd[0]), MIN_RND_LEN, max_rnd_len);
        if (prepare_skb_for_insert(skb, rnd_len))
                return NF_DROP;

        udph = udp_hdr(skb);
        buf_udp = (u8 *) udph + sizeof(struct udphdr);
        obfs_wg(buf_udp, wg_data_len, &ob, info->chacha_key);

        /* packet with DiffServ 0x88 looks distinct? */
        iph = ip_hdr(skb);
        iph->tos = 0;

        /* recalculate ip header checksum */
        iph->tot_len = htons(ntohs(iph->tot_len) + rnd_len);
        iph->check = 0;
        ip_send_check(iph);

        /* CHECKSUM_PARTIAL: The driver is required to checksum the packet.
         * With CHECKSUM_PARTIAL, the udp packet has good checksum in VM, bad
         * checksum after leave VM. Set to CHECKSUM_NONE fixes the problem.
         */
        if (skb->ip_summed == CHECKSUM_PARTIAL)
                skb->ip_summed = CHECKSUM_NONE;

        /* recalculate udp header checksum */
        udph->len = htons(ntohs(udph->len) + rnd_len);
        udph->check = 0;
        udph->check = csum_tcpudp_magic(iph->saddr, iph->daddr,
                                        ntohs(udph->len), IPPROTO_UDP,
                                        csum_partial((char *) udph,
                                                     ntohs(udph->len), 0));
        return XT_CONTINUE;
}

static void restore_mac2(u8 *buf)
{
        struct wg_message_handshake_initiation *hsi;
        struct wg_message_handshake_response *hsr;
        static u8 zero_mac2[WG_COOKIE_LEN];

        /* mac2 was all zeros before obfscation, reset it back to zeros */
        switch (buf[0]) {
        case OBFS_WG_HANDSHAKE_INIT:
                hsi = (struct wg_message_handshake_initiation *) buf;
                /* memcpy is faster than memset, 860 vs 847 Mbits/s */
                memcpy(hsi->macs.mac2, zero_mac2, WG_COOKIE_LEN);
                break;
        case OBFS_WG_HANDSHAKE_RESP:
                hsr = (struct wg_message_handshake_response *) buf;
                memcpy(hsr->macs.mac2, zero_mac2, WG_COOKIE_LEN);
                break;
        }

        buf[0] &= 0x0F;
}

static int restore_wg(u8 *buf, int len, const u8 *key)
{
        u8 buf_prn[CHACHA8_OUTPUT_SIZE];
        u8 *b, *head;
        int i, wg_data_len, rnd_len;

        /* Same as obfuscate, generate the same pseudo-random string from last
         * 8 bytes of UDP packet. Need it for restoring the first 16 bytes of
         * WG packet.
         */
        chacha8_hash((const u64)(buf + len - CHACHA8_INPUT_SIZE), key,
                     buf_prn);

        /* restore the length of random insertion */
        buf[0] ^= buf_prn[16];

        rnd_len = (int) buf[0];
        if (rnd_len + WG_MIN_LEN > len)
                return -1;

        /* shift real WG packet forward to remove random bytes insertion */
        head = buf;
        b = buf + rnd_len;
        wg_data_len = len - rnd_len;
        for (i = 0; i < wg_data_len; i++, head++, b++)
                *head = *b;

        /* restore the first 16 bytes of WG packet */
        head = buf;
        for (i = 0; i < 16; i++, head++)
                *head ^= buf_prn[i];

        restore_mac2(buf);
        return rnd_len;
}

static unsigned int xt_unobfs(struct sk_buff *skb,
                              const struct xt_wg_obfs_info *info)
{
        struct iphdr *iph;
        struct udphdr *udph;
        u8 *buf_udp;
        int data_len;
        int rnd_len;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,3,0)
        if (unlikely(skb_ensure_writable(skb, skb->len)))
#else
        if (unlikely(!skb_make_writable(skb, skb->len)))
#endif
                return NF_DROP;

        udph = udp_hdr(skb);
        buf_udp = (u8 *) udph + sizeof(struct udphdr);
        data_len = ntohs(udph->len) - sizeof(struct udphdr);
        /* random bytes insertion adds at least 4 bytes */
        if (data_len < MIN_RND_LEN)
                return NF_DROP;

        rnd_len = restore_wg(buf_udp, data_len, info->chacha_key);
        if (rnd_len < 0)
                return NF_DROP;

        skb->len -= rnd_len;
        skb->tail -= rnd_len;

        /* recalculate ip header checksum */
        iph = ip_hdr(skb);
        iph->tot_len = htons(ntohs(iph->tot_len) - rnd_len);
        iph->check = 0;
        ip_send_check(iph);

        /* recalculate udp header checksum */
        udph->len = htons(ntohs(udph->len) - rnd_len);
        udph->check = 0;
        udph->check = csum_tcpudp_magic(iph->saddr, iph->daddr,
                                        ntohs(udph->len), IPPROTO_UDP,
                                        csum_partial((char *) udph,
                                                     ntohs(udph->len), 0));
        return XT_CONTINUE;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,7,0)
static unsigned int
xt_wg_obfs_target(struct sk_buff *skb, const struct xt_action_param *par)
#else
static unsigned int
xt_wg_obfs_target(struct sk_buff *skb, const struct xt_target_param *par)
#endif
{
        const struct xt_wg_obfs_info *info = par->targinfo;
        struct iphdr *iph;

        iph = ip_hdr(skb);
        /* only work with UDP so far, may obfuscate UDP into TCP later */
        if (iph->protocol != IPPROTO_UDP)
                return XT_CONTINUE;

        if (info->mode == XT_MODE_OBFS)
                return xt_obfs(skb, info);
        else if (info->mode == XT_MODE_UNOBFS)
                return xt_unobfs(skb, info);

        return XT_CONTINUE;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,35)
static int xt_wg_obfs_checkentry(const struct xt_tgchk_param *par)
{
        if (strcmp(par->table, "mangle")) {
                printk(KERN_WARNING
                       "WGOBFS: can only be called from mangle table\n");
                return -EINVAL;
        }

        return 0;
}
#else
static bool xt_wg_obfs_checkentry(const struct xt_tgchk_param *par)
{
        if (strcmp(par->table, "mangle")) {
                printk(KERN_WARNING
                       "WGOBFS: can only be called from mangle table\n");
                return false;
        }

        return true;
}
#endif

static struct xt_target xt_wg_obfs = {
        .name = "WGOBFS",
        .revision = 0,
        .family = NFPROTO_IPV4,
        .table = "mangle",
        .target = xt_wg_obfs_target,
        .targetsize = XT_ALIGN(sizeof(struct xt_wg_obfs_info)),
        .checkentry = xt_wg_obfs_checkentry,
        .me = THIS_MODULE,
};

static int __init wg_obfs_target_init(void)
{
        return xt_register_target(&xt_wg_obfs);
}

static void __exit wg_obfs_target_exit(void)
{
        xt_unregister_target(&xt_wg_obfs);
}

module_init(wg_obfs_target_init);
module_exit(wg_obfs_target_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Iptables obfuscation module for WireGuard");
MODULE_AUTHOR("Wei Chen <weichen302@gmail.com>");
MODULE_VERSION("0.2");
MODULE_ALIAS("xt_WGOBFS");
