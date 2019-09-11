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

#include "report_internal.h"
#include "corsaro_report.h"

/** Structure for maintaining state for each IP tracker thread that a
 *  packet processing thread will be sending updates to.
 */
typedef struct corsaro_report_tracker_state {

    char *msgbuffer;
    uint32_t msgbufsize;
    char *nextwrite;
    uint32_t seqno;
    corsaro_report_ipmsg_header_t *header;

    void *tracker_queue;

} corsaro_report_tracker_state_t;

/** Packet processing thread state for the report plugin */
typedef struct corsaro_report_state {

    /** State for building and sending IP->tag updates to each of the
     *  IP tracker threads.
     */
    corsaro_report_tracker_state_t *totracker;

    /** An identifier for this packet processing thread */
    int threadid;

    /** Timestamp of the most recent interval */
    uint32_t current_interval;

} corsaro_report_state_t;

#define TRACKER_BUF_REM(t) \
		(t->msgbufsize - (t->nextwrite - t->msgbuffer))

#define BUFFER_SIZE_INCREMENT \
		((sizeof(corsaro_report_single_ip_header_t) + \
		 (10 * sizeof(corsaro_report_msg_tag_t))) * 100)

#define INIT_MSGBUFFER_SIZE \
		(REPORT_BATCH_SIZE * (sizeof(corsaro_report_single_ip_header_t) + \
		 (10 * sizeof(corsaro_report_msg_tag_t))))

static void init_ipmsg_header(corsaro_report_tracker_state_t *track,
		int threadid) {

	track->header->msgtype = CORSARO_IP_MESSAGE_UPDATE;
	track->header->sender = threadid;
	track->header->timestamp = 0;
	track->header->bodycount = 0;
	track->header->seqno = track->seqno;
    track->header->tagcount = 0;
}

static int extend_message_buffer(corsaro_report_tracker_state_t *track) {
	int used = track->nextwrite - track->msgbuffer;

	track->msgbuffer = realloc(track->msgbuffer, track->msgbufsize +
			BUFFER_SIZE_INCREMENT);
	track->msgbufsize += BUFFER_SIZE_INCREMENT;

	if (track->msgbuffer == NULL) {
		return -1;
	}

	track->nextwrite = track->msgbuffer + used;
	track->header = (corsaro_report_ipmsg_header_t *)track->msgbuffer;
	return 0;
}

/** Helper function to quickly find the IP addresses from a libtrace packet.
 *  Also extracts the IP length from the IP header as well.
 *
 *  @param packet           The libtrace packet to get IP addresses from.
 *  @param srcaddr          Pointer to a location to write the source IP into
 *  @param dstaddr          Pointer to a location to write the dest IP into
 *  @param iplen            Pointer to a location to write the IP length into.
 *  @return 0 if successful, -1 if this is not an IPv4 packet or some of the
 *          IP header is missing.
 *  @note This function works for IPv4 only!
 */
static inline int extract_addresses(libtrace_packet_t *packet,
        uint32_t *srcaddr, uint32_t *dstaddr, uint16_t *iplen) {

    libtrace_ip_t *ip;
    void *l3;
    uint16_t ethertype;
    uint32_t rem;

    l3 = trace_get_layer3(packet, &ethertype, &rem);

    if (l3 == NULL || rem == 0) {
        return -1;
    }

    if (ethertype != TRACE_ETHERTYPE_IP) {
        return -1;
    }

    if (rem < sizeof(libtrace_ip_t)) {
        return -1;
    }
    ip = (libtrace_ip_t *)l3;

    *srcaddr = ip->ip_src.s_addr;
    *dstaddr = ip->ip_dst.s_addr;
    *iplen = ntohs(ip->ip_len);
    return 0;
}

/** Check if the basic tags (port, protocol, etc) are valid for a tag set.
 *
 *  @param tags         The set of tags to evaluate.
 *  @return 1 if the basic tags are valid, 0 if they are not.
 */
static inline int basic_tagged(corsaro_packet_tags_t *tags) {
    if (tags->providers_used & 0x01) {
        return 1;
    }
    return 0;
}

/** Check if the maxmind geo-location tags are valid for a tag set.
 *
 *  @param tags         The set of tags to evaluate.
 *  @return 1 if the maxmind tags are valid, 0 if they are not.
 */
static inline int maxmind_tagged(corsaro_packet_tags_t *tags) {
    if (tags->providers_used & (1 << IPMETA_PROVIDER_MAXMIND)) {
        return 1;
    }
    return 0;
}

/** Check if the netacq-edge geo-location tags are valid for a tag set.
 *
 *  @param tags         The set of tags to evaluate.
 *  @return 1 if the netacq-edge tags are valid, 0 if they are not.
 */
static inline int netacq_tagged(corsaro_packet_tags_t *tags) {
    if (tags->providers_used & (1 << IPMETA_PROVIDER_NETACQ_EDGE)) {
        return 1;
    }
    return 0;
}

/** Check if the prefix2asn tags are valid for a tag set.
 *
 *  @param tags         The set of tags to evaluate.
 *  @return 1 if the prefix2asn tags are valid, 0 if they are not.
 */
static inline int pfx2as_tagged(corsaro_packet_tags_t *tags) {
    if (tags->providers_used & (1 << IPMETA_PROVIDER_PFX2AS)) {
        return 1;
    }
    return 0;
}

static char *metclasstostr(corsaro_report_metric_class_t class) {

    switch(class) {
        case CORSARO_METRIC_CLASS_COMBINED:
            return "combined";
        case CORSARO_METRIC_CLASS_IP_PROTOCOL:
            return "IP protocol";
        case CORSARO_METRIC_CLASS_ICMP_TYPE:
            return "ICMP type";
        case CORSARO_METRIC_CLASS_ICMP_CODE:
            return "ICMP code";
        case CORSARO_METRIC_CLASS_TCP_SOURCE_PORT:
            return "TCP source port";
        case CORSARO_METRIC_CLASS_TCP_DEST_PORT:
            return "TCP dest port";
        case CORSARO_METRIC_CLASS_UDP_SOURCE_PORT:
            return "UDP source port";
        case CORSARO_METRIC_CLASS_UDP_DEST_PORT:
            return "UDP dest port";
        case CORSARO_METRIC_CLASS_MAXMIND_CONTINENT:
            return "Maxmind continent";
        case CORSARO_METRIC_CLASS_MAXMIND_COUNTRY:
            return "Maxmind country";
        case CORSARO_METRIC_CLASS_NETACQ_CONTINENT:
            return "Netacq continent";
        case CORSARO_METRIC_CLASS_NETACQ_COUNTRY:
            return "Netacq country";
        case CORSARO_METRIC_CLASS_NETACQ_REGION:
            return "Netacq region";
        case CORSARO_METRIC_CLASS_NETACQ_POLYGON:
            return "Netacq polygon";
        case CORSARO_METRIC_CLASS_PREFIX_ASN:
            return "pfx2as ASN";
    }

    return "unknown";

}

/** Adds a new metric tag to an IP update message.
 *
 *  @param class        The class of the metric that we are adding
 *  @param tagval       The value for the metric that we are adding
 *  @param maxtagval    The maximum allowable value for this metric class.
 *                      If 0, there is no upper limit.
 *  @param state        The packet processing thread state for this plugin
 *  @param body         The IP update that the tag is being added to
 *  @param logger       A reference to a corsaro logger for error reporting
 */
static inline int process_single_tag(corsaro_report_metric_class_t class,
        uint32_t tagval, uint32_t maxtagval,
        corsaro_report_tracker_state_t *track,
        corsaro_logger_t *logger, uint16_t pktbytes) {

    uint64_t metricid;
    corsaro_report_msg_tag_t *tag;

    /* Sanity checking for metrics that have clearly defined bounds */
    if (maxtagval > 0 && tagval >= maxtagval) {
        corsaro_log(logger, "Invalid %s tag: %u", metclasstostr(class),
                tagval);
        return 0;
    }

	if (TRACKER_BUF_REM(track) < sizeof(corsaro_report_msg_tag_t)) {
		if (extend_message_buffer(track) < 0) {
			return -1;
		}
	}

    metricid = GEN_METRICID(class, tagval);
	tag = (corsaro_report_msg_tag_t *)track->nextwrite;
	tag->tagid = metricid;
	tag->bytes = pktbytes;
	tag->packets = 1;

	track->nextwrite += sizeof(corsaro_report_msg_tag_t);
	return 1;

}


/** Creates and initialises packet processing thread state for the report
 *  plugin. This state must be passed into all subsequent packet processing
 *  and interval boundary callbacks for the report plugin.
 *
 *  @param p        A reference to the running instance of the report plugin
 *  @param threadid A unique number identifying the packet processing thread
 *                  that has called this callback.
 *  @return A pointer to the newly created plugin-processing state.
 */
void *corsaro_report_init_processing(corsaro_plugin_t *p, int threadid) {

    corsaro_report_state_t *state;
    corsaro_report_config_t *conf;
    int i;

    conf = (corsaro_report_config_t *)(p->config);
    state = (corsaro_report_state_t *)malloc(sizeof(corsaro_report_state_t));

    state->current_interval = 0;
    state->threadid = threadid;
    state->totracker = (corsaro_report_tracker_state_t *)calloc(
            conf->tracker_count, sizeof(corsaro_report_tracker_state_t));

    /* Maintain state for each of the IP tracker threads. As we
     * process packets, we'll fill each of the metric maps depending on which
     * IPs are seen in the processed packets. Once we have enough data, the
     * map contents will be pushed to the appropriate IP tracker thread and
     * we will start accumulating data afresh.
     */
    for (i = 0; i < conf->tracker_count; i++) {
        state->totracker[i].msgbuffer = calloc(INIT_MSGBUFFER_SIZE, 1);
        state->totracker[i].msgbufsize = INIT_MSGBUFFER_SIZE;
		state->totracker[i].nextwrite = state->totracker[i].msgbuffer +
				sizeof(corsaro_report_ipmsg_header_t);
		state->totracker[i].header = (corsaro_report_ipmsg_header_t *)
				state->totracker[i].msgbuffer;
        state->totracker[i].seqno = 0;
        state->totracker[i].tracker_queue =
            conf->tracker_queues[i * conf->basic.procthreads + state->threadid];

		init_ipmsg_header(&(state->totracker[i]), threadid);
    }

    return state;
}

static int send_iptracker_message(corsaro_report_tracker_state_t *track,
		corsaro_logger_t *logger) {

    int postheader = track->nextwrite - track->msgbuffer -
            sizeof(corsaro_report_ipmsg_header_t);
	track->header->seqno = track->seqno;
	track->seqno ++;

    if (zmq_send(track->tracker_queue, (void *)track->header,
            sizeof(corsaro_report_ipmsg_header_t), ZMQ_SNDMORE) < 0) {
        return -1;
    }

	if (zmq_send(track->tracker_queue,
            (void *)(track->msgbuffer + sizeof(corsaro_report_ipmsg_header_t)),
			postheader, 0) < 0) {
		return -1;
	}

	track->nextwrite = track->msgbuffer + sizeof(corsaro_report_ipmsg_header_t);

	track->header->bodycount = 0;
	track->header->tagcount = 0;
	track->header->seqno = 0;
	track->header->timestamp = 0;

	return 0;
}

/** Tidies up packet processing thread state for the report plugin and
 *  halts the IP tracker threads.
 *
 *  @param p        A reference to the running instance of the report plugin
 *  @param local    The packet processing thread state for this plugin.
 *  @return 0 if successful, -1 if an error occurred.
 */
int corsaro_report_halt_processing(corsaro_plugin_t *p, void *local) {

    corsaro_report_state_t *state;
    corsaro_report_ipmsg_header_t msg;
    corsaro_report_config_t *conf;
    int i;

    conf = (corsaro_report_config_t *)(p->config);
    state = (corsaro_report_state_t *)local;

    if (state == NULL) {
        return 0;
    }

    /* Tell all of the IP tracker threads to halt */
    memset(&msg, 0, sizeof(msg));
    msg.msgtype = CORSARO_IP_MESSAGE_HALT;
    msg.sender = state->threadid;
    msg.seqno = 0;

    for (i = 0; i < conf->tracker_count; i++) {
        /* If there are any outstanding updates, send those first */
        if (state->totracker[i].header->bodycount > 0) {

            if (send_iptracker_message(&(state->totracker[i]), p->logger) < 0) {
                corsaro_log(p->logger,
                        "error while pushing final IP result to tracker thread %d: %s",
                        i, strerror(errno));
            }
        }

        /* Send the halt message */
        if (zmq_send(state->totracker[i].tracker_queue,
                (void *)(&msg), sizeof(corsaro_report_ipmsg_header_t), 0) < 0) {
            corsaro_log(p->logger,
                    "error while pushing halt to tracker thread %d: %s",
                    i, strerror(errno));
        }

		free(state->totracker[i].msgbuffer);
    }

    /* Wait for the tracker threads to stop */
    for (i = 0; i < conf->tracker_count; i++) {
        if (conf->iptrackers[i].tid != 0) {
            pthread_join(conf->iptrackers[i].tid, NULL);
            conf->iptrackers[i].tid = 0;
        }
    }

    free(state->totracker);
    free(state);

    return 0;
}

/** Updates the report plugin state in response to the commencement of
 *  a new interval.
 *
 *  @param p            A reference to the running instance of the report plugin
 *  @param local        The packet processing thread state for this plugin.
 *  @param int_start    The details of the interval that has now started.
 *  @return 0 if successful, -1 if an error occurs.
 */
int corsaro_report_start_interval(corsaro_plugin_t *p, void *local,
        corsaro_interval_t *int_start) {

    corsaro_report_state_t *state;

    state = (corsaro_report_state_t *)local;
    if (state != NULL) {
        /* Save the interval start time, since this is what we will send
         * to the IP tracker threads once the interval ends.
         */
        state->current_interval = int_start->time;
    }
    return 0;
}

/** Updates the report plugin state in response to the ending of an interval
 *  and returns any saved data that needs to be passed on to the merging
 *  thread so it can correctly combine the results for all of the processing
 *  threads.
 *
 *  @param p            A reference to the running instance of the report plugin
 *  @param local        The packet processing thread state for this plugin.
 *  @param int_end      The details of the interval that has now ended.
 *  @param complete     Flag indicating whether the interval was a complete
 *                      one or a partial one.
 *  @return A pointer to an interim result structure that is to be combined
 *          with the corresponding interim results produced by the other
 *          packet processing threads, or NULL if there are no useful results
 *          for this interval.
 */
void *corsaro_report_end_interval(corsaro_plugin_t *p, void *local,
        corsaro_interval_t *int_end, uint8_t complete) {

    corsaro_report_config_t *conf;
    corsaro_report_state_t *state;
    corsaro_report_interim_t *interim;
    corsaro_report_ipmsg_header_t msg;
    int i;

    conf = (corsaro_report_config_t *)(p->config);
    state = (corsaro_report_state_t *)local;

    if (state == NULL) {
        corsaro_log(p->logger,
                "corsaro_report_end_interval: report thread-local state is NULL!");
        return NULL;
    }
    /* Tell the IP tracker threads that there will be no more updates
     * coming from this processing thread for this interval.
     */
    memset(&msg, 0, sizeof(msg));

    if (complete) {
        msg.msgtype = CORSARO_IP_MESSAGE_INTERVAL;
        interim = (corsaro_report_interim_t *)malloc(
                sizeof(corsaro_report_interim_t));
        interim->baseconf = conf;
    } else {
        msg.msgtype = CORSARO_IP_MESSAGE_RESET;
        interim = NULL;
    }

    msg.timestamp = state->current_interval;
    msg.sender = state->threadid;

    for (i = 0; i < conf->tracker_count; i++) {
        if (state->totracker[i].header->bodycount > 0) {
            if (send_iptracker_message(&(state->totracker[i]), p->logger) < 0) {
                corsaro_log(p->logger,
                        "error while pushing end-interval result to tracker thread %d: %s",
                        i, strerror(errno));
            }
        }

        msg.seqno = state->totracker[i].seqno;
        state->totracker[i].seqno ++;
        if (zmq_send(state->totracker[i].tracker_queue,
                (void *)(&msg), sizeof(corsaro_report_ipmsg_header_t), 0) < 0) {
            corsaro_log(p->logger,
                    "error while pushing end-interval to tracker thread %d: %s",
                    i, strerror(errno));
        }
    }

    return (void *)interim;
}

#define PROCESS_SINGLE_TAG(class, val, maxval) \
    if (allowedmetricclasses == 0 || \
            ((1UL << class) & allowedmetricclasses)) { \
        if ((ret = process_single_tag(class, val, maxval, track, logger, \
                iplen)) < 0) { \
            return -1; \
        } else { \
            newtags += ret; \
        } \
    }

/** Insert all of the tags in a tag set into an IP update message that will
 *  be forwarded to an IP tracker thread.
 *
 *  All of the tags in the tag set should be derived from the same packet.
 *
 *  @param track		The state for the socket linking us with the IP
 * 						tracker thread that will receive this update
 *  @param tags         The set of tags to insert into the IP update
 *  @param iplen        The number of IP-layer bytes in the original packet
 *  @param logger       A reference to a corsaro logger for error reporting
 */
static int process_tags(corsaro_report_tracker_state_t *track,
		corsaro_packet_tags_t *tags, uint16_t iplen,
        corsaro_logger_t *logger, uint64_t allowedmetricclasses) {

    int i, ret;
    uint16_t newtags = 0;

    /* "Combined" is simply a total across all metrics, i.e. the total
     * number of packets, source IPs etc. Every IP packet should add to
     * the combined tally.
     */

    PROCESS_SINGLE_TAG(CORSARO_METRIC_CLASS_COMBINED, 0, 0);

    if (!tags || tags->providers_used == 0) {
        return newtags;
    }

    PROCESS_SINGLE_TAG(CORSARO_METRIC_CLASS_IP_PROTOCOL, tags->protocol,
            METRIC_IPPROTOS_MAX);

    if (tags->protocol == TRACE_IPPROTO_ICMP) {
        PROCESS_SINGLE_TAG(CORSARO_METRIC_CLASS_ICMP_TYPE, tags->src_port,
                METRIC_ICMP_MAX);
        PROCESS_SINGLE_TAG(CORSARO_METRIC_CLASS_ICMP_CODE, tags->dest_port,
                METRIC_ICMP_MAX);
    } else if (tags->protocol == TRACE_IPPROTO_TCP) {
        PROCESS_SINGLE_TAG(CORSARO_METRIC_CLASS_TCP_SOURCE_PORT,
                tags->src_port, METRIC_PORT_MAX);
        PROCESS_SINGLE_TAG(CORSARO_METRIC_CLASS_TCP_DEST_PORT,
                tags->dest_port, METRIC_PORT_MAX);
    } else if (tags->protocol == TRACE_IPPROTO_UDP) {
        PROCESS_SINGLE_TAG(CORSARO_METRIC_CLASS_UDP_SOURCE_PORT,
                tags->src_port, METRIC_PORT_MAX);
        PROCESS_SINGLE_TAG(CORSARO_METRIC_CLASS_UDP_DEST_PORT,
                tags->dest_port, METRIC_PORT_MAX);
    }

    if (maxmind_tagged(tags)) {
        PROCESS_SINGLE_TAG(CORSARO_METRIC_CLASS_MAXMIND_CONTINENT,
                tags->maxmind_continent, 0);
        PROCESS_SINGLE_TAG(CORSARO_METRIC_CLASS_MAXMIND_COUNTRY,
                tags->maxmind_country, 0);
    }

    if (netacq_tagged(tags)) {
        PROCESS_SINGLE_TAG(CORSARO_METRIC_CLASS_NETACQ_CONTINENT,
                tags->netacq_continent, 0);
        PROCESS_SINGLE_TAG(CORSARO_METRIC_CLASS_NETACQ_COUNTRY,
                tags->netacq_country, 0);
        PROCESS_SINGLE_TAG(CORSARO_METRIC_CLASS_NETACQ_REGION,
                tags->netacq_region, 0);
        for (i = 0; i < MAX_NETACQ_POLYGONS; i++) {
            if (tags->netacq_polygon[i] == 0) {
                continue;
            }
            PROCESS_SINGLE_TAG(CORSARO_METRIC_CLASS_NETACQ_POLYGON,
                    tags->netacq_polygon[i], 0);
        }
    }

    if (pfx2as_tagged(tags)) {
        PROCESS_SINGLE_TAG(CORSARO_METRIC_CLASS_PREFIX_ASN, tags->prefixasn,
                0);
    }
	return newtags;
}


/** Form an IP update message for a set of tags and ensure that it is queued
 *  for the correct IP tracker thread.
 *
 *  All of the tags in the tag set should be derived from the same packet.
 *
 *  @param conf         The global configuration for this plugin
 *  @param state        The packet processing thread state for this plugin
 *  @param addr         An IP address from the original packet
 *  @param issrc        Set to 1 if 'addr' is the source IP address, 0 if
 *                      'addr' is the destination IP address.
 *  @param iplen        The number of IP-layer bytes in the original packet
 *  @param tags         The set of tags to insert into the IP update
 *  @param logger       A reference to a corsaro logger for error reporting
 */
static inline int update_metrics_for_address(corsaro_report_config_t *conf,
        corsaro_report_state_t *state, uint32_t addr, uint8_t issrc,
        uint16_t iplen, corsaro_packet_tags_t *tags, corsaro_logger_t *logger) {

	int trackerhash;
    int ipoffset;
	uint16_t newtags;
    corsaro_report_msg_tag_t *tag;
	corsaro_report_single_ip_header_t *singleip;
	corsaro_report_tracker_state_t *track;

	/* Hash IPs to IP tracker threads based on the suffix octet of the IP
     * address -- should be reasonably balanced + easy to calculate.
     */
    trackerhash = (addr >> 24) % conf->tracker_count;
	track = &(state->totracker[trackerhash]);

	if (TRACKER_BUF_REM(track) < sizeof(corsaro_report_single_ip_header_t)) {
		if (extend_message_buffer(track) < 0) {
			corsaro_log(logger, "OOM when attempting to extend a message buffer!");
			return -1;
		}
	}

	track->header->bodycount ++;
    /* Reserve space for the IP address header */
    ipoffset = track->nextwrite - track->msgbuffer;

	track->nextwrite += sizeof(corsaro_report_single_ip_header_t);

	newtags = process_tags(track, tags, iplen, logger,
            conf->allowedmetricclasses);

    /* Due to potential buffer reallocation, singleip may not point anywhere
     * valid anymore. */
    singleip = (corsaro_report_single_ip_header_t *)
            (track->msgbuffer + ipoffset);
	singleip->ipaddr = addr;
	singleip->issrc = issrc;
    singleip->numtags = newtags;

    track->header->tagcount += newtags;

	if (track->header->bodycount < REPORT_BATCH_SIZE) {
		return 1;
	}

	if (send_iptracker_message(track, logger) < 0) {
		corsaro_log(logger,
                "error while pushing result to tracker thread %d: %s",
                trackerhash, strerror(errno));
		return -1;
	}
	return 1;
}

/** Update the reported metrics based on the content of a single packet.
 *
 *  @param p            A reference to the running instance of the report plugin
 *  @param local        The packet processing thread state for this plugin.
 *  @param packet       The packet that is being used to update the metrics.
 *  @param tags         The tags associated with this packet by the libcorsaro
 *                      tagging component.
 *  @return 0 if the packet was successfully processed, -1 if an error occurs.
 */
int corsaro_report_process_packet(corsaro_plugin_t *p, void *local,
        libtrace_packet_t *packet, corsaro_packet_tags_t *tags) {

    corsaro_report_state_t *state;
    uint16_t iplen;
    uint32_t srcaddr, dstaddr;
    corsaro_report_config_t *conf;

    conf = (corsaro_report_config_t *)(p->config);
    state = (corsaro_report_state_t *)local;

    if (state == NULL) {
        corsaro_log(p->logger,
                "corsaro_report_process_packet: report thread-local state is NULL!");
        return -1;
    }

    if (extract_addresses(packet, &srcaddr, &dstaddr, &iplen) != 0) {
        return 0;
    }
    /* Update our metrics observed for the source address */
    if (update_metrics_for_address(conf, state, srcaddr, 1, iplen, tags,
			p->logger) < 0) {
		return -1;
	}
    /* Update our metrics observed for the destination address */
    if (update_metrics_for_address(conf, state, dstaddr, 0, iplen, tags,
			p->logger) < 0) {
		return -1;
	}
    return 0;
}


// vim: set sw=4 tabstop=4 softtabstop=4 expandtab :