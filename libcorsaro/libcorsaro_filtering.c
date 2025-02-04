/*
 * corsaro
 *
 * Alistair King, CAIDA, UC San Diego
 * Shane Alcock, WAND, University of Waikato
 *
 * corsaro-info@caida.org
 *
 * Copyright (C) 2012-2019 The Regents of the University of California.
 * All Rights Reserved.
 *
 * This file is part of corsaro.
 *
 * Permission to copy, modify, and distribute this software and its
 * documentation for academic research and education purposes, without fee, and
 * without a written agreement is hereby granted, provided that
 * the above copyright notice, this paragraph and the following paragraphs
 * appear in all copies.
 *
 * Permission to make use of this software for other than academic research and
 * education purposes may be obtained by contacting:
 *
 * Office of Innovation and Commercialization
 * 9500 Gilman Drive, Mail Code 0910
 * University of California
 * La Jolla, CA 92093-0910
 * (858) 534-5815
 * invent@ucsd.edu
 *
 * This software program and documentation are copyrighted by The Regents of the
 * University of California. The software program and documentation are supplied
 * “as is”, without any accompanying services from The Regents. The Regents does
 * not warrant that the operation of the program will be uninterrupted or
 * error-free. The end-user understands that the program was developed for
 * research purposes and is advised not to rely exclusively on the program for
 * any reason.
 *
 * IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION,
 * EVEN IF THE UNIVERSITY OF CALIFORNIA HAS BEEN ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE. THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. THE SOFTWARE PROVIDED
 * HEREUNDER IS ON AN “AS IS” BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO
 * OBLIGATIONS TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
 * MODIFICATIONS.
 */

#include <stdlib.h>
#include <string.h>
#include <libtrace.h>

#include "libcorsaro_filtering.h"
#include "libcorsaro_log.h"

typedef struct filter_params {
    libtrace_ip_t *ip;
    libtrace_tcp_t *tcp;
    libtrace_udp_t *udp;
    libtrace_icmp_t *icmp;
    uint32_t translen;
    uint32_t payloadlen;
    uint16_t source_port;
    uint16_t dest_port;
    uint8_t *payload;
} filter_params_t;

static inline int _apply_ttl200_filter(corsaro_logger_t *logger,
        libtrace_ip_t *ip) {

    if (!ip) {
        return -1;
    }
    if (ip->ip_p == TRACE_IPPROTO_ICMP) {
        return 0;
    }
    if (ip->ip_ttl < 200) {
        return 0;
    }

    return 1;
}

static inline int _apply_notip_filter(corsaro_logger_t *logger,
        libtrace_ip_t *ip) {
    if (!ip) {
        return 1;
    }
    return 0;
}

static inline int _apply_tcpwin_1024_filter(corsaro_logger_t *logger,
        libtrace_tcp_t *tcp) {

    if (!tcp) {
        return -1;
    }

    if (tcp->syn && !tcp->ack && ntohs(tcp->window) == 1024) {
        return 1;
    }

    return 0;
}

static inline int _apply_no_tcp_options_filter(corsaro_logger_t *logger,
        libtrace_tcp_t *tcp) {

    if (!tcp) {
        return -1;
    }

    if (tcp->syn && !tcp->ack && tcp->doff == 5) {
        return 1;
    }
    return 0;
}

static inline int _apply_ttl200_nonspoofed_filter(corsaro_logger_t *logger,
        libtrace_ip_t *ip, libtrace_tcp_t *tcp) {

    int ret;

    /* Must be TTL >= 200 and NOT a masscan probe (i.e. can't have a
     * TCP window of 1024 and no TCP options)
     */
    if ((ret = _apply_ttl200_filter(logger, ip)) <= 0) {
        return ret;
    }

    if (_apply_tcpwin_1024_filter(logger, tcp) <= 0) {
        return 1;
    }

    if (_apply_no_tcp_options_filter(logger, tcp) <= 0) {
        return 1;
    }

    return 0;
}

static inline int _apply_fragment_filter(corsaro_logger_t *logger,
        libtrace_ip_t *ip) {

    if (!ip) {
        return -1;
    }

    if ((ntohs(ip->ip_off) & 0x9fff) == 0) {
        return 0;
    }
    /* Fragment offset is non-zero OR reserved flag is non-zero */
    return 1;
}

static inline int _apply_last_src_byte0_filter(corsaro_logger_t *logger,
        libtrace_ip_t *ip) {

    if (!ip) {
        return -1;
    }

    /* Do byte-swap just in case someone tries to run this on big-endian */
    if ((ntohl(ip->ip_src.s_addr) & 0xff) == 0) {
        return 1;
    }

    return 0;
}

static inline int _apply_last_src_byte255_filter(corsaro_logger_t *logger,
        libtrace_ip_t *ip) {

    if (!ip) {
        return -1;
    }

    /* Do byte-swap just in case someone tries to run this on big-endian */
    if ((ntohl(ip->ip_src.s_addr) & 0xff) == 0xff) {
        return 1;
    }

    return 0;
}

static inline int _apply_same_src_dest_filter(corsaro_logger_t *logger,
        libtrace_ip_t *ip) {

    if (!ip) {
        return -1;
    }

    if (ip->ip_src.s_addr == ip->ip_dst.s_addr) {
        return 1;
    }
    return 0;
}

static inline int _apply_udp_port_zero_filter(corsaro_logger_t *logger,
        filter_params_t *fparams) {

    if (!fparams->udp) {
        return 0;
    }

    if (fparams->translen < 4) {
        return -1;
    }

    if (fparams->source_port == 0 || fparams->dest_port == 0) {
        return 1;
    }

    return 0;
}

static inline int _apply_tcp_port_zero_filter(corsaro_logger_t *logger,
        filter_params_t *fparams) {

    if (!fparams->tcp) {
        return 0;
    }

    if (fparams->translen < 4) {
        return -1;
    }

    if (fparams->source_port == 0 || fparams->dest_port == 0) {
        return 1;
    }

    return 0;
}

static inline int _apply_udp_destport_eighty_filter(corsaro_logger_t *logger,
        filter_params_t *fparams) {

    if (!fparams->udp) {
        return 0;
    }

    if (fparams->translen < 4) {
        return -1;
    }

    if (fparams->dest_port == 80) {
        return 1;
    }

    return 0;
}

static inline int _apply_udp_0x31_filter(corsaro_logger_t *logger,
        filter_params_t *fparams) {

    uint8_t pattern[10] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x31, 0x00};

    if (fparams->payload == NULL || fparams->udp == NULL) {
        return 0;
    }

    if (fparams->payloadlen < 10) {
        return 0;
    }

    if (ntohs(fparams->ip->ip_len) != 58) {
        return 0;
    }

    if (memcmp(pattern, fparams->payload, 10) == 0) {
        return 1;
    }

    return 0;
}

static inline int _apply_sip_status_filter(corsaro_logger_t *logger,
        filter_params_t *fparams) {

  /* uint8_t pattern[9] = {0x53, 0x49, 0x50, 0x2f, 0x32, 0x2e, 0x30, 0x20, 0x34}; */
  uint8_t pattern[7] = {0x53, 0x49, 0x50, 0x2f, 0x32, 0x2e, 0x30};

    if (fparams->payload == NULL || fparams->udp == NULL) {
        return 0;
    }

    if (fparams->payloadlen < 7) {
        return 0;
    }

    if (fparams->source_port == 5060 && fparams->dest_port == 5060 && (memcmp(pattern, fparams->payload, 7) == 0) ) {
        return 1;
    }

    return 0;
}

static inline int _apply_udp_iplen_96_filter(corsaro_logger_t *logger,
        libtrace_ip_t *ip) {

    if (!ip) {
        return -1;
    }

    if (ip->ip_p == TRACE_IPPROTO_UDP && ntohs(ip->ip_len) == 96) {
        return 1;
    }

    return 0;
}

static inline int _apply_udp_iplen_1500_filter(corsaro_logger_t *logger,
        libtrace_ip_t *ip) {

    if (!ip) {
        return -1;
    }

    if (ip->ip_p == TRACE_IPPROTO_UDP && ntohs(ip->ip_len) == 1500) {
        return 1;
    }

    return 0;
}

static inline int _apply_port_53_filter(corsaro_logger_t *logger,
        uint16_t source_port, uint16_t dest_port) {

    if (source_port == 53 || dest_port == 53) {
        return 1;
    }

    return 0;
}

static inline int _apply_port_tcp23_filter(corsaro_logger_t *logger,
        filter_params_t *fparams) {

    if (fparams->tcp != NULL && (fparams->source_port == 23 ||
                fparams->dest_port == 23)) {
        return 1;
    }

    return 0;
}

static inline int _apply_port_tcp80_filter(corsaro_logger_t *logger,
        filter_params_t *fparams) {

    if (fparams->tcp != NULL && (fparams->source_port == 80 ||
                fparams->dest_port == 80)) {
        return 1;
    }

    return 0;
}

static inline int _apply_port_tcp5000_filter(corsaro_logger_t *logger,
        filter_params_t *fparams) {

    if (fparams->tcp != NULL && (fparams->source_port == 5000 ||
                fparams->dest_port == 5000)) {
        return 1;
    }

    return 0;
}

static inline int _apply_asn_208843_scan_filter(corsaro_logger_t *logger,
        filter_params_t *fparams) {

    int32_t srcip;

    if (!fparams->ip) {
        return 0;
    }
    
    srcip = ntohl(fparams->ip->ip_src.s_addr);

    if (fparams->tcp == NULL) {
        return 0;
    }

    if ((!fparams->tcp->syn) || fparams->tcp->ack) {
        return 0;
    }

    if (fparams->ip->ip_ttl >= 64) {
        return 0;
    }

    if ((srcip & 0xffffff00) != 0x2d534000) {
        return 0;
    }

    if (ntohs(fparams->tcp->window) != 8192) {
        return 0;
    }

    if (ntohs(fparams->ip->ip_len) != 44) {
        return 0;
    }

    return 1;
}

static inline int _apply_dns_resp_oddport_filter(corsaro_logger_t *logger,
        filter_params_t *fparams) {

    uint16_t *ptr16 = (uint16_t *)fparams->payload;

    if (fparams->payload == NULL || fparams->udp == NULL) {
        return 0;
    }

    if (ntohs(fparams->ip->ip_len) <= 42) {
        return 0;
    }

    if (fparams->payloadlen < 12) {
        return 0;
    }

    /* Check flags and codes */
    if ((ntohs(ptr16[1]) & 0xfff0) != 0x8180) {
        return 0;
    }

    /* Question count */
    if (ntohs(ptr16[2]) >= 10) {
        return 0;
    }

    /* Answer record count */
    if (ntohs(ptr16[3]) >= 10) {
        return 0;
    }

    /* NS (authority record) count */
    if (ntohs(ptr16[4]) >= 10) {
        return 0;
    }

    /* Additional record count */
    if (ntohs(ptr16[5]) >= 10) {
        return 0;
    }

    return 1;
}

static inline int _apply_netbios_name_filter(corsaro_logger_t *logger,
        filter_params_t *fparams) {

    uint16_t *ptr16 = (uint16_t *)fparams->payload;
    if (!fparams->payload || !fparams->udp) {
        return 0;
    }

    if (fparams->source_port != 137 || fparams->dest_port != 137) {
        return 0;
    }

    if (ntohs(fparams->ip->ip_len) <= 48) {
        return 0;
    }

    if (fparams->payloadlen < 20) {
        return 0;
    }

    if (ptr16[6] != htons(0x2043)) {
        return 0;
    }

    if (ptr16[7] != htons(0x4b41)) {
        return 0;
    }

    if (ptr16[8] != htons(0x4141)) {
        return 0;
    }

    if (ptr16[9] != htons(0x4141)) {
        return 0;
    }

    return 1;
}

static inline int _apply_backscatter_filter(corsaro_logger_t *logger,
        filter_params_t *fparams) {

    /* TODO return different positive values for filter quality analysis */

    /* UDP backscatter -- just DNS for now */
    if (fparams->udp) {
        if (fparams->source_port == 53) {
            return 1;
        }
    } else if (fparams->icmp) {
        switch(fparams->icmp->type) {       
            case 0:     // echo reply
            case 3:     // dest unreachable
            case 4:     // source quench
            case 5:     // redirect
            case 11:    // time exceeded
            case 12:    // parameter problem
            case 14:    // timestamp reply
            case 16:    // info reply
            case 18:    // address mask reply
                return 1;
        }
    } else if (fparams->tcp && fparams->payload) {
        /* No SYN-ACKs and no RSTs */
        if ((fparams->tcp->syn && fparams->tcp->ack) || fparams->tcp->rst) {
            return 1;
        }
    }
    return 0;
}

static inline int _apply_rfc5735_filter(corsaro_logger_t *logger,
        libtrace_ip_t *ip) {

    uint32_t srcip;

    if (!ip) {
        return -1;
    }

    srcip = ntohl(ip->ip_src.s_addr);

    /* TODO return different positive values for filter quality analysis */

    /* Optimization: if first bit of srcip is not set, then we only need
     * to check ranges where the first octet <= 127
     */
    if ((srcip & 0x80000000) == 0) {
        /* 0.0.0.0/8 */
        if ((srcip & 0xff000000) == 0x00000000) {
            return 1;
        }

        /* 10.0.0.0/8 */
        if ((srcip & 0xff000000) == 0x0a000000) {
            return 1;
        }

        /* 127.0.0.0/8 */
        if ((srcip & 0xff000000) == 0x7f000000) {
            return 1;
        }
        return 0;
    }

    /* 169.254.0.0/16 */
    if ((srcip & 0xffff0000) == 0xa9fe0000) {
        return 1;
    }

    /* 172.16.0.0/12 */
    if ((srcip & 0xfff00000) == 0xac100000) {
        return 1;
    }

    if ((srcip & 0xff000000) == 0xc0000000) {
        /* 192.0.0.0/24 */
        if ((srcip & 0x00ffff00) == 0x00000000) {
            return 1;
        }

        /* 192.0.2.0/24 */
        if ((srcip & 0x00ffff00) == 0x00000200) {
           return 1;
        }

        /* 192.88.99.0/24 */
        if ((srcip & 0x00ffff00) == 0x00586300) {
            return 1;
        }

        /* 192.168.0.0/16 */
        if ((srcip & 0x00ff0000) == 0x00a80000) {
            return 1;
        }
    }

    /* 198.18.0.0/15 */
    if ((srcip & 0xfffe0000) == 0xc6120000) {
        return 1;
    }

    /* 198.51.100.0/24 */
    if ((srcip & 0xffffff00) == 0xc6336400) {
        return 1;
    }

    /* 203.0.113.0/24 */
    if ((srcip & 0xffffff00) == 0xcb007100) {
        return 1;
    }

    /* 224.0.0.0/4 and 240.0.0.0/4 */
    if ((srcip & 0xf0000000) >= 0xe0000000) {
        return 1;
    }

    return 0;
}

static inline int _apply_abnormal_protocol_filter(corsaro_logger_t *logger,
        filter_params_t *fparams) {

    if (!fparams->ip) {
        return -1;
    }

    if (fparams->ip->ip_p == TRACE_IPPROTO_ICMP || fparams->ip->ip_p == TRACE_IPPROTO_UDP) {
        return 0;
    }

    if (fparams->ip->ip_p == TRACE_IPPROTO_IPV6) {
        return 0;
    }

    if (fparams->ip->ip_p != TRACE_IPPROTO_TCP) {
        return 1;
    }

    if (!fparams->payload || !fparams->tcp) {
        return -1;       // filter it?
    }

    /* Allow normal TCP flag combos */
    /* XXX this is a bit silly looking, can we optimise somehow? */

    /* TODO return different positive values for filter quality analysis */
    /* SYN */
    if (fparams->tcp->syn && !fparams->tcp->ack && !fparams->tcp->fin &&
            !fparams->tcp->psh && !fparams->tcp->rst && !fparams->tcp->urg) {
        return 0;
    }

    /* ACK */
    if (fparams->tcp->ack && !fparams->tcp->syn && !fparams->tcp->fin &&
            !fparams->tcp->psh && !fparams->tcp->rst && !fparams->tcp->urg) {
        return 0;
    }

    /* RST */
    if (fparams->tcp->rst && !fparams->tcp->syn && !fparams->tcp->ack &&
            !fparams->tcp->fin && !fparams->tcp->psh && !fparams->tcp->urg) {
        return 0;
    }

    /* FIN */
    if (fparams->tcp->fin && !fparams->tcp->syn && !fparams->tcp->ack &&
            !fparams->tcp->rst && !fparams->tcp->psh && !fparams->tcp->urg) {
        return 0;
    }

    /* SYN-FIN */
    if (fparams->tcp->fin && fparams->tcp->syn && !fparams->tcp->ack &&
            !fparams->tcp->rst && !fparams->tcp->psh && !fparams->tcp->urg) {
        return 0;
    }

    /* SYN-ACK */
    if (fparams->tcp->syn && fparams->tcp->ack && !fparams->tcp->fin &&
            !fparams->tcp->rst && !fparams->tcp->psh && !fparams->tcp->urg) {
        return 0;
    }

    /* FIN-ACK */
    if (fparams->tcp->fin && fparams->tcp->ack && !fparams->tcp->syn &&
            !fparams->tcp->rst && !fparams->tcp->psh && !fparams->tcp->urg) {
        return 0;
    }

    /* ACK-PSH */
    if (fparams->tcp->ack && fparams->tcp->psh && !fparams->tcp->syn &&
            !fparams->tcp->rst && !fparams->tcp->fin && !fparams->tcp->urg) {
        return 0;
    }

    /* FIN-ACK-PSH */
    if (fparams->tcp->fin && fparams->tcp->ack && fparams->tcp->psh &&
            !fparams->tcp->syn && !fparams->tcp->rst && !fparams->tcp->urg) {
        return 0;
    }

    /* Every other flag combo is "abnormal" */
    return 1;
}

static inline int _apply_bittorrent_filter(corsaro_logger_t *logger,
        filter_params_t *fparams) {

    uint32_t *ptr32;
    uint16_t udplen;
    uint16_t iplen;
    uint16_t *ptr16;
    uint8_t last10pat[10] = {0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00};

    if (!fparams->udp || !fparams->payload) {
        return 0;
    }
    ptr32 = (uint32_t *)(fparams->payload);
    ptr16 = (uint16_t *)(fparams->payload);
    udplen = ntohs(fparams->udp->len);
    iplen = ntohs(fparams->ip->ip_len);

    /* XXX This filter is frightening and should definitely be double
     * checked. */

    /* TODO return different positive values for filter quality analysis */
    if (udplen >= 20 && fparams->payloadlen >= 12) {
        if (ntohl(ptr32[0]) == 0x64313a61 || ntohl(ptr32[0]) == 0x64313a72) {
            if (ntohl(ptr32[1]) == 0x64323a69 && ntohl(ptr32[2]) == 0x6432303a)
            {
                return 1;
            }
        }
    }
    if (udplen >= 48 && fparams->payloadlen >= 40) {
        if (ntohl(ptr32[5]) == 0x13426974 && ntohl(ptr32[6]) == 0x546f7272 &&
                ntohl(ptr32[7]) == 0x656e7420 &&
                ntohl(ptr32[8]) == 0x70726f74 &&
                ntohl(ptr32[9]) == 0x6f636f6c) {
            return 1;
        }
    }
    if (iplen >= 0x3a) {
        if (ntohs(ptr16[0]) == 0x4102 || ntohs(ptr16[0]) == 0x2102
                || ntohs(ptr16[0]) == 0x3102
                || ntohs(ptr16[0]) == 0x1102) {

            if (fparams->payloadlen >= udplen - sizeof(libtrace_udp_t)) {
                uint8_t *ptr8 = (uint8_t *)fparams->payload;
                ptr8 += (fparams->payloadlen - 10);
                if (memcmp(ptr8, last10pat, 10) == 0) {
                    return 1;
                }
            }
        }
    }
    if (iplen == 0x30) {
        if (ntohs(ptr16[0]) == 0x4100 || ntohs(ptr16[0]) == 0x2100
                || ntohs(ptr16[0]) == 0x3102
                || ntohs(ptr16[0]) == 0x1100) {

            return 1;
        }
    }
    if (iplen == 61) {
        if (ntohl(ptr32[3]) == 0x7fffffff && ntohl(ptr32[4]) == 0xab020400 &&
                ntohl(ptr32[5]) == 0x01000000 &&
                ntohl(ptr32[6]) == 0x08000000 && ptr32[7] == 0) {
            return 1;
        }
    }
    return 0;
}

#define PREPROCESS_FROM_IP(ip, rem) \
    libtrace_tcp_t *tcp = NULL;     \
    libtrace_udp_t *udp = NULL;     \
    libtrace_icmp_t *icmp = NULL;     \
    uint8_t *udppayload = NULL;     \
    uint8_t *tcppayload = NULL;     \
    uint32_t payloadlen = 0;        \
    uint32_t translen = 0;          \
    uint16_t source_port = 0;       \
    uint16_t dest_port = 0;         \
    uint16_t ethertype = 0;         \
                                    \
    uint8_t proto = 0;          \
    void *transport = trace_get_payload_from_ip(ip, &proto, &rem);    \
    translen = rem;             \
                                \
    /* XXX what about IP in IP?  */             \
    if (proto == TRACE_IPPROTO_UDP) {           \
        udp = (libtrace_udp_t *)transport;      \
        if (rem >= 4) {                         \
            source_port = ntohs(udp->source);   \
            dest_port = ntohs(udp->dest);       \
        }                                       \
        payload = (uint8_t *)trace_get_payload_from_udp(udp, &rem);  \
        payloadlen = rem;                       \
    }                                           \
    else if (proto == TRACE_IPPROTO_TCP) {      \
        tcp = (libtrace_tcp_t *)transport;      \
        if (rem >= 4) {                         \
            source_port = ntohs(tcp->source);   \
            dest_port = ntohs(tcp->dest);       \
        }                                       \
        tcppayload = (uint8_t *)trace_get_payload_from_tcp(tcp, &rem);  \
        payloadlen = rem;                       \
    } else if (proto == TRACE_IPPROTO_ICMP) {   \
        icmp = (libtrace_icmp_t *)transport;    \
        if (rem >= 2) {                         \
            source_port = icmp->type;           \
            dest_port = icmp->code;             \
        }                                       \
    }                                           \


#define PREPROCESS_PACKET            \
    filter_params_t fparams;        \
    uint32_t rem = 0;               \
    uint16_t ethertype = 0;         \
                                    \
    memset(&fparams, 0, sizeof(filter_params_t)); \
    fparams.ip = (libtrace_ip_t *)trace_get_layer3(packet, &ethertype, &rem);      \
                                    \
    if (ethertype == TRACE_ETHERTYPE_IP) {                       \
        uint8_t proto = 0;          \
        void *transport = trace_get_payload_from_ip(fparams.ip, &proto, &rem);    \
        fparams.translen = rem;             \
                                    \
        /* XXX what about IP in IP?  */             \
        if (proto == TRACE_IPPROTO_UDP) {           \
            fparams.udp = (libtrace_udp_t *)transport;      \
            if (rem >= 4) {                         \
                fparams.source_port = ntohs(fparams.udp->source);   \
                fparams.dest_port = ntohs(fparams.udp->dest);       \
            }                                       \
            fparams.payload = (uint8_t *)trace_get_payload_from_udp(fparams.udp, &rem);  \
            fparams.payloadlen = rem;                       \
        }                                           \
        else if (proto == TRACE_IPPROTO_TCP) {      \
            fparams.tcp = (libtrace_tcp_t *)transport;      \
            if (rem >= 4) {                         \
                fparams.source_port = ntohs(fparams.tcp->source);   \
                fparams.dest_port = ntohs(fparams.tcp->dest);       \
            }                                       \
            fparams.payload = (uint8_t *)trace_get_payload_from_tcp(fparams.tcp, &rem);  \
            fparams.payloadlen = rem;                       \
        } else if (proto == TRACE_IPPROTO_ICMP) {   \
            fparams.icmp = (libtrace_icmp_t *)transport;    \
            if (rem >= 2) {                         \
                fparams.source_port = fparams.icmp->type;           \
                fparams.dest_port = fparams.icmp->code;             \
            }                                       \
        }                                           \
    }                               \


static int _apply_spoofing_filter(corsaro_logger_t *logger,
        filter_params_t *fparams) {

    if (_apply_abnormal_protocol_filter(logger, fparams) > 0) {
        return 1;
    }

    if (_apply_ttl200_nonspoofed_filter(logger, fparams->ip,
            fparams->tcp) > 0) {
        return 1;
    }

    if (_apply_fragment_filter(logger, fparams->ip) > 0) {
        return 1;
    }

    if (_apply_last_src_byte0_filter(logger, fparams->ip) > 0) {
        return 1;
    }

    if (_apply_last_src_byte255_filter(logger, fparams->ip) > 0) {
        return 1;
    }

    if (_apply_same_src_dest_filter(logger, fparams->ip) > 0) {
        return 1;
    }

    if (_apply_udp_port_zero_filter(logger, fparams) > 0) {
        return 1;
    }

    if (_apply_tcp_port_zero_filter(logger, fparams) > 0) {
        return 1;
    }

    if (_apply_udp_destport_eighty_filter(logger, fparams) > 0) {
        return 1;
    }
    return 0;
}

static int _apply_erratic_filter(corsaro_logger_t *logger,
        filter_params_t *fparams, bool spoofedstateunknown) {

    /* All spoofed packets are automatically erratic */
    if (spoofedstateunknown && _apply_spoofing_filter(logger, fparams) > 0) {
        return 1;
    }

    if (_apply_ttl200_filter(logger, fparams->ip) > 0) {
        return 1;
    }

    if (_apply_udp_0x31_filter(logger, fparams) > 0) {
        return 1;
    }

    if (_apply_sip_status_filter(logger, fparams) > 0) {
        return 1;
    }

    if (_apply_udp_iplen_96_filter(logger, fparams->ip) > 0) {
        return 1;
    }

    if (_apply_udp_iplen_1500_filter(logger, fparams->ip) > 0) {
        return 1;
    }
    
    if (_apply_port_53_filter(logger, fparams->source_port, fparams->dest_port) > 0) {
        return 1;
    }

    if (_apply_port_tcp23_filter(logger, fparams) > 0) {
        return 1;
    }

    if (_apply_port_tcp80_filter(logger, fparams) > 0) {
        return 1;
    }

    if (_apply_port_tcp5000_filter(logger, fparams) > 0) {
        return 1;
    }

    if (_apply_asn_208843_scan_filter(logger, fparams) > 0) {
        return 1;
    }

    if (_apply_dns_resp_oddport_filter(logger, fparams) > 0) { 
        return 1;
    }

    if (_apply_netbios_name_filter(logger, fparams) > 0) {
        return 1;
    }

    if (_apply_backscatter_filter(logger, fparams) > 0) {
        return 1;
    }

    if (_apply_bittorrent_filter(logger, fparams) > 0) {
        return 1;
    }

    return 0;
}

static inline int _apply_routable_filter(corsaro_logger_t *logger,
        libtrace_ip_t *ip) {
    return _apply_rfc5735_filter(logger, ip);
}

static inline int _apply_large_scale_scan_filter(corsaro_logger_t *logger,
        filter_params_t *fparams) {

    if (_apply_no_tcp_options_filter(logger, fparams->tcp) <= 0) {
        return 0;
    }

    if (_apply_ttl200_filter(logger, fparams->ip) <= 0) {
        return 0;
    }

    if (_apply_tcpwin_1024_filter(logger, fparams->tcp) <= 0) {
        return 0;
    }

    return 1;
}

int corsaro_apply_large_scale_scan_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {
    PREPROCESS_PACKET
    return _apply_large_scale_scan_filter(logger, &fparams);
}

int corsaro_apply_erratic_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {
    PREPROCESS_PACKET
    return _apply_erratic_filter(logger, &fparams, 1);
}

int corsaro_apply_spoofing_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {
    PREPROCESS_PACKET
    return _apply_spoofing_filter(logger, &fparams);
}

int corsaro_apply_routable_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_routable_filter(logger, fparams.ip);
}

int corsaro_apply_abnormal_protocol_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {
    PREPROCESS_PACKET
    return _apply_abnormal_protocol_filter(logger, &fparams);
}

int corsaro_apply_ttl200_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_ttl200_filter(logger, fparams.ip);
}

int corsaro_apply_ttl200_nonspoofed_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_ttl200_nonspoofed_filter(logger, fparams.ip, fparams.tcp);
}

int corsaro_apply_no_tcp_options_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {
    PREPROCESS_PACKET
    return _apply_no_tcp_options_filter(logger, fparams.tcp);
}

int corsaro_apply_tcpwin_1024_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {
    PREPROCESS_PACKET
    return _apply_tcpwin_1024_filter(logger, fparams.tcp);
}

int corsaro_apply_fragment_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_fragment_filter(logger, fparams.ip);
}

int corsaro_apply_last_src_byte0_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_last_src_byte0_filter(logger, fparams.ip);
}

int corsaro_apply_last_src_byte255_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {
    PREPROCESS_PACKET
    return _apply_last_src_byte255_filter(logger, fparams.ip);
}

int corsaro_apply_same_src_dest_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_same_src_dest_filter(logger, fparams.ip);
}

int corsaro_apply_udp_port_zero_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {
    PREPROCESS_PACKET
    return _apply_udp_port_zero_filter(logger, &fparams);
}

int corsaro_apply_tcp_port_zero_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_tcp_port_zero_filter(logger, &fparams);
}

int corsaro_apply_udp_destport_eighty_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_udp_destport_eighty_filter(logger, &fparams);
}

int corsaro_apply_rfc5735_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {
    PREPROCESS_PACKET
    return _apply_rfc5735_filter(logger, fparams.ip);
}

int corsaro_apply_backscatter_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_backscatter_filter(logger, &fparams);
}

int corsaro_apply_bittorrent_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_bittorrent_filter(logger, &fparams);
}

int corsaro_apply_udp_0x31_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_udp_0x31_filter(logger, &fparams);
}

int corsaro_apply_sip_status_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_sip_status_filter(logger, &fparams);
}

int corsaro_apply_udp_iplen_96_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_udp_iplen_96_filter(logger, fparams.ip);
}

int corsaro_apply_udp_iplen_1500_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_udp_iplen_1500_filter(logger, fparams.ip);
}

int corsaro_apply_port_53_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_port_53_filter(logger, fparams.source_port, fparams.dest_port);
}

int corsaro_apply_port_tcp23_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_port_tcp23_filter(logger, &fparams);
}

int corsaro_apply_port_tcp80_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_port_tcp80_filter(logger, &fparams);
}

int corsaro_apply_port_tcp5000_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_port_tcp5000_filter(logger, &fparams);
}

int corsaro_apply_asn_208843_scan_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_asn_208843_scan_filter(logger, &fparams);
}

int corsaro_apply_dns_resp_oddport_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {
    PREPROCESS_PACKET
    return _apply_dns_resp_oddport_filter(logger, &fparams);
}

int corsaro_apply_netbios_name_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    return _apply_netbios_name_filter(logger, &fparams);
}


const char *corsaro_get_builtin_filter_name(corsaro_logger_t *logger,
        corsaro_builtin_filter_id_t filtid) {

    char unknown[2048];

    if (filtid < 0 || filtid >= CORSARO_FILTERID_MAX) {
        corsaro_log(logger, "Attempted to get name for invalid filter using id %d",
                filtid);
        return NULL;
    }

    switch(filtid) {
        case CORSARO_FILTERID_SPOOFED:
            return "spoofed";
        case CORSARO_FILTERID_ERRATIC:
            return "erratic";
        case CORSARO_FILTERID_ROUTED:
            return "routed";
        case CORSARO_FILTERID_ABNORMAL_PROTOCOL:
            return "abnormal-protocol";
        case CORSARO_FILTERID_TTL_200:
            return "ttl-200";
        case CORSARO_FILTERID_TTL_200_NONSPOOFED:
            return "ttl-200-nonspoofed";
        case CORSARO_FILTERID_FRAGMENT:
            return "fragmented";
        case CORSARO_FILTERID_LAST_SRC_IP_0:
            return "last-byte-src-0";
        case CORSARO_FILTERID_LAST_SRC_IP_255:
            return "last-byte-src-255";
        case CORSARO_FILTERID_SAME_SRC_DEST_IP:
            return "same-src-dst";
        case CORSARO_FILTERID_UDP_PORT_0:
            return "udp-port-0";
        case CORSARO_FILTERID_TCP_PORT_0:
            return "tcp-port-0";
        case CORSARO_FILTERID_UDP_DESTPORT_80:
            return "udp-destport-80";
        case CORSARO_FILTERID_RFC5735:
            return "rfc5735";
        case CORSARO_FILTERID_BACKSCATTER:
            return "backscatter";
        case CORSARO_FILTERID_BITTORRENT:
            return "bittorrent";
        case CORSARO_FILTERID_UDP_0X31:
            return "udp-0x31";
        case CORSARO_FILTERID_SIP_STATUS:
            return "sip-status";
        case CORSARO_FILTERID_UDP_IPLEN_96:
            return "udp-ip-len-96";
        case CORSARO_FILTERID_UDP_IPLEN_1500:
            return "udp-ip-len-1500";
        case CORSARO_FILTERID_PORT_53:
            return "port-53";
        case CORSARO_FILTERID_TCP_PORT_23:
            return "tcp-port-23";
        case CORSARO_FILTERID_TCP_PORT_80:
            return "tcp-port-80";
        case CORSARO_FILTERID_TCP_PORT_5000:
            return "tcp-port-5000";
        case CORSARO_FILTERID_ASN_208843_SCAN:
            return "asn-208843-scan";
        case CORSARO_FILTERID_DNS_RESP_NONSTANDARD:
            return "dns-resp-non-standard";
        case CORSARO_FILTERID_NETBIOS_QUERY_NAME:
            return "netbios-query-name";
        default:
            corsaro_log(logger, "Warning: no filter name for id %d -- please add one to corsaro_get_builtin_filter_name()", filtid);
            snprintf(unknown, 2048, "unknown-%d", filtid);
            /* Naughty, returning a local variable address */
            return (const char *)unknown;
    }
    return NULL;
}

static inline void _set_filter_params(libtrace_ip_t *ip, uint32_t iprem,
        filter_params_t *fparams) {

    uint32_t rem = iprem;
    uint8_t proto = 0;
    void *transport = trace_get_payload_from_ip(ip, &proto, &rem);

    memset(fparams, 0, sizeof(filter_params_t));
    fparams->ip = ip;
    fparams->translen = rem;

    /* XXX what about IP in IP?  */
    if (proto == TRACE_IPPROTO_UDP) {
        fparams->udp = (libtrace_udp_t *)transport;
        if (rem >= 4) {
            fparams->source_port = ntohs(fparams->udp->source);
            fparams->dest_port = ntohs(fparams->udp->dest);
        }
        fparams->payload = (uint8_t *)trace_get_payload_from_udp(fparams->udp, &rem);
        fparams->payloadlen = rem;
    }
    else if (proto == TRACE_IPPROTO_TCP) {
        fparams->tcp = (libtrace_tcp_t *)transport;
        if (rem >= 4) {
            fparams->source_port = ntohs(fparams->tcp->source);
            fparams->dest_port = ntohs(fparams->tcp->dest);
        }
        fparams->payload = (uint8_t *)trace_get_payload_from_tcp(fparams->tcp, &rem);
        fparams->payloadlen = rem;
    } else if (proto == TRACE_IPPROTO_ICMP) {
        fparams->icmp = (libtrace_icmp_t *)transport;
        if (rem >= 2) {
            fparams->source_port = fparams->icmp->type;
            fparams->dest_port = fparams->icmp->code;
        }
    }

}

int corsaro_apply_all_filters(corsaro_logger_t *logger, libtrace_ip_t *ip,
        uint32_t iprem, corsaro_filter_torun_t *torun) {

    int i;
    filter_params_t fparams;

    _set_filter_params(ip, iprem, &fparams);

    for (i = 0; i < CORSARO_FILTERID_MAX; i++) {
        torun[i].filterid = i;
        torun[i].result = 255;
    }

    /* Trying to avoid using a 'switch' in here to save on processing time,
     * since we're going to be hitting all of the filter checks anyway.
     *
     * Similarly, I'm going to try and use the results for the more-specific
     * filters to automatically infer the result of the broader ones
     * (e.g. spoofed, erratic, etc.).
     *
     * This will make the code uglier, but also hopefully a bit faster */
    torun[CORSARO_FILTERID_TTL_200].result =
            _apply_ttl200_filter(logger, fparams.ip);
    torun[CORSARO_FILTERID_TTL_200_NONSPOOFED].result =
            _apply_ttl200_nonspoofed_filter(logger, fparams.ip, fparams.tcp);
    torun[CORSARO_FILTERID_NO_TCP_OPTIONS].result =
             _apply_no_tcp_options_filter(logger, fparams.tcp);
    torun[CORSARO_FILTERID_TCPWIN_1024].result =
             _apply_tcpwin_1024_filter(logger, fparams.tcp);

    if (torun[CORSARO_FILTERID_TTL_200].result == 1 &&
            torun[CORSARO_FILTERID_NO_TCP_OPTIONS].result == 1 &&
            torun[CORSARO_FILTERID_TCPWIN_1024].result == 1) {

        torun[CORSARO_FILTERID_LARGE_SCALE_SCAN].result = 1;
    }

    torun[CORSARO_FILTERID_ABNORMAL_PROTOCOL].result =
            _apply_abnormal_protocol_filter(logger, &fparams);
    torun[CORSARO_FILTERID_FRAGMENT].result =
            _apply_fragment_filter(logger, fparams.ip);
    torun[CORSARO_FILTERID_LAST_SRC_IP_0].result =
            _apply_last_src_byte0_filter(logger, fparams.ip);
    torun[CORSARO_FILTERID_LAST_SRC_IP_255].result =
            _apply_last_src_byte255_filter(logger, fparams.ip);
    torun[CORSARO_FILTERID_SAME_SRC_DEST_IP].result =
            _apply_same_src_dest_filter(logger, fparams.ip);
    torun[CORSARO_FILTERID_UDP_PORT_0].result =
            _apply_udp_port_zero_filter(logger, &fparams);
    torun[CORSARO_FILTERID_TCP_PORT_0].result =
            _apply_tcp_port_zero_filter(logger, &fparams);
    torun[CORSARO_FILTERID_UDP_DESTPORT_80].result =
            _apply_udp_destport_eighty_filter(logger, &fparams);

    if (torun[CORSARO_FILTERID_ABNORMAL_PROTOCOL].result == 1 ||
            torun[CORSARO_FILTERID_UDP_DESTPORT_80].result == 1 ||
            torun[CORSARO_FILTERID_FRAGMENT].result == 1 ||
            torun[CORSARO_FILTERID_LAST_SRC_IP_0].result == 1 ||
            torun[CORSARO_FILTERID_LAST_SRC_IP_255].result == 1 ||
            torun[CORSARO_FILTERID_SAME_SRC_DEST_IP].result == 1 ||
            torun[CORSARO_FILTERID_TTL_200_NONSPOOFED].result == 1 ||
            torun[CORSARO_FILTERID_UDP_PORT_0].result == 1 ||
            torun[CORSARO_FILTERID_TCP_PORT_0].result == 1) {

        torun[CORSARO_FILTERID_SPOOFED].result = 1;
        torun[CORSARO_FILTERID_ERRATIC].result = 1;
    }

    torun[CORSARO_FILTERID_RFC5735].result =
            _apply_rfc5735_filter(logger, fparams.ip);

    if (torun[CORSARO_FILTERID_RFC5735].result == 1) {
        torun[CORSARO_FILTERID_ROUTED].result = 1;
    }

    torun[CORSARO_FILTERID_NOTIP].result = _apply_notip_filter(logger,
            fparams.ip);

    torun[CORSARO_FILTERID_BACKSCATTER].result =
            _apply_backscatter_filter(logger, &fparams);
    torun[CORSARO_FILTERID_BITTORRENT].result =
            _apply_bittorrent_filter(logger, &fparams);
    torun[CORSARO_FILTERID_UDP_0X31].result =
            _apply_udp_0x31_filter(logger, &fparams);
    torun[CORSARO_FILTERID_SIP_STATUS].result =
            _apply_sip_status_filter(logger, &fparams);
    torun[CORSARO_FILTERID_UDP_IPLEN_96].result =
            _apply_udp_iplen_96_filter(logger, fparams.ip);
    torun[CORSARO_FILTERID_UDP_IPLEN_1500].result =
            _apply_udp_iplen_1500_filter(logger, fparams.ip);
    torun[CORSARO_FILTERID_PORT_53].result =
            _apply_port_53_filter(logger, fparams.source_port,
                        fparams.dest_port);
    torun[CORSARO_FILTERID_TCP_PORT_23].result =
            _apply_port_tcp23_filter(logger, &fparams);
    torun[CORSARO_FILTERID_TCP_PORT_80].result =
            _apply_port_tcp80_filter(logger, &fparams);
    torun[CORSARO_FILTERID_TCP_PORT_5000].result =
            _apply_port_tcp5000_filter(logger, &fparams);
    torun[CORSARO_FILTERID_ASN_208843_SCAN].result = 
            _apply_asn_208843_scan_filter(logger, &fparams);
    torun[CORSARO_FILTERID_DNS_RESP_NONSTANDARD].result =
            _apply_dns_resp_oddport_filter(logger, &fparams);
    torun[CORSARO_FILTERID_NETBIOS_QUERY_NAME].result =
            _apply_netbios_name_filter(logger, &fparams);

    if (torun[CORSARO_FILTERID_ERRATIC].result != 1 && (
            torun[CORSARO_FILTERID_BACKSCATTER].result == 1 ||
            torun[CORSARO_FILTERID_BITTORRENT].result == 1 ||
            torun[CORSARO_FILTERID_UDP_0X31].result == 1 ||
            torun[CORSARO_FILTERID_SIP_STATUS].result == 1 ||
            torun[CORSARO_FILTERID_UDP_IPLEN_96].result == 1 ||
            torun[CORSARO_FILTERID_UDP_IPLEN_1500].result == 1 ||
            torun[CORSARO_FILTERID_PORT_53].result == 1 ||
            torun[CORSARO_FILTERID_TCP_PORT_23].result == 1 ||
            torun[CORSARO_FILTERID_TCP_PORT_80].result == 1 ||
            torun[CORSARO_FILTERID_TCP_PORT_5000].result == 1 ||
            torun[CORSARO_FILTERID_ASN_208843_SCAN].result == 1 ||
            torun[CORSARO_FILTERID_TTL_200].result == 1 ||
            torun[CORSARO_FILTERID_DNS_RESP_NONSTANDARD].result == 1 ||
            torun[CORSARO_FILTERID_NETBIOS_QUERY_NAME].result == 1)) {

        torun[CORSARO_FILTERID_ERRATIC].result = 1;
    }
    return 0;
}

int corsaro_apply_multiple_filters(corsaro_logger_t *logger,
        libtrace_ip_t *ip, uint32_t iprem, corsaro_filter_torun_t *torun,
        int torun_count) {
    int i;
    uint8_t spoofedstate = -1;
    filter_params_t fparams;

    _set_filter_params(ip, iprem, &fparams);

    for (i = 0; i < torun_count; i++) {
        switch(torun[i].filterid) {
            case CORSARO_FILTERID_SPOOFED:
                torun[i].result = _apply_spoofing_filter(logger, &fparams);
                if (torun[i].result) {
                    spoofedstate = 1;
                } else {
	            spoofedstate = 0;
		}
                break;
            case CORSARO_FILTERID_ERRATIC:
                if (spoofedstate == 1) {
                    torun[i].result = 1;
                } else {
                    torun[i].result = _apply_erratic_filter(logger, &fparams,
                        (spoofedstate < 0));
                }
                break;
            case CORSARO_FILTERID_ROUTED:
                torun[i].result = _apply_routable_filter(logger, fparams.ip);
                break;
            case CORSARO_FILTERID_LARGE_SCALE_SCAN:
                torun[i].result = _apply_large_scale_scan_filter(logger,
                        &fparams);
                break;
            case CORSARO_FILTERID_ABNORMAL_PROTOCOL:
                torun[i].result = _apply_abnormal_protocol_filter(logger, &fparams);
                break;
            case CORSARO_FILTERID_TTL_200:
                torun[i].result =_apply_ttl200_filter(logger, fparams.ip);
                break;
            case CORSARO_FILTERID_TTL_200_NONSPOOFED:
                torun[i].result =_apply_ttl200_nonspoofed_filter(logger,
                        fparams.ip, fparams.tcp);
                break;
            case CORSARO_FILTERID_NO_TCP_OPTIONS:
                torun[i].result = _apply_no_tcp_options_filter(logger,
                        fparams.tcp);
                break;
            case CORSARO_FILTERID_TCPWIN_1024:
                torun[i].result = _apply_tcpwin_1024_filter(logger,
                        fparams.tcp);
                break;
            case CORSARO_FILTERID_FRAGMENT:
                torun[i].result =_apply_fragment_filter(logger, fparams.ip);
                break;
            case CORSARO_FILTERID_LAST_SRC_IP_0:
                torun[i].result =_apply_last_src_byte0_filter(logger, fparams.ip);
                break;
            case CORSARO_FILTERID_LAST_SRC_IP_255:
                torun[i].result =_apply_last_src_byte255_filter(logger, fparams.ip);
                break;
            case CORSARO_FILTERID_SAME_SRC_DEST_IP:
                torun[i].result =_apply_same_src_dest_filter(logger, fparams.ip);
                break;
            case CORSARO_FILTERID_UDP_PORT_0:
                torun[i].result =_apply_udp_port_zero_filter(logger, &fparams);
                break;
            case CORSARO_FILTERID_TCP_PORT_0:
                torun[i].result =_apply_tcp_port_zero_filter(logger, &fparams);
                break;
            case CORSARO_FILTERID_UDP_DESTPORT_80:
                torun[i].result =_apply_udp_destport_eighty_filter(logger, &fparams);
                break;
            case CORSARO_FILTERID_RFC5735:
                torun[i].result =_apply_rfc5735_filter(logger, fparams.ip);
                break;
            case CORSARO_FILTERID_BACKSCATTER:
                torun[i].result =_apply_backscatter_filter(logger, &fparams);
                break;
            case CORSARO_FILTERID_BITTORRENT:
                torun[i].result =_apply_bittorrent_filter(logger, &fparams);
                break;
            case CORSARO_FILTERID_UDP_0X31:
                torun[i].result =_apply_udp_0x31_filter(logger, &fparams);
                break;
            case CORSARO_FILTERID_SIP_STATUS:
                torun[i].result =_apply_sip_status_filter(logger, &fparams);
                break;
            case CORSARO_FILTERID_UDP_IPLEN_96:
                torun[i].result =_apply_udp_iplen_96_filter(logger, fparams.ip);
                break;
            case CORSARO_FILTERID_UDP_IPLEN_1500:
                torun[i].result =_apply_udp_iplen_1500_filter(logger, fparams.ip);
                break;
            case CORSARO_FILTERID_PORT_53:
                torun[i].result =_apply_port_53_filter(logger, fparams.source_port,
                        fparams.dest_port);
                break;
            case CORSARO_FILTERID_TCP_PORT_23:
                torun[i].result =_apply_port_tcp23_filter(logger, &fparams);
                break;
            case CORSARO_FILTERID_TCP_PORT_80:
                torun[i].result =_apply_port_tcp80_filter(logger, &fparams);
                break;
            case CORSARO_FILTERID_TCP_PORT_5000:
                torun[i].result =_apply_port_tcp5000_filter(logger, &fparams);
                break;
            case CORSARO_FILTERID_ASN_208843_SCAN:
                torun[i].result =_apply_asn_208843_scan_filter(logger, &fparams);
                break;
            case CORSARO_FILTERID_DNS_RESP_NONSTANDARD:
                torun[i].result =_apply_dns_resp_oddport_filter(logger, &fparams);
                break;
            case CORSARO_FILTERID_NETBIOS_QUERY_NAME:
                torun[i].result =_apply_netbios_name_filter(logger, &fparams);
                break;
            case CORSARO_FILTERID_NOTIP:
                torun[i].result = _apply_notip_filter(logger, fparams.ip);
                break;
            default:
                corsaro_log(logger, "Warning: no filter callback for id %d -- please add one to corsaro_apply_multiple_filters()", torun[i].filterid);
                return -1;
        }
    }
    return 0;
}

int corsaro_apply_filter_by_id(corsaro_logger_t *logger,
        corsaro_builtin_filter_id_t filtid, libtrace_packet_t *packet) {

    PREPROCESS_PACKET
    if (filtid < 0 || filtid >= CORSARO_FILTERID_MAX) {
        corsaro_log(logger, "Attempted to apply invalid filter using id %d",
                filtid);
        return -1;
    }

    switch(filtid) {
        case CORSARO_FILTERID_SPOOFED:
            return _apply_spoofing_filter(logger, &fparams);
        case CORSARO_FILTERID_ERRATIC:
            return _apply_erratic_filter(logger, &fparams, 1);
        case CORSARO_FILTERID_ROUTED:
            return _apply_routable_filter(logger, fparams.ip);
        case CORSARO_FILTERID_ABNORMAL_PROTOCOL:
            return _apply_abnormal_protocol_filter(logger, &fparams);
        case CORSARO_FILTERID_TTL_200:
            return _apply_ttl200_filter(logger, fparams.ip);
        case CORSARO_FILTERID_TTL_200_NONSPOOFED:
            return _apply_ttl200_nonspoofed_filter(logger, fparams.ip,
                    fparams.tcp);
        case CORSARO_FILTERID_FRAGMENT:
            return _apply_fragment_filter(logger, fparams.ip);
        case CORSARO_FILTERID_LAST_SRC_IP_0:
            return _apply_last_src_byte0_filter(logger, fparams.ip);
        case CORSARO_FILTERID_LAST_SRC_IP_255:
            return _apply_last_src_byte255_filter(logger, fparams.ip);
        case CORSARO_FILTERID_SAME_SRC_DEST_IP:
            return _apply_same_src_dest_filter(logger, fparams.ip);
        case CORSARO_FILTERID_UDP_PORT_0:
            return _apply_udp_port_zero_filter(logger, &fparams);
        case CORSARO_FILTERID_TCP_PORT_0:
            return _apply_tcp_port_zero_filter(logger, &fparams);
        case CORSARO_FILTERID_UDP_DESTPORT_80:
            return _apply_udp_destport_eighty_filter(logger, &fparams);
        case CORSARO_FILTERID_RFC5735:
            return _apply_rfc5735_filter(logger, fparams.ip);
        case CORSARO_FILTERID_BACKSCATTER:
            return _apply_backscatter_filter(logger, &fparams);
        case CORSARO_FILTERID_BITTORRENT:
            return _apply_bittorrent_filter(logger, &fparams);
        case CORSARO_FILTERID_UDP_0X31:
            return _apply_udp_0x31_filter(logger, &fparams);
        case CORSARO_FILTERID_SIP_STATUS:
            return _apply_sip_status_filter(logger, &fparams);
        case CORSARO_FILTERID_UDP_IPLEN_96:
            return _apply_udp_iplen_96_filter(logger, fparams.ip);
        case CORSARO_FILTERID_UDP_IPLEN_1500:
            return _apply_udp_iplen_1500_filter(logger, fparams.ip);
        case CORSARO_FILTERID_PORT_53:
            return _apply_port_53_filter(logger, fparams.source_port, fparams.dest_port);
        case CORSARO_FILTERID_TCP_PORT_23:
            return _apply_port_tcp23_filter(logger, &fparams);
        case CORSARO_FILTERID_TCP_PORT_80:
            return _apply_port_tcp80_filter(logger, &fparams);
        case CORSARO_FILTERID_TCP_PORT_5000:
            return _apply_port_tcp5000_filter(logger, &fparams);
        case CORSARO_FILTERID_ASN_208843_SCAN:
            return _apply_asn_208843_scan_filter(logger, &fparams);
        case CORSARO_FILTERID_DNS_RESP_NONSTANDARD:
            return _apply_dns_resp_oddport_filter(logger, &fparams);
        case CORSARO_FILTERID_NETBIOS_QUERY_NAME:
            return _apply_netbios_name_filter(logger, &fparams);
        default:
            corsaro_log(logger, "Warning: no filter callback for id %d -- please add one to corsaro_apply_filter_by_id()", filtid);
    }
    return -1;
}

int corsaro_apply_single_custom_filter(corsaro_logger_t *logger,
        corsaro_filter_t *filter, libtrace_packet_t *packet) {

    libtrace_filter_t *ltfilter = NULL;

    ltfilter = trace_create_filter(filter->filterstring);
    if (ltfilter && trace_apply_filter(ltfilter, packet) == 0) {
        /* Filter did not match */
        trace_destroy_filter(ltfilter);
        return 1;
    }
    trace_destroy_filter(ltfilter);
    /* Filter matched */
    return 0;
}

int corsaro_apply_custom_filters_AND(corsaro_logger_t *logger,
        libtrace_list_t *filtlist, libtrace_packet_t *packet) {

    libtrace_list_node_t *n;
    corsaro_filter_t *f;
    libtrace_filter_t *ltfilter = NULL;

    if (filtlist == NULL || filtlist->head == NULL) {
        return 1;
    }

    n = filtlist->head;
    while (n) {
        f = (corsaro_filter_t *)(n->data);
        n = n->next;

        ltfilter = trace_create_filter(f->filterstring);
        if (ltfilter && trace_apply_filter(ltfilter, packet) == 0) {
            trace_destroy_filter(ltfilter);
            return 1;
        }
        trace_destroy_filter(ltfilter);
    }

    /* All filters matched, packet is OK */
    return 0;
}

int corsaro_apply_custom_filters_OR(corsaro_logger_t *logger,
        libtrace_list_t *filtlist, libtrace_packet_t *packet) {

    libtrace_list_node_t *n;
    corsaro_filter_t *f;
    libtrace_filter_t *ltfilter = NULL;
    int matched = 0;

    if (filtlist == NULL || filtlist->head == NULL) {
        return 1;
    }

    n = filtlist->head;
    while (n) {
        f = (corsaro_filter_t *)(n->data);
        n = n->next;

        ltfilter = trace_create_filter(f->filterstring);
        if (ltfilter && trace_apply_filter(ltfilter, packet) > 0) {
            matched = 1;
            trace_destroy_filter(ltfilter);
            break;
        }
        trace_destroy_filter(ltfilter);
    }

    if (matched > 0) {
        /* At least one filter matched, so packet is OK */
        return 0;
    }

    /* No filters in the list matched, discard packet */
    return 1;
}

libtrace_list_t *corsaro_create_filters(corsaro_logger_t *logger,
        char *fname) {

    /* TODO */
    return NULL;
}

void corsaro_destroy_filters(libtrace_list_t *filtlist) {

    libtrace_list_node_t *n;
    corsaro_filter_t *f;

    if (filtlist == NULL) {
        return;
    }

    n = filtlist->head;
    while (n) {
        f = (corsaro_filter_t *)(n->data);
        if (f->filtername) {
            free(f->filtername);
        }
        if (f->filterstring) {
            free(f->filterstring);
        }
        n = n->next;
    }

    libtrace_list_deinit(filtlist);
}

// vim: set sw=4 tabstop=4 softtabstop=4 expandtab :
