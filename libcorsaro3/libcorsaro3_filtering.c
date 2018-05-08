/*
 * corsaro
 *
 * Alistair King, CAIDA, UC San Diego
 * Shane Alcock, WAND, University of Waikato
 *
 * corsaro-info@caida.org
 *
 * Copyright (C) 2012 The Regents of the University of California.
 *
 * This file is part of corsaro.
 *
 * corsaro is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * corsaro is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with corsaro.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdlib.h>
#include <string.h>
#include <libtrace.h>

#include "libcorsaro3_filtering.h"
#include "libcorsaro3_log.h"

static inline int _apply_ttl200_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    libtrace_ip_t *ip;

    ip = trace_get_ip(packet);
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

static inline int _apply_fragment_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    libtrace_ip_t *ip;

    ip = trace_get_ip(packet);
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
        libtrace_packet_t *packet) {

    libtrace_ip_t *ip;

    ip = trace_get_ip(packet);
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
        libtrace_packet_t *packet) {

    libtrace_ip_t *ip;

    ip = trace_get_ip(packet);
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
        libtrace_packet_t *packet) {

    libtrace_ip_t *ip;

    ip = trace_get_ip(packet);
    if (!ip) {
        return -1;
    }

    if (ip->ip_src.s_addr == ip->ip_dst.s_addr) {
        return 1;
    }
    return 0;
}

static inline int _apply_udp_port_zero_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    libtrace_udp_t *udp;
    void *transport;
    uint8_t proto;
    uint32_t rem;

    transport = trace_get_transport(packet, &proto, &rem);
    if (!transport) {
        return -1;
    }

    if (proto != TRACE_IPPROTO_UDP) {
        return 0;
    }

    if (rem < 4) {
        return -1;
    }
    udp = (libtrace_udp_t *)transport;

    /* Don't bother byteswapping since zero is the same regardless */
    if (udp->source == 0 || udp->dest == 0) {
        return 1;
    }

    return 0;
}

static inline int _apply_tcp_port_zero_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    libtrace_tcp_t *tcp;
    void *transport;
    uint8_t proto;
    uint32_t rem;

    transport = trace_get_transport(packet, &proto, &rem);
    if (!transport) {
        return -1;
    }

    if (proto != TRACE_IPPROTO_TCP) {
        return 0;
    }

    if (rem < 4) {
        return -1;
    }
    tcp = (libtrace_tcp_t *)transport;

    /* Don't bother byteswapping since zero is the same regardless */
    if (tcp->source == 0 || tcp->dest == 0) {
        return 1;
    }

    return 0;
}

static inline int _apply_udp_0x31_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    void *transport;
    uint8_t *udppayload;
    libtrace_udp_t *udp;
    uint8_t proto;
    uint32_t rem;
    libtrace_ip_t *ip;
    uint8_t pattern[10] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x31, 0x00};

    ip = trace_get_ip(packet);
    if (!ip) {
        return -1;
    }

    transport = trace_get_transport(packet, &proto, &rem);
    if (!transport) {
        return -1;
    }

    if (proto != TRACE_IPPROTO_UDP) {
        return 0;
    }

    udp = (libtrace_udp_t *)transport;
    udppayload = (uint8_t *)trace_get_payload_from_udp(udp, &rem);

    if (udppayload == NULL) {
        return 0;
    }

    if (rem < 10) {
        return 0;
    }

    if (ntohs(ip->ip_len) != 58) {
        return 0;
    }

    if (memcmp(pattern, udppayload, 10) == 0) {
        return 1;
    }

    return 0;
}

static inline int _apply_udp_iplen_96_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    libtrace_ip_t *ip;
    void *transport;
    uint8_t proto;
    uint32_t rem;

    ip = trace_get_ip(packet);
    if (!ip) {
        return -1;
    }

    transport = trace_get_transport(packet, &proto, &rem);
    if (proto == TRACE_IPPROTO_UDP && ntohs(ip->ip_len) == 96) {
        return 1;
    }

    return 0;
}

static inline int _apply_port_53_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    if (trace_get_source_port(packet) == 53 ||
            trace_get_destination_port(packet) == 53) {
        return 1;
    }

    return 0;
}

static inline int _apply_port_tcp23_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    if (trace_get_tcp(packet) != NULL &&
            (trace_get_source_port(packet) == 23 ||
             trace_get_destination_port(packet) == 23)) {
        return 1;
    }

    return 0;
}

static inline int _apply_port_tcp80_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    if (trace_get_tcp(packet) != NULL &&
            (trace_get_source_port(packet) == 80 ||
             trace_get_destination_port(packet) == 80)) {
        return 1;
    }

    return 0;
}

static inline int _apply_port_tcp5000_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    if (trace_get_tcp(packet) != NULL &&
            (trace_get_source_port(packet) == 5000 ||
             trace_get_destination_port(packet) == 5000)) {
        return 1;
    }

    return 0;
}

static inline int _apply_dns_resp_oddport_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    void *transport;
    uint8_t proto;
    uint32_t rem;
    libtrace_udp_t *udp;
    uint16_t *udppayload;
    libtrace_ip_t *ip;

    ip = trace_get_ip(packet);
    if (!ip) {
        return -1;
    }

    transport = trace_get_transport(packet, &proto, &rem);
    if (!transport) {
        return -1;
    }

    if (proto != TRACE_IPPROTO_UDP) {
        return 0;
    }

    udp = (libtrace_udp_t *)transport;
    udppayload = (uint16_t *)trace_get_payload_from_udp(udp, &rem);

    if (ntohs(ip->ip_len) <= 42) {
        return 0;
    }

    if (rem < 20 - sizeof(libtrace_udp_t)) {
        return 0;
    }

    /* Check flags and codes */
    if ((ntohs(udppayload[1]) & 0xfff0) != 0x8180) {
        return 0;
    }

    /* Question count */
    if (ntohs(udppayload[2]) >= 10) {
        return 0;
    }

    /* Answer record count */
    if (ntohs(udppayload[3]) >= 10) {
        return 0;
    }

    /* NS (authority record) count */
    if (ntohs(udppayload[4]) >= 10) {
        return 0;
    }

    /* Additional record count */
    if (ntohs(udppayload[5]) >= 10) {
        return 0;
    }

    return 1;
}

static inline int _apply_netbios_name_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    void *transport;
    uint8_t proto;
    uint32_t rem;
    libtrace_udp_t *udp;
    uint16_t *udppayload;
    libtrace_ip_t *ip;

    ip = trace_get_ip(packet);
    if (!ip) {
        return -1;
    }

    transport = trace_get_transport(packet, &proto, &rem);
    if (!transport) {
        return -1;
    }

    if (proto != TRACE_IPPROTO_UDP) {
        return 0;
    }

    udp = (libtrace_udp_t *)transport;
    udppayload = (uint16_t *)trace_get_payload_from_udp(udp, &rem);

    if (ntohs(udp->source) != 137 && ntohs(udp->dest) != 137) {
        return 0;
    }

    if (ntohs(ip->ip_len) <= 48) {
        return 0;
    }

    if (rem < 28 - sizeof(libtrace_udp_t)) {
        return 0;
    }

    if (ntohs(udppayload[6]) != 0x2043) {
        return 0;
    }

    if (ntohs(udppayload[7]) != 0x4b41) {
        return 0;
    }

    if (ntohs(udppayload[8]) != 0x4141) {
        return 0;
    }

    if (ntohs(udppayload[9]) != 0x4141) {
        return 0;
    }

    return 1;
}

static inline int _apply_backscatter_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    libtrace_tcp_t *tcp;
    libtrace_udp_t *udp;
    libtrace_icmp_t *icmp;
    void *transport;
    uint8_t proto;
    uint32_t rem;

    transport = trace_get_transport(packet, &proto, &rem);
    if (!transport) {
        return -1;
    }

    /* TODO return different positive values for filter quality analysis */

    /* UDP backscatter -- just DNS for now */
    if (proto == TRACE_IPPROTO_UDP) {
        udp = (libtrace_udp_t *)transport;
        if (rem < 2) {
            return -1;
        }
        if (ntohs(udp->source) == 53) {
            return 1;
        }
    }

    if (proto == TRACE_IPPROTO_ICMP) {
        icmp = (libtrace_icmp_t *)transport;
        if (rem < 1) {
            return -1;
        }
        switch(icmp->type) {
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
    }

    if (proto == TRACE_IPPROTO_TCP) {
        tcp = (libtrace_tcp_t *)transport;
        if (rem < sizeof(libtrace_tcp_t)) {
            return -1;
        }
        /* No SYN-ACKs and no RSTs */
        if ((tcp->syn && tcp->ack) || tcp->rst) {
            return 1;
        }
    }
    return 0;
}

static inline int _apply_rfc5735_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    libtrace_ip_t *ip;
    uint32_t srcip;

    ip = trace_get_ip(packet);
    if (!ip) {
        return -1;
    }

    srcip = ntohl(ip->ip_src.s_addr);

    /* TODO return different positive values for filter quality analysis */

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

    /* 169.254.0.0/16 */
    if ((srcip & 0xffff0000) == 0xa9fe0000) {
        return 1;
    }

    /* 172.16.0.0/12 */
    if ((srcip & 0xfff00000) == 0xac100000) {
        return 1;
    }

    /* 192.0.0.0/24 */
    if ((srcip & 0xffffff00) == 0xc0000000) {
        return 1;
    }

    /* 192.0.2.0/24 */
    if ((srcip & 0xffffff00) == 0xc0000200) {
        return 1;
    }

    /* 192.88.99.0/24 */
    if ((srcip & 0xffffff00) == 0xc0586300) {
        return 1;
    }

    /* 192.168.0.0/16 */
    if ((srcip & 0xffff0000) == 0xc0a80000) {
        return 1;
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

    /* 224.0.0.0/4 */
    if ((srcip & 0xf0000000) == 0xe0000000) {
        return 1;
    }

    /* 240.0.0.0/4 */
    if ((srcip & 0xf0000000) == 0xf0000000) {
        return 1;
    }

    return 0;
}

static inline int _apply_abnormal_protocol_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    uint8_t ipproto;
    uint32_t rem;
    void *transport;
    libtrace_tcp_t *tcp;

    transport = trace_get_transport(packet, &ipproto, &rem);
    if (!transport) {
        return -1;
    }

    if (ipproto == TRACE_IPPROTO_ICMP || ipproto == TRACE_IPPROTO_UDP) {
        return 0;
    }

    if (ipproto == TRACE_IPPROTO_IPV6) {
        return 0;
    }

    if (ipproto != TRACE_IPPROTO_TCP) {
        return 1;
    }

    tcp = (libtrace_tcp_t *)transport;
    if (rem < sizeof(libtrace_tcp_t)) {
        return -1;       // filter it?
    }

    /* Allow normal TCP flag combos */
    /* XXX this is a bit silly looking, can we optimise somehow? */

    /* TODO return different positive values for filter quality analysis */
    /* SYN */
    if (tcp->syn && !tcp->ack && !tcp->fin && !tcp->psh && !tcp->rst &&
            !tcp->urg) {
        return 0;
    }

    /* ACK */
    if (tcp->ack && !tcp->syn && !tcp->fin && !tcp->psh && !tcp->rst &&
            !tcp->urg) {
        return 0;
    }

    /* RST */
    if (tcp->rst && !tcp->syn && !tcp->ack && !tcp->fin && !tcp->psh &&
            !tcp->urg) {
        return 0;
    }

    /* FIN */
    if (tcp->fin && !tcp->syn && !tcp->ack && !tcp->rst && !tcp->psh &&
            !tcp->urg) {
        return 0;
    }

    /* SYN-FIN */
    if (tcp->fin && tcp->syn && !tcp->ack && !tcp->rst && !tcp->psh &&
            !tcp->urg) {
        return 0;
    }

    /* SYN-ACK */
    if (tcp->syn && tcp->ack && !tcp->fin && !tcp->rst && !tcp->psh &&
            !tcp->urg) {
        return 0;
    }

    /* FIN-ACK */
    if (tcp->fin && tcp->ack && !tcp->syn && !tcp->rst && !tcp->psh &&
            !tcp->urg) {
        return 0;
    }

    /* ACK-PSH */
    if (tcp->ack && tcp->psh && !tcp->syn && !tcp->rst && !tcp->fin &&
            !tcp->urg) {
        return 0;
    }

    /* FIN-ACK-PSH */
    if (tcp->fin && tcp->ack && tcp->psh && !tcp->syn && !tcp->rst &&
            !tcp->urg) {
        return 0;
    }

    /* Every other flag combo is "abnormal" */
    return 1;
}

static inline int _apply_bittorrent_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    void *transport;
    uint8_t proto;
    uint32_t rem;
    libtrace_udp_t *udp;
    uint16_t *udppayload;
    libtrace_ip_t *ip;
    uint32_t *ptr32;
    uint16_t udplen;
    uint8_t last10pat[10] = {0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00};

    ip = trace_get_ip(packet);
    if (!ip) {
        return -1;
    }

    transport = trace_get_transport(packet, &proto, &rem);
    if (!transport) {
        return -1;
    }

    if (proto != TRACE_IPPROTO_UDP) {
        return 0;
    }

    udp = (libtrace_udp_t *)transport;
    udppayload = (uint16_t *)trace_get_payload_from_udp(udp, &rem);
    ptr32 = (uint32_t *)(udppayload);
    udplen = ntohs(udp->len);

    /* XXX This filter is frightening and should definitely be double
     * checked. */

    /* TODO return different positive values for filter quality analysis */
    if (udplen >= 20 && rem >= 12) {
        if (ntohl(ptr32[0]) == 0x64313a61 || ntohl(ptr32[0]) == 0x64313a72) {
            if (ntohl(ptr32[1] == 0x64323a69) && ntohl(ptr32[2] == 0x6432303a))
            {
                return 1;
            }
        }
    }

    if (udplen >= 48 && rem >= 40) {
        if (ntohl(ptr32[5]) == 0x13426974 && ntohl(ptr32[6]) == 0x546f7272 &&
                ntohl(ptr32[7]) == 0x656e7420 &&
                ntohl(ptr32[8]) == 0x70726f74 &&
                ntohl(ptr32[9]) == 0x6f636f6c) {
            return 1;
        }
    }

    if (ntohs(ip->ip_len) >= 0x3a) {
        if (ntohs(udppayload[0]) == 0x4102 || ntohs(udppayload[0]) == 0x2102
                || ntohs(udppayload[0]) == 0x3102
                || ntohs(udppayload[0]) == 0x1102) {

            if (rem >= udplen - sizeof(libtrace_udp_t)) {
                uint8_t *ptr8 = (uint8_t *)udppayload;
                ptr8 += (udplen - 10);
                if (memcmp(ptr8, last10pat, 10) == 0) {
                    return 1;
                }
            }
        }
    }

    if (ntohs(ip->ip_len) == 0x30) {
        if (ntohs(udppayload[0]) == 0x4100 || ntohs(udppayload[0]) == 0x2100
                || ntohs(udppayload[0]) == 0x3100
                || ntohs(udppayload[0]) == 0x1100) {

            return 1;
        }
    }

    if (ntohs(ip->ip_len) == 61) {
        if (ntohl(ptr32[3]) == 0x7fffffff && ntohl(ptr32[4]) == 0xab020400 &&
                ntohl(ptr32[5]) == 0x01000000 &&
                ntohl(ptr32[6]) == 0x08000000 && ptr32[7] == 0) {
            return 1;
        }
    }

    return 0;
}

int corsaro_apply_spoofing_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    if (_apply_abnormal_protocol_filter(logger, packet) > 0) {
        return 1;
    }

    if (_apply_ttl200_filter(logger, packet) > 0) {
        return 1;
    }

    if (_apply_fragment_filter(logger, packet) > 0) {
        return 1;
    }

    if (_apply_last_src_byte0_filter(logger, packet) > 0) {
        return 1;
    }

    if (_apply_last_src_byte255_filter(logger, packet) > 0) {
        return 1;
    }

    if (_apply_same_src_dest_filter(logger, packet) > 0) {
        return 1;
    }

    if (_apply_udp_port_zero_filter(logger, packet) > 0) {
        return 1;
    }

    if (_apply_tcp_port_zero_filter(logger, packet) > 0) {
        return 1;
    }
    return 0;
}

int corsaro_apply_erratic_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    if (_apply_udp_0x31_filter(logger, packet) > 0) {
        return 1;
    }

    if (_apply_udp_iplen_96_filter(logger, packet) > 0) {
        return 1;
    }

    if (_apply_port_53_filter(logger, packet) > 0) {
        return 1;
    }

    if (_apply_port_tcp23_filter(logger, packet) > 0) {
        return 1;
    }

    if (_apply_port_tcp80_filter(logger, packet) > 0) {
        return 1;
    }

    if (_apply_port_tcp5000_filter(logger, packet) > 0) {
        return 1;
    }

    if (_apply_dns_resp_oddport_filter(logger, packet) > 0) {
        return 1;
    }

    if (_apply_netbios_name_filter(logger, packet) > 0) {
        return 1;
    }

    if (_apply_backscatter_filter(logger, packet) > 0) {
        return 1;
    }

    if (_apply_bittorrent_filter(logger, packet) > 0) {
        return 1;
    }

    return 0;
}

int corsaro_apply_routable_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_rfc5735_filter(logger, packet);
}

int corsaro_apply_abnormal_protocol_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_abnormal_protocol_filter(logger, packet);
}

int corsaro_apply_ttl200_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_ttl200_filter(logger, packet);
}

int corsaro_apply_fragment_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_fragment_filter(logger, packet);
}

int corsaro_apply_last_src_byte0_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_last_src_byte0_filter(logger, packet);
}

int corsaro_apply_last_src_byte255_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_last_src_byte255_filter(logger, packet);
}

int corsaro_apply_same_src_dest_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_same_src_dest_filter(logger, packet);
}

int corsaro_apply_udp_port_zero_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_udp_port_zero_filter(logger, packet);
}

int corsaro_apply_tcp_port_zero_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_tcp_port_zero_filter(logger, packet);
}

int corsaro_apply_rfc5735_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_rfc5735_filter(logger, packet);
}

int corsaro_apply_backscatter_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_backscatter_filter(logger, packet);
}

int corsaro_apply_bittorrent_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_bittorrent_filter(logger, packet);
}

int corsaro_apply_udp_0x31_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_udp_0x31_filter(logger, packet);
}

int corsaro_apply_udp_iplen_96_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_udp_iplen_96_filter(logger, packet);
}

int corsaro_apply_port_53_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_port_53_filter(logger, packet);
}

int corsaro_apply_port_tcp23_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_port_tcp23_filter(logger, packet);
}

int corsaro_apply_port_tcp80_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_port_tcp80_filter(logger, packet);
}

int corsaro_apply_port_tcp5000_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_port_tcp5000_filter(logger, packet);
}

int corsaro_apply_dns_resp_oddport_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_dns_resp_oddport_filter(logger, packet);
}

int corsaro_apply_netbios_name_filter(corsaro_logger_t *logger,
        libtrace_packet_t *packet) {

    return _apply_netbios_name_filter(logger, packet);
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