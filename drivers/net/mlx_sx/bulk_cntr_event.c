/*
 * Copyright (c) 2010-2020,  Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/skbuff.h>

#include <linux/mlx_sx/device.h>
#include <linux/mlx_sx/auto_registers/reg.h>
#include <linux/mlx_sx/auto_registers/cmd_auto.h>
#include "bulk_cntr_db.h"
#include "bulk_cntr_event.h"

#define EVENT_EMAD_HDR_TID(skb) (be64_to_cpu(((struct emad_operation*)((skb)->data + sizeof(struct sx_eth_hdr)))->tid))
#define EVENT_REG_OFFSET(skb)        \
    ((skb)->data +                   \
     sizeof(struct sx_eth_hdr) +     \
     sizeof(struct emad_operation) + \
     sizeof(struct sxd_emad_tlv_reg))

typedef void (*sx_bulk_cntr_reg_parser_t)(const void                                *reg,
                                          u32                                        reg_len,
                                          struct sxd_bulk_cntr_buffer_layout_common *layout_common,
                                          void                                      *context);

int send_trap(const void    *buf,
              const uint32_t buf_size,
              uint16_t       trap_id,
              u8             is_from_user,
              u8             device_id,
              pid_t          target_pid);

/* function to parse multi-reg EMAD frame */
static void __sx_bulk_cntr_multi_emad_parse(const void               *buff,
                                            u32                       buff_len,
                                            sx_bulk_cntr_reg_parser_t reg_parser_cb,
                                            void                     *context)
{
    const struct sx_eth_hdr                   *emad_eth_header = (struct sx_eth_hdr*)buff; /* ethernet header */
    const struct sxd_emad_tlv_reg             *tlv;
    struct sxd_bulk_cntr_buffer_layout_common *layout_common;
    sxd_bulk_cntr_event_id_t                   event_id;
    u32                                        buff_remain = buff_len;
    u16                                        type, len;
    int                                        err;

    /* check that buffer is long enough */
    if (buff_len < sizeof(*emad_eth_header) + sizeof(struct emad_operation)) {
        printk(KERN_ERR "SX_BULK_CNTR: buffer is too small [buff_len=%u]\n", buff_len);
        return;
    }

    /* skip the EMAD ethernet header */
    buff_remain -= sizeof(*emad_eth_header);

    /* check that the first TLV is OPERATION_TLV and then skip it */
    tlv = (struct sxd_emad_tlv_reg*)(((u8*)buff) + sizeof(*emad_eth_header));

    type = sxd_emad_tlv_type(tlv);
    len = sxd_emad_tlv_len(tlv);

    if (type != TLV_TYPE_OPERATION_E) {
        printk(KERN_ERR "SX_BULK_CNTR: operation TLV is missing in buffer\n");
        return;
    }

    if (len != sizeof(struct emad_operation)) {
        printk(KERN_ERR "SX_BULK_CNTR: operation TLV has bad length (len=%u)\n", len);
        return;
    }

    event_id.event_id_value = cpu_to_be64(((struct emad_operation*)tlv)->tid);
    err = bulk_cntr_db_event_id_to_buffer(&event_id, &layout_common);
    if (err) {
        printk(KERN_ERR "SX_BULK_CNTR: unable to find buffer for event_id %llu\n", event_id.event_id_value);
        return;
    }

    buff_remain -= sizeof(struct emad_operation);
    tlv = (struct sxd_emad_tlv_reg*)(((u8*)tlv) + sizeof(struct emad_operation));

    /* time to parse the registers until END_TLV */
    while (buff_remain >= sizeof(*tlv)) {
        type = sxd_emad_tlv_type(tlv);
        len = sxd_emad_tlv_len(tlv);

        if ((type != TLV_TYPE_REG_E) && (type != TLV_TYPE_END_E)) {
            printk(KERN_ERR "SX_BULK_CNTR: unexpected TLV in event (type=%u)\n", type);
            break;
        }

        /* in case of END_TLV, just stop parsing */
        if (type == TLV_TYPE_END_E) {
            break;
        }

        if (len == 0) {
            printk(KERN_ERR "SX_BULK_CNTR: REG-TLV length is 0\n");
            break;
        }

        if (len > buff_remain) {
            printk(KERN_ERR "SX_BULK_CNTR: REG-TLV unexpected length (len=%u, buff_len=%u)\n", len, buff_remain);
            break;
        }

        /* call the reg-parser callback */
        reg_parser_cb(((u8*)tlv) + sizeof(struct sxd_emad_tlv_reg),
                      len - sizeof(struct sxd_emad_tlv_reg),
                      layout_common,
                      context);

        /* go to the next register in the frame */
        tlv = (struct sxd_emad_tlv_reg*)(((u8*)tlv) + len);
        buff_remain -= len;
    }

    if (type != TLV_TYPE_END_E) {
        printk(KERN_ERR "SX_BULK_CNTR: unexpected end of frame (no END_TLV)\n");
    }
}


static int __send_notification_to_user(const sxd_bulk_cntr_event_id_t  *ev_id,
                                       unsigned long                    buffer_id,
                                       enum sxd_bulk_cntr_done_status_e status,
                                       uint32_t                         cookie)
{
    sxd_bulk_counter_done_notification_t event = {
        .status = status,
        .buffer_id = buffer_id,
        .cookie = cookie,
    };
    int                                  err;

    /* User will use the cookie to identify whether this notification is what he wants. */
    err = send_trap(&event,
                    sizeof(event),
                    SXD_TRAP_ID_BULK_COUNTER_DONE_EVENT,
                    0,
                    1,
                    TARGET_PID_DONT_CARE);
    if (err) {
        printk(KERN_ERR "failed to send notification for bulk-counter transaction completion\n");
    }

    return err;
}


/* this function is called in cases that the driver needs ack from SDK
 * to complete a bulk-counter transaction. For example, when flow-counter
 * bulk-read is done, SDK has to unlock the range of counters before the user
 * get notified that the transaction is done.
 */
int sx_bulk_cntr_handle_ack(const sxd_bulk_cntr_event_id_t *ev_id, unsigned long buffer_id)
{
    enum sxd_bulk_cntr_done_status_e status;
    uint32_t                         cookie;

    /* complete the transaction: get status and delete from DB */
    int err = bulk_cntr_db_complete(ev_id, NULL, &status, &cookie);

    if (err) {
        printk(KERN_ERR "Bulk-Cntr ACK: failed to complete the transaction\n");
        goto out;
    }

    /* send notification to user upon transaction completion */
    err = __send_notification_to_user(ev_id, buffer_id, status, cookie);
    if (err) {
        printk(KERN_ERR "Bulk-Cntr SDK ack: failed to send notification to user upon transaction completion\n");
    }

out:
    return err;
}

/***************************************************************************************
 * MOCS_DONE
 */

static void __mocs_done_parser(const void                                *mocs_reg,
                               u32                                        reg_len,
                               struct sxd_bulk_cntr_buffer_layout_common *layout_common,
                               void                                      *context)
{
    sxd_bulk_cntr_event_id_t         event_id;
    unsigned long                    buffer_id;
    enum sxd_bulk_cntr_done_status_e status;
    uint32_t                         cookie;
    u64                              emad_tid = *((u64*)context);
    int                              err;

    event_id.event_id_value = (((uint64_t)mlxsw_reg_mocs_hi_get(mocs_reg)) << 32) | mlxsw_reg_mocs_lo_get(mocs_reg);

    if (emad_tid != event_id.event_id_value) {
        /* according to PRM they should be equal in MOCS_DONE event */
        printk("MOCS_DONE event is corrupted: emad_tid=%llu, event_tid=%llu\n", emad_tid, event_id.event_id_value);
        return;
    }

    if (event_id.event_id_fields.type == SXD_BULK_CNTR_KEY_TYPE_FLOW_E) {
        /* we don't send the notification to user now. First, SDK has to acknowledge that
         * it has unlocked the counters. we're waiting for the ack that will be handled
         * in sx_bulk_cntr_handle_ack() */
        return;
    }

    /* complete the transaction: get status and delete from DB */
    err = bulk_cntr_db_complete(&event_id, &buffer_id, &status, &cookie);
    if (err) {
        printk(KERN_ERR "failed to get transaction status on bulk-counter completion\n");
        return;
    }

    /* send notification to user upon transaction completion */
    err = __send_notification_to_user(&event_id, buffer_id, status, cookie);
    if (err) {
        printk(KERN_ERR "Bulk-Cntr FW done - failed to send notification to user upon transaction completion\n");
    }
}


void sx_bulk_cntr_handle_mocs_done(struct completion_info *ci)
{
    u64 emad_tid;

    emad_tid = EVENT_EMAD_HDR_TID(ci->skb);
    __sx_bulk_cntr_multi_emad_parse(ci->skb->data,
                                    ci->skb->len,
                                    __mocs_done_parser,
                                    &emad_tid);
}

/***************************************************************************************
 * PPCNT
 */
#define CHECK_PPCNT_GRP_EXPECTED(bulk_cntr_port_grp)                                            \
    do {                                                                                        \
        if (!(layout_port->mappings.counter_set_bitmap & bulk_cntr_port_grp)) {                 \
            printk(KERN_ERR "Bulk-Cntr PPCNT parser - unexpected PPCNT group %u\n", ppcnt.grp); \
            return;                                                                             \
        }                                                                                       \
    } while (0)

#define CHECK_RECEIVED_PPCNT_COUNT()                                                       \
    do {                                                                                   \
        if (layout_common->counters_received_so_far == layout_common->num_of_counters) {   \
            printk(KERN_ERR "Bulk-Cntr PPCNT parser - got more counters than required\n"); \
            return;                                                                        \
        }                                                                                  \
    } while (0)

#define CHECK_PPCNT_GRP_AND_COUNT(grp) \
    do {                               \
        CHECK_PPCNT_GRP_EXPECTED(grp); \
        CHECK_RECEIVED_PPCNT_COUNT();  \
    } while (0)


#define CHECK_PPCNT_TC_GRP_AND_COUNT()                                                                    \
    do {                                                                                                  \
        CHECK_PPCNT_GRP_EXPECTED(SXD_BULK_CNTR_PORT_GRP_TC_E);                                            \
        if (ppcnt.prio_tc > SXD_PORT_TC_ID_MAX) {                                                         \
            printk(KERN_ERR "Bulk-Cntr PPCNT parser - invalid traffic class %u\n", ppcnt.prio_tc);        \
            return;                                                                                       \
        }                                                                                                 \
        if (!SXD_BULK_CNTR_PORT_BITMAP_GET_TC(layout_port->mappings.counter_set_bitmap, ppcnt.prio_tc)) { \
            printk(KERN_ERR "Bulk-Cntr PPCNT parser - unexpected traffic class %u\n", ppcnt.prio_tc);     \
            return;                                                                                       \
        }                                                                                                 \
        CHECK_RECEIVED_PPCNT_COUNT();                                                                     \
    } while (0)

#define GET_PPCNT_COUNTER_64(dst_struct_name, dst_field_name, src_struct_name, src_field_name)                \
    do {                                                                                                      \
        counter_64 = (((sxd_port_cntr_t)(ppcnt.counter_set.src_struct_name.src_field_name ## _high)) << 32) + \
                     ppcnt.counter_set.src_struct_name.src_field_name ## _low;                                \
        if (port_cntr_index != layout_port->counters_size) {                                                  \
            layout_port->counters[port_cntr_index].dst_struct_name.dst_field_name = counter_64;               \
        }                                                                                                     \
        if (lag_cntr_index != layout_port->counters_size) {                                                   \
            layout_port->counters[lag_cntr_index].dst_struct_name.dst_field_name += counter_64;               \
        }                                                                                                     \
    } while (0)

#define GET_PPCNT_COUNTER_64_PER_PRIO(dst_struct_name, dst_field_name, src_struct_name, src_field_name)        \
    do {                                                                                                       \
        counter_64 = (((sxd_port_cntr_t)(ppcnt.counter_set.src_struct_name.src_field_name ## _high)) << 32) +  \
                     ppcnt.counter_set.src_struct_name.src_field_name ## _low;                                 \
        if (port_cntr_index != layout_port->counters_size) {                                                   \
            layout_port->counters[port_cntr_index].dst_struct_name[ppcnt.prio_tc].dst_field_name = counter_64; \
        }                                                                                                      \
        if (lag_cntr_index != layout_port->counters_size) {                                                    \
            layout_port->counters[lag_cntr_index].dst_struct_name[ppcnt.prio_tc].dst_field_name += counter_64; \
        }                                                                                                      \
    } while (0)


#define GET_PPCNT_COUNTER_64_PER_PRIO_WITH_CACHE(dst_struct_name,                                                  \
                                                 dst_field_name,                                                   \
                                                 src_struct_name,                                                  \
                                                 src_field_name,                                                   \
                                                 cache_field_name)                                                 \
    do {                                                                                                           \
        counter_64 = (((sxd_port_cntr_t)(ppcnt.counter_set.src_struct_name.src_field_name ## _high)) << 32) +      \
                     ppcnt.counter_set.src_struct_name.src_field_name ## _low + cntr_prio_cache->cache_field_name; \
        cntr_prio_buff.cache_field_name = counter_64;                                                              \
        if (port_cntr_index != layout_port->counters_size) {                                                       \
            layout_port->counters[port_cntr_index].dst_struct_name[ppcnt.prio_tc].dst_field_name = counter_64;     \
        }                                                                                                          \
        if (lag_cntr_index != layout_port->counters_size) {                                                        \
            layout_port->counters[lag_cntr_index].dst_struct_name[ppcnt.prio_tc].dst_field_name += counter_64;     \
        }                                                                                                          \
    } while (0)

#define GET_PPCNT_COUNTER_32(dst_struct_name, dst_field_name, src_struct_name, src_field_name)  \
    do {                                                                                        \
        counter_32 = ppcnt.counter_set.src_struct_name.src_field_name;                          \
        if (port_cntr_index != layout_port->counters_size) {                                    \
            layout_port->counters[port_cntr_index].dst_struct_name.dst_field_name = counter_32; \
        }                                                                                       \
        if (lag_cntr_index != layout_port->counters_size) {                                     \
            layout_port->counters[lag_cntr_index].dst_struct_name.dst_field_name += counter_32; \
        }                                                                                       \
    } while (0)

static void __ppcnt_parser(const void                                *ppcnt_reg,
                           u32                                        reg_len,
                           struct sxd_bulk_cntr_buffer_layout_common *layout_common,
                           void                                      *context)
{
    int                                 ret = 0;
    struct ku_ppcnt_reg                 ppcnt;
    uint16_t                            lag_id;
    sxd_bulk_cntr_buffer_layout_port_t *layout_port = (sxd_bulk_cntr_buffer_layout_port_t*)layout_common;
    bool                                is_per_prio_marked;
    bool                                is_per_tc_marked;
    bool                                is_per_pg_marked;
    uint16_t                            port_cntr_index;
    uint16_t                            lag_cntr_index;
    sxd_port_cntr_t                     counter_64;
    sxd_port_cntr32_t                   counter_32;
    sxd_port_cntr_prio_t               *cntr_prio_cache = NULL;
    sxd_port_cntr_prio_t                cntr_prio_buff;
    int                                 err = 0;

    memset(&ppcnt, 0, sizeof(ppcnt));

    if (reg_len < MLXSW_PPCNT_LEN) {
        printk(KERN_ERR "Bulk-Cntr PPCNT parser - reg_len is less than expected len (reg_len=%u, expected=%u)\n",
               reg_len, MLXSW_PPCNT_LEN);
        return;
    }

    ret = __PPCNT_decode((u8*)ppcnt_reg, &ppcnt, context);
    if (ret != 0) {
        printk(KERN_ERR "Bulk-Cntr PPCNT parser - failed to decode PPCNT\n");
        return;
    }

    /* local_port range is [1 - MAX_PHYPORT_NUM] */
    if (ppcnt.local_port > MAX_PHYPORT_NUM) {
        printk(KERN_ERR "Bulk-Cntr PPCNT parser - invalid local port %u\n", ppcnt.local_port);
        return;
    }

    port_cntr_index = layout_port->mappings.port_index_map[ppcnt.local_port];
    lag_id = layout_port->mappings.port_to_lag_map[ppcnt.local_port];
    if (lag_id != MAX_LAG_NUM) {
        lag_cntr_index = layout_port->mappings.lag_index_map[lag_id];
    } else {
        lag_cntr_index = layout_port->counters_size;
    }

    if ((port_cntr_index >= layout_port->counters_size) && (lag_cntr_index >= layout_port->counters_size)) {
        printk(KERN_ERR "Bulk-Cntr PPCNT parser - unexpected local port %u\n", ppcnt.local_port);
        return;
    }

    switch (ppcnt.grp) {
    case SXD_PPCNT_GRP_IEEE_802_3_COUNTERS_E:
        CHECK_PPCNT_GRP_AND_COUNT(SXD_BULK_CNTR_PORT_GRP_IEEE_802_DOT_3_E);
        GET_PPCNT_COUNTER_64(ieee_802_dot_3,
                             a_frames_transmitted_ok,
                             eth_802_3_cntrs_grp_data_layout,
                             a_frames_transmitted_ok);
        GET_PPCNT_COUNTER_64(ieee_802_dot_3,
                             a_frames_received_ok,
                             eth_802_3_cntrs_grp_data_layout,
                             a_frames_received_ok);
        GET_PPCNT_COUNTER_64(ieee_802_dot_3,
                             a_frame_check_sequence_errors,
                             eth_802_3_cntrs_grp_data_layout,
                             a_frame_check_sequence_errors);
        GET_PPCNT_COUNTER_64(ieee_802_dot_3, a_alignment_errors, eth_802_3_cntrs_grp_data_layout, a_alignment_errors);
        GET_PPCNT_COUNTER_64(ieee_802_dot_3,
                             a_octets_transmitted_ok,
                             eth_802_3_cntrs_grp_data_layout,
                             a_octets_transmitted_ok);
        GET_PPCNT_COUNTER_64(ieee_802_dot_3,
                             a_octets_received_ok,
                             eth_802_3_cntrs_grp_data_layout,
                             a_octets_received_ok);
        GET_PPCNT_COUNTER_64(ieee_802_dot_3,
                             a_multicast_frames_xmitted_ok,
                             eth_802_3_cntrs_grp_data_layout,
                             a_multicast_frames_xmitted_ok);
        GET_PPCNT_COUNTER_64(ieee_802_dot_3,
                             a_broadcast_frames_xmitted_ok,
                             eth_802_3_cntrs_grp_data_layout,
                             a_broadcast_frames_xmitted_ok);
        GET_PPCNT_COUNTER_64(ieee_802_dot_3,
                             a_multicast_frames_received_ok,
                             eth_802_3_cntrs_grp_data_layout,
                             a_multicast_frames_received_ok);
        GET_PPCNT_COUNTER_64(ieee_802_dot_3,
                             a_broadcast_frames_recieved_ok,
                             eth_802_3_cntrs_grp_data_layout,
                             a_broadcast_frames_received_ok);
        GET_PPCNT_COUNTER_64(ieee_802_dot_3,
                             a_in_range_length_errors,
                             eth_802_3_cntrs_grp_data_layout,
                             a_in_range_length_errors);
        GET_PPCNT_COUNTER_64(ieee_802_dot_3,
                             a_out_of_range_length_field,
                             eth_802_3_cntrs_grp_data_layout,
                             a_out_of_range_length_field);
        GET_PPCNT_COUNTER_64(ieee_802_dot_3,
                             a_frame_too_long_errors,
                             eth_802_3_cntrs_grp_data_layout,
                             a_frame_too_long_errors);
        GET_PPCNT_COUNTER_64(ieee_802_dot_3,
                             a_symbol_error_during_carrier,
                             eth_802_3_cntrs_grp_data_layout,
                             a_symbol_error_during_carrier);
        GET_PPCNT_COUNTER_64(ieee_802_dot_3,
                             a_mac_control_frames_transmitted,
                             eth_802_3_cntrs_grp_data_layout,
                             a_mac_control_frames_transmitted);
        GET_PPCNT_COUNTER_64(ieee_802_dot_3,
                             a_mac_control_frames_received,
                             eth_802_3_cntrs_grp_data_layout,
                             a_mac_control_frames_received);
        GET_PPCNT_COUNTER_64(ieee_802_dot_3,
                             a_unsupported_opcodes_received,
                             eth_802_3_cntrs_grp_data_layout,
                             a_unsupported_opcodes_received);
        GET_PPCNT_COUNTER_64(ieee_802_dot_3,
                             a_pause_mac_ctrl_frames_received,
                             eth_802_3_cntrs_grp_data_layout,
                             a_pause_mac_ctrl_frames_received);
        GET_PPCNT_COUNTER_64(ieee_802_dot_3,
                             a_pause_mac_ctrl_frames_transmitted,
                             eth_802_3_cntrs_grp_data_layout,
                             a_pause_mac_ctrl_frames_transmitted);
        break;

    case SXD_PPCNT_GRP_RFC_2863_COUNTERS_E:
        CHECK_PPCNT_GRP_AND_COUNT(SXD_BULK_CNTR_PORT_GRP_RFC_2863_E);
        GET_PPCNT_COUNTER_64(rfc_2863, if_in_octets, eth_2863_cntrs_grp_data_layout, if_in_octets);
        GET_PPCNT_COUNTER_64(rfc_2863, if_in_ucast_pkts, eth_2863_cntrs_grp_data_layout, if_in_ucast_pkts);
        GET_PPCNT_COUNTER_64(rfc_2863, if_in_discards, eth_2863_cntrs_grp_data_layout, if_in_discards);
        GET_PPCNT_COUNTER_64(rfc_2863, if_in_errors, eth_2863_cntrs_grp_data_layout, if_in_errors);
        GET_PPCNT_COUNTER_64(rfc_2863, if_in_unknown_protos, eth_2863_cntrs_grp_data_layout, if_in_unknown_protos);
        GET_PPCNT_COUNTER_64(rfc_2863, if_out_octets, eth_2863_cntrs_grp_data_layout, if_out_octets);
        GET_PPCNT_COUNTER_64(rfc_2863, if_out_ucast_pkts, eth_2863_cntrs_grp_data_layout, if_out_ucast_pkts);
        GET_PPCNT_COUNTER_64(rfc_2863, if_out_discards, eth_2863_cntrs_grp_data_layout, if_out_discards);
        GET_PPCNT_COUNTER_64(rfc_2863, if_out_errors, eth_2863_cntrs_grp_data_layout, if_out_errors);
        GET_PPCNT_COUNTER_64(rfc_2863, if_in_multicast_pkts, eth_2863_cntrs_grp_data_layout, if_in_multicast_pkts);
        GET_PPCNT_COUNTER_64(rfc_2863, if_in_broadcast_pkts, eth_2863_cntrs_grp_data_layout, if_in_broadcast_pkts);
        GET_PPCNT_COUNTER_64(rfc_2863, if_out_multicast_pkts, eth_2863_cntrs_grp_data_layout, if_out_multicast_pkts);
        GET_PPCNT_COUNTER_64(rfc_2863, if_out_broadcast_pkts, eth_2863_cntrs_grp_data_layout, if_out_broadcast_pkts);
        break;

    case SXD_PPCNT_GRP_RFC_2819_COUNTERS_E:
        CHECK_PPCNT_GRP_AND_COUNT(SXD_BULK_CNTR_PORT_GRP_RFC_2819_E);
        GET_PPCNT_COUNTER_64(rfc_2819, ether_stats_drop_events, eth_2819_cntrs_grp_data_layout,
                             ether_stats_drop_events);
        GET_PPCNT_COUNTER_64(rfc_2819, ether_stats_octets, eth_2819_cntrs_grp_data_layout, ether_stats_octets);
        GET_PPCNT_COUNTER_64(rfc_2819, ether_stats_pkts, eth_2819_cntrs_grp_data_layout, ether_stats_pkts);
        GET_PPCNT_COUNTER_64(rfc_2819,
                             ether_stats_broadcast_pkts,
                             eth_2819_cntrs_grp_data_layout,
                             ether_stats_broadcast_pkts);
        GET_PPCNT_COUNTER_64(rfc_2819,
                             ether_stats_multicast_pkts,
                             eth_2819_cntrs_grp_data_layout,
                             ether_stats_multicast_pkts);
        GET_PPCNT_COUNTER_64(rfc_2819,
                             ether_stats_crc_align_errors,
                             eth_2819_cntrs_grp_data_layout,
                             ether_stats_crc_align_errors);
        GET_PPCNT_COUNTER_64(rfc_2819,
                             ether_stats_undersize_pkts,
                             eth_2819_cntrs_grp_data_layout,
                             ether_stats_undersize_pkts);
        GET_PPCNT_COUNTER_64(rfc_2819,
                             ether_stats_oversize_pkts,
                             eth_2819_cntrs_grp_data_layout,
                             ether_stats_oversize_pkts);
        GET_PPCNT_COUNTER_64(rfc_2819, ether_stats_fragments, eth_2819_cntrs_grp_data_layout, ether_stats_fragments);
        GET_PPCNT_COUNTER_64(rfc_2819, ether_stats_jabbers, eth_2819_cntrs_grp_data_layout, ether_stats_jabbers);
        GET_PPCNT_COUNTER_64(rfc_2819, ether_stats_collisions, eth_2819_cntrs_grp_data_layout, ether_stats_collisions);
        GET_PPCNT_COUNTER_64(rfc_2819,
                             ether_stats_pkts64octets,
                             eth_2819_cntrs_grp_data_layout,
                             ether_stats_pkts64octets);
        GET_PPCNT_COUNTER_64(rfc_2819,
                             ether_stats_pkts65to127octets,
                             eth_2819_cntrs_grp_data_layout,
                             ether_stats_pkts65to127octets);
        GET_PPCNT_COUNTER_64(rfc_2819,
                             ether_stats_pkts128to255octets,
                             eth_2819_cntrs_grp_data_layout,
                             ether_stats_pkts128to255octets);
        GET_PPCNT_COUNTER_64(rfc_2819,
                             ether_stats_pkts256to511octets,
                             eth_2819_cntrs_grp_data_layout,
                             ether_stats_pkts256to511octets);
        GET_PPCNT_COUNTER_64(rfc_2819,
                             ether_stats_pkts512to1023octets,
                             eth_2819_cntrs_grp_data_layout,
                             ether_stats_pkts512to1023octets);
        GET_PPCNT_COUNTER_64(rfc_2819,
                             ether_stats_pkts1024to1518octets,
                             eth_2819_cntrs_grp_data_layout,
                             ether_stats_pkts1024to1518octets);
        GET_PPCNT_COUNTER_64(rfc_2819,
                             ether_stats_pkts1519to2047octets,
                             eth_2819_cntrs_grp_data_layout,
                             ether_stats_pkts1519to2047octets);
        GET_PPCNT_COUNTER_64(rfc_2819,
                             ether_stats_pkts2048to4095octets,
                             eth_2819_cntrs_grp_data_layout,
                             ether_stats_pkts2048to4095octets);
        GET_PPCNT_COUNTER_64(rfc_2819,
                             ether_stats_pkts4096to8191octets,
                             eth_2819_cntrs_grp_data_layout,
                             ether_stats_pkts4096to8191octets);
        GET_PPCNT_COUNTER_64(rfc_2819,
                             ether_stats_pkts8192to10239octets,
                             eth_2819_cntrs_grp_data_layout,
                             ether_stats_pkts8192to10239octets);
        break;

    case SXD_PPCNT_GRP_RFC_3635_COUNTERS_E:
        CHECK_PPCNT_GRP_AND_COUNT(SXD_BULK_CNTR_PORT_GRP_RFC_3635_E);
        GET_PPCNT_COUNTER_64(rfc_3635,
                             dot3stats_alignment_errors,
                             eth_3635_cntrs_grp_data_layout,
                             dot3stats_alignment_errors);
        GET_PPCNT_COUNTER_64(rfc_3635, dot3stats_fcs_errors, eth_3635_cntrs_grp_data_layout, dot3stats_fcs_errors);
        GET_PPCNT_COUNTER_64(rfc_3635,
                             dot3stats_single_collision_frames,
                             eth_3635_cntrs_grp_data_layout,
                             dot3stats_single_collision_frames);
        GET_PPCNT_COUNTER_64(rfc_3635,
                             dot3stats_multiple_collision_frames,
                             eth_3635_cntrs_grp_data_layout,
                             dot3stats_multiple_collision_frames);
        GET_PPCNT_COUNTER_64(rfc_3635,
                             dot3stats_sqe_test_errors,
                             eth_3635_cntrs_grp_data_layout,
                             dot3stats_sqe_test_errors);
        GET_PPCNT_COUNTER_64(rfc_3635,
                             dot3stats_deferred_transmissions,
                             eth_3635_cntrs_grp_data_layout,
                             dot3stats_deferred_transmissions);
        GET_PPCNT_COUNTER_64(rfc_3635,
                             dot3stats_late_collisions,
                             eth_3635_cntrs_grp_data_layout,
                             dot3stats_late_collisions);
        GET_PPCNT_COUNTER_64(rfc_3635,
                             dot3stats_excessive_collisions,
                             eth_3635_cntrs_grp_data_layout,
                             dot3stats_excessive_collisions);
        GET_PPCNT_COUNTER_64(rfc_3635,
                             dot3stats_internal_mac_transmit_errors,
                             eth_3635_cntrs_grp_data_layout,
                             dot3stats_internal_mac_transmit_errors);
        GET_PPCNT_COUNTER_64(rfc_3635,
                             dot3stats_carrier_sense_errors,
                             eth_3635_cntrs_grp_data_layout,
                             dot3stats_carrier_sense_errors);
        GET_PPCNT_COUNTER_64(rfc_3635,
                             dot3stats_frame_too_longs,
                             eth_3635_cntrs_grp_data_layout,
                             dot3stats_frame_too_longs);
        GET_PPCNT_COUNTER_64(rfc_3635,
                             dot3stats_internal_mac_receive_errors,
                             eth_3635_cntrs_grp_data_layout,
                             dot3stats_internal_mac_receive_errors);
        GET_PPCNT_COUNTER_64(rfc_3635, dot3stats_symbol_errors, eth_3635_cntrs_grp_data_layout,
                             dot3stats_symbol_errors);
        GET_PPCNT_COUNTER_64(rfc_3635,
                             dot3control_in_unknown_opcodes,
                             eth_3635_cntrs_grp_data_layout,
                             dot3control_in_unknown_opcodes);
        GET_PPCNT_COUNTER_64(rfc_3635, dot3in_pause_frames, eth_3635_cntrs_grp_data_layout, dot3in_pause_frames);
        GET_PPCNT_COUNTER_64(rfc_3635, dot3out_pause_frames, eth_3635_cntrs_grp_data_layout, dot3out_pause_frames);
        break;

    case SXD_PPCNT_GRP_PER_PRIORITY_COUNTERS_E:
        if (ppcnt.prio_tc > SXD_PORT_PRIO_ID_MAX) {
            printk(KERN_ERR "Bulk-Cntr PPCNT parser - invalid priority %u\n", ppcnt.prio_tc);
            return;
        }
        is_per_prio_marked =
            SXD_BULK_CNTR_PORT_BITMAP_GET_PRIO(layout_port->mappings.counter_set_bitmap, ppcnt.prio_tc);
        is_per_tc_marked = (ppcnt.prio_tc <= SXD_PORT_TC_ID_MAX) && SXD_BULK_CNTR_PORT_BITMAP_GET_TC(
            layout_port->mappings.counter_set_bitmap,
            ppcnt.prio_tc);
        is_per_pg_marked = (ppcnt.prio_tc < SXD_PRIORITY_GROUP_NUM) && SXD_BULK_CNTR_PORT_BITMAP_GET_PG(
            layout_port->mappings.counter_set_bitmap,
            ppcnt.prio_tc);
        if ((!is_per_prio_marked) && (!is_per_tc_marked) && (!is_per_pg_marked)) {
            printk(KERN_ERR "Bulk-Cntr PPCNT parser - unexpected priority %u\n", ppcnt.prio_tc);
            return;
        }
        CHECK_RECEIVED_PPCNT_COUNT();

        memset(&cntr_prio_buff, 0, sizeof(cntr_prio_buff));
        err = bulk_cntr_db_per_prio_cache_entry_get(ppcnt.local_port, ppcnt.prio_tc, &cntr_prio_cache);
        if (err) {
            printk(
                KERN_ERR "Bulk-Cntr PPCNT parser - failed to get the per prio cache entry for local_port %u, prio %u\n",
                ppcnt.local_port,
                ppcnt.prio_tc);
            return;
        }

        if (is_per_prio_marked) {
            GET_PPCNT_COUNTER_64_PER_PRIO_WITH_CACHE(prio, tx_pause, eth_per_prio_grp_data_layout, tx_pause, tx_pause);
            GET_PPCNT_COUNTER_64_PER_PRIO_WITH_CACHE(prio, rx_pause, eth_per_prio_grp_data_layout, rx_pause, rx_pause);
            GET_PPCNT_COUNTER_64_PER_PRIO_WITH_CACHE(prio,
                                                     tx_pause_duration,
                                                     eth_per_prio_grp_data_layout,
                                                     tx_pause_duration,
                                                     tx_pause_duration);
            GET_PPCNT_COUNTER_64_PER_PRIO_WITH_CACHE(prio,
                                                     rx_pause_duration,
                                                     eth_per_prio_grp_data_layout,
                                                     rx_pause_duration,
                                                     rx_pause_duration);
            if (ppcnt.clr) {
                cntr_prio_buff.tx_pause = 0;
                cntr_prio_buff.rx_pause = 0;
                cntr_prio_buff.tx_pause_duration = 0;
                cntr_prio_buff.rx_pause_duration = 0;
            }
        }

        if (is_per_tc_marked) {
            GET_PPCNT_COUNTER_64_PER_PRIO_WITH_CACHE(tc, tx_octet, eth_per_prio_grp_data_layout, tx_octets, tx_octets);
            GET_PPCNT_COUNTER_64_PER_PRIO_WITH_CACHE(tc, tx_frames, eth_per_prio_grp_data_layout, tx_frames,
                                                     tx_frames);
            if (ppcnt.clr) {
                cntr_prio_buff.tx_octets = 0;
                cntr_prio_buff.tx_frames = 0;
            }
        }

        if (is_per_pg_marked) {
            GET_PPCNT_COUNTER_64_PER_PRIO_WITH_CACHE(pg, rx_octet, eth_per_prio_grp_data_layout, rx_octets, rx_octets);
            GET_PPCNT_COUNTER_64_PER_PRIO_WITH_CACHE(pg, rx_frames, eth_per_prio_grp_data_layout, rx_frames,
                                                     rx_frames);
            GET_PPCNT_COUNTER_64_PER_PRIO_WITH_CACHE(pg,
                                                     rx_buffer_discard,
                                                     eth_per_prio_grp_data_layout,
                                                     rx_discards,
                                                     rx_discard);
            if (ppcnt.clr) {
                cntr_prio_buff.rx_octets = 0;
                cntr_prio_buff.rx_frames = 0;
                cntr_prio_buff.rx_discard = 0;
            }
        }
        if (ppcnt.clr) {
            memcpy(cntr_prio_cache, &cntr_prio_buff, sizeof(sxd_port_cntr_prio_t));
        }
        break;

    case SXD_PPCNT_GRP_PER_TRAFFIC_CLASS_COUNTERS_E:
        CHECK_PPCNT_TC_GRP_AND_COUNT();
        GET_PPCNT_COUNTER_64_PER_PRIO(tc, tx_queue, eth_per_traffic_class_layout, transmit_queue);
        GET_PPCNT_COUNTER_64_PER_PRIO(tc, tx_no_buffer_discard_uc, eth_per_traffic_class_layout, no_buffer_discard_uc);
        break;

    case SXD_PPCNT_GRP_PER_TRAFFIC_CLASS_CONGESTION_COUNTERS_E:
        CHECK_PPCNT_TC_GRP_AND_COUNT();
        GET_PPCNT_COUNTER_64_PER_PRIO(tc, tx_wred_discard, eth_per_traffic_class_cong_layout, wred_discard);
        break;

    case SXD_PPCNT_GRP_ETHERNET_EXTENDED_COUNTERS_E:
        CHECK_PPCNT_GRP_AND_COUNT(SXD_BULK_CNTR_PORT_GRP_PERF_E);
        GET_PPCNT_COUNTER_64(perf, tx_wait, eth_extended_cntrs_grp_data_layout, port_transmit_wait);
        GET_PPCNT_COUNTER_64(perf, ecn_marked, eth_extended_cntrs_grp_data_layout, ecn_marked);
        GET_PPCNT_COUNTER_64(perf, no_buffer_discard_mc, eth_extended_cntrs_grp_data_layout, no_buffer_discard_mc);
        GET_PPCNT_COUNTER_64(perf, rx_ebp, eth_extended_cntrs_grp_data_layout, rx_ebp);
        GET_PPCNT_COUNTER_64(perf, tx_ebp, eth_extended_cntrs_grp_data_layout, tx_ebp);
        GET_PPCNT_COUNTER_64(perf, rx_buffer_almost_full, eth_extended_cntrs_grp_data_layout, rx_buffer_almost_full);
        GET_PPCNT_COUNTER_64(perf, rx_buffer_full, eth_extended_cntrs_grp_data_layout, rx_buffer_full);
        GET_PPCNT_COUNTER_64(perf, tx_stats_pkts64octets, eth_extended_cntrs_grp_data_layout, tx_stats_pkts64octets);
        GET_PPCNT_COUNTER_64(perf,
                             tx_stats_pkts65to127octets,
                             eth_extended_cntrs_grp_data_layout,
                             tx_stats_pkts65to127octets);
        GET_PPCNT_COUNTER_64(perf,
                             tx_stats_pkts128to255octets,
                             eth_extended_cntrs_grp_data_layout,
                             tx_stats_pkts128to255octets);
        GET_PPCNT_COUNTER_64(perf,
                             tx_stats_pkts256to511octets,
                             eth_extended_cntrs_grp_data_layout,
                             tx_stats_pkts256to511octets);
        GET_PPCNT_COUNTER_64(perf,
                             tx_stats_pkts512to1023octets,
                             eth_extended_cntrs_grp_data_layout,
                             tx_stats_pkts512to1023octets);
        GET_PPCNT_COUNTER_64(perf,
                             tx_stats_pkts1024to1518octets,
                             eth_extended_cntrs_grp_data_layout,
                             tx_stats_pkts1024to1518octets);
        GET_PPCNT_COUNTER_64(perf,
                             tx_stats_pkts1519to2047octets,
                             eth_extended_cntrs_grp_data_layout,
                             tx_stats_pkts1519to2047octets);
        GET_PPCNT_COUNTER_64(perf,
                             tx_stats_pkts2048to4095octets,
                             eth_extended_cntrs_grp_data_layout,
                             tx_stats_pkts2048to4095octets);
        GET_PPCNT_COUNTER_64(perf,
                             tx_stats_pkts4096to8191octets,
                             eth_extended_cntrs_grp_data_layout,
                             tx_stats_pkts4096to8191octets);
        GET_PPCNT_COUNTER_64(perf,
                             tx_stats_pkts8192to10239octets,
                             eth_extended_cntrs_grp_data_layout,
                             tx_stats_pkts8192to10239octets);
        break;

    case SXD_PPCNT_GRP_ETHERNET_DISCARD_COUNTERS_E:
        CHECK_PPCNT_GRP_AND_COUNT(SXD_BULK_CNTR_PORT_GRP_DISCARD_E);
        GET_PPCNT_COUNTER_64(discard, ingress_general, eth_discard_cntrs_grp, ingress_general);
        GET_PPCNT_COUNTER_64(discard, ingress_policy_engine, eth_discard_cntrs_grp, ingress_policy_engine);
        GET_PPCNT_COUNTER_64(discard, ingress_vlan_membership, eth_discard_cntrs_grp, ingress_vlan_membership);
        GET_PPCNT_COUNTER_64(discard, ingress_tag_frame_type, eth_discard_cntrs_grp, ingress_tag_frame_type);
        GET_PPCNT_COUNTER_64(discard, egress_vlan_membership, eth_discard_cntrs_grp, egress_vlan_membership);
        GET_PPCNT_COUNTER_64(discard, loopback_filter, eth_discard_cntrs_grp, loopback_filter);
        GET_PPCNT_COUNTER_64(discard, egress_general, eth_discard_cntrs_grp, egress_general);
        GET_PPCNT_COUNTER_64(discard, egress_hoq, eth_discard_cntrs_grp, egress_hoq);
        GET_PPCNT_COUNTER_64(discard, port_isolation, eth_discard_cntrs_grp, port_isolation);
        GET_PPCNT_COUNTER_64(discard, egress_policy_engine, eth_discard_cntrs_grp, egress_policy_engine);
        GET_PPCNT_COUNTER_64(discard, ingress_tx_link_down, eth_discard_cntrs_grp, ingress_tx_link_down);
        GET_PPCNT_COUNTER_64(discard, egress_stp_filter, eth_discard_cntrs_grp, egress_stp_filter);
        GET_PPCNT_COUNTER_64(discard, egress_hoq_stall, eth_discard_cntrs_grp, egress_hoq_stall);
        GET_PPCNT_COUNTER_64(discard, egress_sll, eth_discard_cntrs_grp, egress_sll);
        GET_PPCNT_COUNTER_64(discard, ingress_discard_all, eth_discard_cntrs_grp, ingress_discard_all);
        break;

    case SXD_PPCNT_GRP_PHYSICAL_LAYER_COUNTERS_E:
        CHECK_PPCNT_GRP_AND_COUNT(SXD_BULK_CNTR_PORT_GRP_PHY_LAYER_E);
        GET_PPCNT_COUNTER_64(phy_layer, time_since_last_clear, phys_layer_cntrs, time_since_last_clear);
        GET_PPCNT_COUNTER_64(phy_layer, symbol_errors, phys_layer_cntrs, symbol_errors);
        GET_PPCNT_COUNTER_64(phy_layer, sync_headers_errors, phys_layer_cntrs, sync_headers_errors);
        GET_PPCNT_COUNTER_64(phy_layer, edpl_bip_errors_lane0, phys_layer_cntrs, edpl_bip_errors_lane0);
        GET_PPCNT_COUNTER_64(phy_layer, edpl_bip_errors_lane1, phys_layer_cntrs, edpl_bip_errors_lane1);
        GET_PPCNT_COUNTER_64(phy_layer, edpl_bip_errors_lane2, phys_layer_cntrs, edpl_bip_errors_lane2);
        GET_PPCNT_COUNTER_64(phy_layer, edpl_bip_errors_lane3, phys_layer_cntrs, edpl_bip_errors_lane3);
        GET_PPCNT_COUNTER_64(phy_layer, fc_fec_corrected_blocks_lane0, phys_layer_cntrs,
                             fc_fec_corrected_blocks_lane0);
        GET_PPCNT_COUNTER_64(phy_layer, fc_fec_corrected_blocks_lane1, phys_layer_cntrs,
                             fc_fec_corrected_blocks_lane1);
        GET_PPCNT_COUNTER_64(phy_layer, fc_fec_corrected_blocks_lane2, phys_layer_cntrs,
                             fc_fec_corrected_blocks_lane2);
        GET_PPCNT_COUNTER_64(phy_layer, fc_fec_corrected_blocks_lane3, phys_layer_cntrs,
                             fc_fec_corrected_blocks_lane3);
        GET_PPCNT_COUNTER_64(phy_layer,
                             fc_fec_uncorrectable_blocks_lane0,
                             phys_layer_cntrs,
                             fc_fec_uncorrectable_blocks_lane0);
        GET_PPCNT_COUNTER_64(phy_layer,
                             fc_fec_uncorrectable_blocks_lane1,
                             phys_layer_cntrs,
                             fc_fec_uncorrectable_blocks_lane1);
        GET_PPCNT_COUNTER_64(phy_layer,
                             fc_fec_uncorrectable_blocks_lane2,
                             phys_layer_cntrs,
                             fc_fec_uncorrectable_blocks_lane2);
        GET_PPCNT_COUNTER_64(phy_layer,
                             fc_fec_uncorrectable_blocks_lane3,
                             phys_layer_cntrs,
                             fc_fec_uncorrectable_blocks_lane3);
        GET_PPCNT_COUNTER_64(phy_layer, rs_fec_corrected_blocks, phys_layer_cntrs, rs_fec_corrected_blocks);
        GET_PPCNT_COUNTER_64(phy_layer, rs_fec_uncorrectable_blocks, phys_layer_cntrs, rs_fec_uncorrectable_blocks);
        GET_PPCNT_COUNTER_64(phy_layer, rs_fec_no_errors_blocks, phys_layer_cntrs, rs_fec_no_errors_blocks);
        GET_PPCNT_COUNTER_64(phy_layer, rs_fec_single_error_blocks, phys_layer_cntrs, rs_fec_single_error_blocks);
        GET_PPCNT_COUNTER_64(phy_layer,
                             rs_fec_corrected_symbols_total,
                             phys_layer_cntrs,
                             rs_fec_corrected_symbols_total);
        GET_PPCNT_COUNTER_64(phy_layer,
                             rs_fec_corrected_symbols_lane0,
                             phys_layer_cntrs,
                             rs_fec_corrected_symbols_lane0);
        GET_PPCNT_COUNTER_64(phy_layer,
                             rs_fec_corrected_symbols_lane1,
                             phys_layer_cntrs,
                             rs_fec_corrected_symbols_lane1);
        GET_PPCNT_COUNTER_64(phy_layer,
                             rs_fec_corrected_symbols_lane2,
                             phys_layer_cntrs,
                             rs_fec_corrected_symbols_lane2);
        GET_PPCNT_COUNTER_64(phy_layer,
                             rs_fec_corrected_symbols_lane3,
                             phys_layer_cntrs,
                             rs_fec_corrected_symbols_lane3);
        GET_PPCNT_COUNTER_32(phy_layer, link_down_events, phys_layer_cntrs, link_down_events);
        GET_PPCNT_COUNTER_32(phy_layer, successful_recovery_events, phys_layer_cntrs, successful_recovery_events);
        break;

    case SXD_PPCNT_GRP_PHYSICAL_LAYER_STATISTICAL_COUNTERS_E:
        CHECK_PPCNT_GRP_AND_COUNT(SXD_BULK_CNTR_PORT_GRP_PHY_LAYER_STATS_E);
        GET_PPCNT_COUNTER_64(phy_layer_stats, time_since_last_clear, phys_layer_stat_cntrs, time_since_last_clear);
        GET_PPCNT_COUNTER_64(phy_layer_stats, phy_received_bits, phys_layer_stat_cntrs, phy_received_bits);
        GET_PPCNT_COUNTER_64(phy_layer_stats, phy_symbol_errors, phys_layer_stat_cntrs, phy_symbol_errors);
        GET_PPCNT_COUNTER_64(phy_layer_stats, phy_corrected_bits, phys_layer_stat_cntrs, phy_corrected_bits);
        GET_PPCNT_COUNTER_64(phy_layer_stats, phy_raw_errors_lane0, phys_layer_stat_cntrs, phy_raw_errors_lane0);
        GET_PPCNT_COUNTER_64(phy_layer_stats, phy_raw_errors_lane1, phys_layer_stat_cntrs, phy_raw_errors_lane1);
        GET_PPCNT_COUNTER_64(phy_layer_stats, phy_raw_errors_lane2, phys_layer_stat_cntrs, phy_raw_errors_lane2);
        GET_PPCNT_COUNTER_64(phy_layer_stats, phy_raw_errors_lane3, phys_layer_stat_cntrs, phy_raw_errors_lane3);
        GET_PPCNT_COUNTER_64(phy_layer_stats, phy_raw_errors_lane4, phys_layer_stat_cntrs, phy_raw_errors_lane4);
        GET_PPCNT_COUNTER_64(phy_layer_stats, phy_raw_errors_lane5, phys_layer_stat_cntrs, phy_raw_errors_lane5);
        GET_PPCNT_COUNTER_64(phy_layer_stats, phy_raw_errors_lane6, phys_layer_stat_cntrs, phy_raw_errors_lane6);
        GET_PPCNT_COUNTER_64(phy_layer_stats, phy_raw_errors_lane7, phys_layer_stat_cntrs, phy_raw_errors_lane7);
        GET_PPCNT_COUNTER_32(phy_layer_stats, raw_ber_magnitude, phys_layer_stat_cntrs, raw_ber_magnitude);
        GET_PPCNT_COUNTER_32(phy_layer_stats, raw_ber_coef, phys_layer_stat_cntrs, raw_ber_coef);
        GET_PPCNT_COUNTER_32(phy_layer_stats, effective_ber_magnitude, phys_layer_stat_cntrs, effective_ber_magnitude);
        GET_PPCNT_COUNTER_32(phy_layer_stats, effective_ber_coef, phys_layer_stat_cntrs, effective_ber_coef);
        break;

    default:
        printk(KERN_ERR "Bulk-Cntr PPCNT parser - unexpected PPCNT group %u\n", ppcnt.grp);
        return;
    }

    layout_common->counters_received_so_far++;
}


void sx_bulk_cntr_handle_ppcnt(struct completion_info *ci)
{
    __sx_bulk_cntr_multi_emad_parse(ci->skb->data, ci->skb->len, __ppcnt_parser, NULL);
}


/***************************************************************************************
 * MGPCB
 *
 *       31                       24                       16                       8                        0
 *       +------------------------+------------------------+------------------------+------------------------+
 * 000h  |                        |                            counter_index_base                            |
 *       +------------------------+------------------------+------------------------+------------------------+
 * 004h  |                                                                          |         num_rec        |
 *       +------------------------+------------------------+------------------------+------------+-----------+
 * 008h  |                                                                                       |  opcode   |
 *       +------------------------+------------------------+------------------------+------------+-----------+
 * 00ch  |                                                                                                   |
 *       +---------------------------------------------------------------------------------------------------+
 * 010h  |                                        record[0].byte_counter                                     |
 * 014h  |                                                                                                   |
 *       +---------------------------------------------------------------------------------------------------+
 * 018h  |                                        record[0].packet_counter                                   |
 * 01ch  |                                                                                                   |
 *       +---------------------------------------------------------------------------------------------------+
 *       .
 *       .
 *       .
 *       +---------------------------------------------------------------------------------------------------+
 * 400h  |                                        record[63].byte_counter                                    |
 * 404h  |                                                                                                   |
 *       +---------------------------------------------------------------------------------------------------+
 * 408h  |                                        record[63].packet_counter                                  |
 * 40ch  |                                                                                                   |
 *       +---------------------------------------------------------------------------------------------------+
 *
 */

struct mgpcb_record {
    u64 byte_counter;
    u64 packet_counter;
};
struct mgpcb_reg {
    u32                 counter_index_base;
    u32                 num_rec;
    u32                 opcode;
    u32                 reserved;
    struct mgpcb_record records[0];
};
static void __mgpcb_parser(const void                                *mgpcb_reg,
                           u32                                        reg_len,
                           struct sxd_bulk_cntr_buffer_layout_common *layout_common,
                           void                                      *context)
{
    const struct mgpcb_reg                  *mgpcb = (struct mgpcb_reg*)mgpcb_reg;
    struct sxd_bulk_cntr_buffer_layout_flow *layout_flow = (struct sxd_bulk_cntr_buffer_layout_flow*)layout_common;
    u32                                      num_rec = be32_to_cpu(mgpcb->num_rec) & 0xff;
    u32                                      expected_reg_len = sizeof(struct mgpcb_reg) + num_rec *
                                                                sizeof(struct mgpcb_record);
    u32 counter_base_index;
    u32 i, index_to_buff;

    if (reg_len < expected_reg_len) {
        printk(KERN_ERR "MGPCB parse: reg_len is less than expected len (reg_len=%u, expected=%u)\n",
               reg_len, expected_reg_len);
        return;
    }

    counter_base_index = be32_to_cpu(mgpcb->counter_index_base) & 0xffffff;
    index_to_buff = (counter_base_index - layout_flow->base_counter_id) / 2; /* counter indexes increment by 2 */

    for (i = 0; i < num_rec; i++) {
        layout_flow->counters[index_to_buff + i].flow_counter_bytes = be64_to_cpu(mgpcb->records[i].byte_counter);
        layout_flow->counters[index_to_buff + i].flow_counter_packets = be64_to_cpu(mgpcb->records[i].packet_counter);
        if (layout_common->counters_received_so_far == layout_common->num_of_counters) {
            printk(KERN_ERR "MGPCB parse: got more counters than required\n");
            break;
        }

        layout_common->counters_received_so_far++;
    }
}

void sx_bulk_cntr_handle_mgpcb(struct completion_info *ci)
{
    __sx_bulk_cntr_multi_emad_parse(ci->skb->data, ci->skb->len, __mgpcb_parser, NULL);
}


/***************************************************************************************
 * PBSR
 *
 *       31                       24                       16                       8                        0
 *       +------------------------+------------------------+------------------------+------------------------+
 * 000h  |                        |    local_port          |                        |                   |type|
 *       +------------------------+------------------------+------------------------+------------------------+
 * 004h  |                                                                                                   |
 *       +------------------------+------------------------+------------------------+------------+-----------+
 * 008h  |                                                 |       used_shared_headroom_buffer               |
 *       +------------------------+------------------------+------------------------+------------+-----------+
 * 00ch  |                                        stat_buffer[0]                                             |
 * 010h  |                                                                                                   |
 *       +---------------------------------------------------------------------------------------------------+
 * 018h  |                                        stat_buffer[1]                                             |
 * 01ch  |                                                                                                   |
 *       +---------------------------------------------------------------------------------------------------+
 *       .
 *       .
 *       .
 *       +---------------------------------------------------------------------------------------------------+
 * 054h  |                                        stat_buffer[9]                                             |
 * 058h  |                                                                                                   |
 *       +---------------------------------------------------------------------------------------------------+
 * 05ch  |                                        shared_buffer_status                                       |
 * 060h  |                                                                                                   |
 *       +---------------------------------------------------------------------------------------------------+
 *
 */

static void __calculate_headroom(struct sxd_bulk_cntr_buffer_layout_headroom *layout_headroom,
                                 const struct ku_pbsr_reg                    *pbsr,
                                 u8                                           port_idx,
                                 u8                                           is_8xlane,
                                 u8                                           is_slave)
{
    u8 i = 0, cnt = 0, idx = 0;

    if (is_8xlane) {
        cnt = 2;
    } else {
        cnt = 1;
    }

    if (is_slave) {
        idx = 1;
    } else {
        layout_headroom->headroom[port_idx].shared_headroom_usage.curr_occupancy = be16_to_cpu(
            pbsr->used_shared_headroom_buffer);
        layout_headroom->headroom[port_idx].shared_headroom_usage.watermark = 0;
        idx = 0;
    }

    layout_headroom->headroom[port_idx].shared.statistics.curr_occupancy +=
        pbsr->stat_shared_headroom_pool.used_buffer;
    layout_headroom->headroom[port_idx].shared.statistics.watermark +=
        pbsr->stat_shared_headroom_pool.watermark;
    layout_headroom->headroom[port_idx].shared.occupancy_statistics_lst.cnt = cnt;
    layout_headroom->headroom[port_idx].shared.occupancy_statistics_lst.statistics[idx].curr_occupancy =
        pbsr->stat_shared_headroom_pool.used_buffer;
    layout_headroom->headroom[port_idx].shared.occupancy_statistics_lst.statistics[idx].watermark =
        pbsr->stat_shared_headroom_pool.watermark;

    for (i = 0; i < SXD_BULK_CNTR_PG_NUM; i++) {
        layout_headroom->headroom[port_idx].port_pg[i].statistics.curr_occupancy +=
            pbsr->stat_buffer[i].used_buffer;
        layout_headroom->headroom[port_idx].port_pg[i].statistics.watermark +=
            pbsr->stat_buffer[i].watermark;
        layout_headroom->headroom[port_idx].port_pg[i].occupancy_statistics_lst.cnt = cnt;
        layout_headroom->headroom[port_idx].port_pg[i].occupancy_statistics_lst.statistics[idx].curr_occupancy =
            pbsr->stat_buffer[i].used_buffer;
        layout_headroom->headroom[port_idx].port_pg[i].occupancy_statistics_lst.statistics[idx].watermark =
            pbsr->stat_buffer[i].watermark;
    }

    return;
}

static void __pbsr_parser(const void                                *pbsr_reg,
                          u32                                        reg_len,
                          struct sxd_bulk_cntr_buffer_layout_common *layout_common,
                          void                                      *context)
{
    struct ku_pbsr_reg                           pbsr;
    struct sxd_bulk_cntr_buffer_layout_headroom *layout_headroom =
        (struct sxd_bulk_cntr_buffer_layout_headroom*)layout_common;
    int ret = 0;
    u8  buffer_type, local_port, port_idx;

    ret = __PBSR_decode((u8*)pbsr_reg, &pbsr, context);
    if (ret != 0) {
        printk(KERN_ERR "Failed to decode PBSR\n");
        return;
    }

    buffer_type = pbsr.buffer_type;
    local_port = pbsr.local_port;

    port_idx = local_port - 1;

    if (layout_headroom->port_width[port_idx] != 8) {
        __calculate_headroom(layout_headroom, &pbsr, port_idx, 0, 0);
    } else {
        if (buffer_type == SXD_BULK_CNTR_HEADROOM_BUFFER_TYPE_MASTER_E) {
            __calculate_headroom(layout_headroom, &pbsr, port_idx, 1, 0);
        }

        if (buffer_type == SXD_BULK_CNTR_HEADROOM_BUFFER_TYPE_SLAVE_E) {
            __calculate_headroom(layout_headroom, &pbsr, port_idx, 1, 1);
        }
    }

    if (layout_common->counters_received_so_far == layout_common->num_of_counters) {
        printk(KERN_ERR "PBSR parse: got more counters than required\n");
        return;
    }

    layout_common->counters_received_so_far++;

    return;
}

void sx_bulk_cntr_handle_pbsr(struct completion_info *ci)
{
    __sx_bulk_cntr_multi_emad_parse(ci->skb->data, ci->skb->len, __pbsr_parser, NULL);
}


/***************************************************************************************
 * SBSRD
 *
 *       31                       24                       16                       8                        0
 *       +------------------------+------------------------+------------------------+------------------------+
 * 000h  | s|d                                                                      |         type           |
 *       +------------------------+------------------------+------------------------+------------------------+
 * 004h  |                                                                          |         num_rec        |
 *       +------------------------+------------------------+------------------------+------------+-----------+
 * 008h  |            first_entry_index                    |                       |    first_local_port     |
 *       +------------------------+------------------------+------------------------+------------+-----------+
 * 00ch  |                                                                                                   |
 *       +---------------------------------------------------------------------------------------------------+
 * 010h  |                                        shared_buffer_status[0]                                    |
 * 014h  |                                                                                                   |
 *       +---------------------------------------------------------------------------------------------------+
 * 018h  |                                        shared_buffer_status[1]                                    |
 * 01ch  |                                                                                                   |
 *       +---------------------------------------------------------------------------------------------------+
 *       .
 *       .
 *       .
 *       +---------------------------------------------------------------------------------------------------+
 * 408h  |                                        shared_buffer_status[127]                                  |
 * 40ch  |                                                                                                   |
 *       +---------------------------------------------------------------------------------------------------+
 *
 */

static void __sbsrd_parser(const void                                *sbsrd_reg,
                           u32                                        reg_len,
                           struct sxd_bulk_cntr_buffer_layout_common *layout_common,
                           void                                      *context)
{
    struct ku_sbsrd_reg                               sbsrd;
    struct sxd_bulk_cntr_buffer_layout_shared_buffer *layout_shared_buffer =
        (struct sxd_bulk_cntr_buffer_layout_shared_buffer*)layout_common;
    int ret = 0;
    u32 i = 0, idx = 0, sw_pool_id = 0, num_rec = 0;
    u8  type = 0, desc = 0;
    u32 port_idx = layout_shared_buffer->port_idx;
    u8  pg_tc_sp_idx = layout_shared_buffer->pg_tc_sp_idx;
    u8  port_pool_idx = layout_shared_buffer->port_pool_idx;
    u8  pool_idx = layout_shared_buffer->pool_idx;
    u8  last_type = layout_shared_buffer->last_type;

    ret = __SBSRD_decode((u8*)sbsrd_reg, &sbsrd, context);
    if (ret != 0) {
        printk(KERN_ERR "Failed to decode SBSRD\n");
        return;
    }

    type = sbsrd.type;
    desc = sbsrd.desc;
    num_rec = sbsrd.num_rec;

    if (type != last_type) {
        port_idx = 0;
        pg_tc_sp_idx = 0;
        port_pool_idx = 0;
    }

    for (i = 0; i < num_rec; i++) {
        switch (type) {
        case SXD_BULK_CNTR_SHARED_BUFFER_ATTR_TYPE_RX_PG_E:
            if (port_idx == SXD_BULK_CNTR_PORT_NUM) {
                printk(KERN_ERR "SBSRD parse: got too many number of rx_pg record.\n");
                return;
            }

            if (desc == 0) {
                layout_shared_buffer->port[port_idx].rx_pg[pg_tc_sp_idx].curr_occupancy =
                    sbsrd.shared_buffer_status[i].buff_occupancy;
                layout_shared_buffer->port[port_idx].rx_pg[pg_tc_sp_idx].watermark =
                    sbsrd.shared_buffer_status[i].max_buff_occupancy;
            } else {
                layout_shared_buffer->port[port_idx].rx_pg_desc[pg_tc_sp_idx].curr_occupancy =
                    sbsrd.shared_buffer_status[i].buff_occupancy;
                layout_shared_buffer->port[port_idx].rx_pg_desc[pg_tc_sp_idx].watermark =
                    sbsrd.shared_buffer_status[i].max_buff_occupancy;
            }

            pg_tc_sp_idx++;
            if (pg_tc_sp_idx == SXD_BULK_CNTR_PG_NUM) {
                port_idx++;
                pg_tc_sp_idx = 0;
            }

            break;

        case SXD_BULK_CNTR_SHARED_BUFFER_ATTR_TYPE_TX_TC_E:
            if (port_idx == SXD_BULK_CNTR_PORT_NUM) {
                printk(KERN_ERR "SBSRD parse: got too many number of tx_tc record.\n");
                return;
            }

            if (desc == 0) {
                layout_shared_buffer->port[port_idx].tx_tc[pg_tc_sp_idx].curr_occupancy =
                    sbsrd.shared_buffer_status[i].buff_occupancy;
                layout_shared_buffer->port[port_idx].tx_tc[pg_tc_sp_idx].watermark =
                    sbsrd.shared_buffer_status[i].max_buff_occupancy;
            } else {
                layout_shared_buffer->port[port_idx].tx_tc_desc[pg_tc_sp_idx].curr_occupancy =
                    sbsrd.shared_buffer_status[i].buff_occupancy;
                layout_shared_buffer->port[port_idx].tx_tc_desc[pg_tc_sp_idx].watermark =
                    sbsrd.shared_buffer_status[i].max_buff_occupancy;
            }

            pg_tc_sp_idx++;
            if (pg_tc_sp_idx == SXD_BULK_CNTR_TC_NUM) {
                port_idx++;
                pg_tc_sp_idx = 0;
            }

            break;

        case SXD_BULK_CNTR_SHARED_BUFFER_ATTR_TYPE_RX_PER_POOL_E:
            if (port_idx == SXD_BULK_CNTR_PORT_NUM) {
                printk(KERN_ERR "SBSRD parse: got too many number of rx_per_pool record.\n");
                return;
            }
            /* The index is in order by direction (ingress/egress) and then by buffer type (data/descriptor),
             * So the first 16 (pool number) are for data ingress, the second 16 are for data egress,
             * the third one are for descriptor ingress, the fourth are for descriptor egress */
            idx = pg_tc_sp_idx + (SXD_BULK_CNTR_POOL_NUM * 2) * desc;
            sw_pool_id = layout_shared_buffer->sw_pool_id[idx];
            if (sw_pool_id != SXD_BULK_CNTR_POOL_ID_INVALID) {
                if (desc == 0) {
                    layout_shared_buffer->port[port_idx].rx_per_pool[port_pool_idx].sw_pool_id = sw_pool_id;
                    layout_shared_buffer->port[port_idx].rx_per_pool[port_pool_idx].occupancy.curr_occupancy =
                        sbsrd.shared_buffer_status[i].buff_occupancy;
                    layout_shared_buffer->port[port_idx].rx_per_pool[port_pool_idx].occupancy.watermark =
                        sbsrd.shared_buffer_status[i].max_buff_occupancy;
                } else {
                    layout_shared_buffer->port[port_idx].rx_per_pool_desc[port_pool_idx].sw_pool_id = sw_pool_id;
                    layout_shared_buffer->port[port_idx].rx_per_pool_desc[port_pool_idx].occupancy.curr_occupancy =
                        sbsrd.shared_buffer_status[i].buff_occupancy;
                    layout_shared_buffer->port[port_idx].rx_per_pool_desc[port_pool_idx].occupancy.watermark =
                        sbsrd.shared_buffer_status[i].max_buff_occupancy;
                }
                port_pool_idx++;
            }

            pg_tc_sp_idx++;
            if (pg_tc_sp_idx == SXD_BULK_CNTR_POOL_NUM) {
                port_idx++;
                pg_tc_sp_idx = 0;
                port_pool_idx = 0;
            }

            break;

        case SXD_BULK_CNTR_SHARED_BUFFER_ATTR_TYPE_TX_PER_POOL_E:
            if (port_idx == SXD_BULK_CNTR_PORT_NUM) {
                printk(KERN_ERR "SBSRD parse: got too many number of tx_per_pool record.\n");
                return;
            }
            /* The index is in order by direction (ingress/egress) and then by buffer type (data/descriptor),
             * So the first 16 (pool num) are for data ingress, the second 16 are for data egress,
             * the third one are for descriptor ingress, the fourth are for descriptor egress */
            idx = pg_tc_sp_idx + SXD_BULK_CNTR_POOL_NUM + (SXD_BULK_CNTR_POOL_NUM * 2) * desc;
            sw_pool_id = layout_shared_buffer->sw_pool_id[idx];
            if (sw_pool_id != SXD_BULK_CNTR_POOL_ID_INVALID) {
                if (desc == 0) {
                    if (sw_pool_id == layout_shared_buffer->mc_pool_id) {
                        layout_shared_buffer->mc_port[port_idx].occupancy.curr_occupancy =
                            sbsrd.shared_buffer_status[i].buff_occupancy;
                        layout_shared_buffer->mc_port[port_idx].occupancy.watermark =
                            sbsrd.shared_buffer_status[i].max_buff_occupancy;
                    } else {
                        layout_shared_buffer->port[port_idx].tx_per_pool[port_pool_idx].sw_pool_id = sw_pool_id;
                        layout_shared_buffer->port[port_idx].tx_per_pool[port_pool_idx].occupancy.curr_occupancy =
                            sbsrd.shared_buffer_status[i].buff_occupancy;
                        layout_shared_buffer->port[port_idx].tx_per_pool[port_pool_idx].occupancy.watermark =
                            sbsrd.shared_buffer_status[i].max_buff_occupancy;
                    }
                } else {
                    layout_shared_buffer->port[port_idx].tx_per_pool_desc[port_pool_idx].sw_pool_id = sw_pool_id;
                    layout_shared_buffer->port[port_idx].tx_per_pool_desc[port_pool_idx].occupancy.curr_occupancy =
                        sbsrd.shared_buffer_status[i].buff_occupancy;
                    layout_shared_buffer->port[port_idx].tx_per_pool_desc[port_pool_idx].occupancy.watermark =
                        sbsrd.shared_buffer_status[i].max_buff_occupancy;
                }
                port_pool_idx++;
            }

            pg_tc_sp_idx++;
            if (pg_tc_sp_idx == SXD_BULK_CNTR_POOL_NUM) {
                port_idx++;
                pg_tc_sp_idx = 0;
                port_pool_idx = 0;
            }
            break;

        case SXD_BULK_CNTR_SHARED_BUFFER_ATTR_TYPE_MC_SWITH_PRIO_E:
            if (pg_tc_sp_idx == SXD_BULK_CNTR_SP_NUM) {
                printk(KERN_ERR "SBSRD parse: got too many number of mc_switch_prio record.\n");
                return;
            }
            layout_shared_buffer->mc_switch_prio.statistics[pg_tc_sp_idx].curr_occupancy =
                sbsrd.shared_buffer_status[i].buff_occupancy;
            layout_shared_buffer->mc_switch_prio.statistics[pg_tc_sp_idx].watermark =
                sbsrd.shared_buffer_status[i].max_buff_occupancy;
            pg_tc_sp_idx++;
            break;

        case SXD_BULK_CNTR_SHARED_BUFFER_ATTR_TYPE_RX_POOL_E:
            if (pg_tc_sp_idx == SXD_BULK_CNTR_POOL_NUM) {
                printk(KERN_ERR "SBSRD parse: got too many number of tc_pool record.\n");
                return;
            }
            /* The index is in order by direction (ingress/egress) and then by buffer type (data/descriptor),
             * So the first 16 (pool num) are for data ingress, the second 16 are for data egress,
             * the third one are for descriptor ingress, the fourth are for descriptor egress */
            idx = pg_tc_sp_idx + (SXD_BULK_CNTR_POOL_NUM * 2) * desc;
            sw_pool_id = layout_shared_buffer->sw_pool_id[idx];
            if (sw_pool_id != SXD_BULK_CNTR_POOL_ID_INVALID) {
                layout_shared_buffer->pool.statistics[pool_idx].sw_pool_id = sw_pool_id;
                layout_shared_buffer->pool.statistics[pool_idx].occupancy.curr_occupancy =
                    sbsrd.shared_buffer_status[i].buff_occupancy;
                layout_shared_buffer->pool.statistics[pool_idx].occupancy.watermark =
                    sbsrd.shared_buffer_status[i].max_buff_occupancy;
                pool_idx++;
            }
            pg_tc_sp_idx++;
            break;

        case SXD_BULK_CNTR_SHARED_BUFFER_ATTR_TYPE_TX_POOL_E:
            if (pg_tc_sp_idx == SXD_BULK_CNTR_POOL_NUM) {
                printk(KERN_ERR "SBSRD parse: got too many number of tc_pool record.\n");
                return;
            }
            /* The index is in order by direction (ingress/egress) and then by buffer type (data/descriptor),
             * So the first 16 (pool num) are for data ingress, the second 16 are for data egress,
             * the third one are for descriptor ingress, the fourth are for descriptor egress */
            idx = pg_tc_sp_idx + SXD_BULK_CNTR_POOL_NUM + (SXD_BULK_CNTR_POOL_NUM * 2) * desc;
            sw_pool_id = layout_shared_buffer->sw_pool_id[idx];
            if (sw_pool_id != SXD_BULK_CNTR_POOL_ID_INVALID) {
                layout_shared_buffer->pool.statistics[pool_idx].sw_pool_id = sw_pool_id;
                layout_shared_buffer->pool.statistics[pool_idx].occupancy.curr_occupancy =
                    sbsrd.shared_buffer_status[i].buff_occupancy;
                layout_shared_buffer->pool.statistics[pool_idx].occupancy.watermark =
                    sbsrd.shared_buffer_status[i].buff_occupancy;
                pool_idx++;
            }
            pg_tc_sp_idx++;
            break;

        case SXD_BULK_CNTR_SHARED_BUFFER_ATTR_TYPE_CPU_TX_TC_E:
            /* Don't support it for now */
            break;

        default:
            printk(KERN_ERR "SBSRD parse: got unknown SBSRD type\n");
            return;
            break;
        }

        layout_common->counters_received_so_far++;
    }

    layout_shared_buffer->port_idx = port_idx;
    layout_shared_buffer->pg_tc_sp_idx = pg_tc_sp_idx;
    layout_shared_buffer->port_pool_idx = port_pool_idx;
    layout_shared_buffer->pool_idx = pool_idx;
    layout_shared_buffer->last_type = type;
}

void sx_bulk_cntr_handle_sbsrd(struct completion_info *ci)
{
    __sx_bulk_cntr_multi_emad_parse(ci->skb->data, ci->skb->len, __sbsrd_parser, NULL);
}

/***************************************************************************************
 * CEER
 */

static int __ceer_l2_type_map(sxd_ceer_l2_type_t l2_type, uint32_t *type_p)
{
    int rc = 0;

    switch (l2_type) {
    case SXD_CEER_L2_TYPE_NO_ETH_E:
    case SXD_CEER_L2_TYPE_ETH_E:
        *type_p = l2_type;
        break;

    default:
        printk(KERN_ERR "CEER invalid l2 type (%d).\n", l2_type);
        rc = -1;
        break;
    }

    return rc;
}

static int __ceer_vlan_type_map(sxd_ceer_vlan_type_t vlan_type, uint32_t *type_p)
{
    int rc = 0;

    switch (vlan_type) {
    case SXD_CEER_VLAN_TYPE_NO_TAG_E:
    case SXD_CEER_VLAN_TYPE_PRIO_TAG_E:
    case SXD_CEER_VLAN_TYPE_VLAN_TAG_E:
    case SXD_CEER_VLAN_TYPE_QINQ_E:
        *type_p = vlan_type;
        break;

    default:
        printk(KERN_ERR "CEER invalid vlan type (%d).\n", vlan_type);
        rc = -1;
        break;
    }

    return rc;
}

static int __ceer_l3_type_map(sxd_ceer_l3_type_t l3_type, uint32_t *type_p)
{
    int rc = 0;

    switch (l3_type) {
    case SXD_CEER_L3_TYPE_IPV4_E:
    case SXD_CEER_L3_TYPE_IPV6_E:
    case SXD_CEER_L3_TYPE_GRH_E:
    case SXD_CEER_L3_TYPE_FIBERCHANNEL_E:
        *type_p = l3_type;
        break;

    case SXD_CEER_L3_TYPE_UNKNOWN_E:
        *type_p = SXD_CEER_L3_TYPE_FIBERCHANNEL_E + 1;
        break;

    default:
        printk(KERN_ERR "CEER invalid l3 type (%d).\n", l3_type);
        rc = -1;
        break;
    }

    return rc;
}

static int __ceer_l4_type_map(sxd_ceer_l4_type_t l4_type, uint32_t *type_p)
{
    int rc = 0;

    switch (l4_type) {
    case SXD_CEER_L4_TYPE_TCP_E:
    case SXD_CEER_L4_TYPE_UDP_E:
    case SXD_CEER_L4_TYPE_ICMP_E:
    case SXD_CEER_L4_TYPE_IGMP_E:
    case SXD_CEER_L4_TYPE_BTH_E:
    case SXD_CEER_L4_TYPE_BTH_OVER_UDP_E:
    case SXD_CEER_L4_TYPE_AH_E:
    case SXD_CEER_L4_TYPE_ESP_E:
        *type_p = l4_type;
        break;

    case SXD_CEER_L4_TYPE_UNKNOWN_E:
        *type_p = SXD_CEER_L4_TYPE_ESP_E + 1;
        break;

    default:
        printk(KERN_ERR "CEER invalid l4 type (%d).\n", l4_type);
        rc = -1;
        break;
    }

    return rc;
}

static int __ceer_extra_data_type_map(sxd_ceer_tunnel_type_t tunnel_type, uint32_t *type_p)
{
    int rc = 0;

    switch (tunnel_type) {
    case SXD_CEER_TUNNEL_TYPE_NO_TUNNEL_E:
    case SXD_CEER_TUNNEL_TYPE_VXLAN_E:
    case SXD_CEER_TUNNEL_TYPE_NVGRE_E:
    case SXD_CEER_TUNNEL_TYPE_IP_IN_IP_E:
    case SXD_CEER_TUNNEL_TYPE_IP_IN_GRE_IN_IP_E:
    case SXD_CEER_TUNNEL_TYPE_MPLS_E:
        *type_p = tunnel_type;
        break;

    default:
        printk(KERN_ERR "CEER invalid tunnel type (%d).\n", tunnel_type);
        rc = -1;
        break;
    }

    return rc;
}

static int __ceer_extra_data_mpls_map(ku_elephant_flow_mpls_extra_data_t *mpls_data_p,
                                      sxd_ceer_ceer_extra_fields_MPLS_t  *ceer_mpls_p)
{
    int rc = 0;

    mpls_data_p->bos = ceer_mpls_p->mpls_bos;
    mpls_data_p->exp = ceer_mpls_p->mpls_exp;
    mpls_data_p->mpls_labels[0].label_valid = ceer_mpls_p->mpls_lb0_v;
    if (mpls_data_p->mpls_labels[0].label_valid == 1) {
        mpls_data_p->mpls_labels[0].label_id = ceer_mpls_p->mpls_label0_label_id;
        mpls_data_p->mpls_labels[0].ttl = ceer_mpls_p->mpls_label0_ttl;
    }
    mpls_data_p->mpls_labels[1].label_valid = ceer_mpls_p->mpls_lb1_v;
    if (mpls_data_p->mpls_labels[1].label_valid == 1) {
        mpls_data_p->mpls_labels[1].label_id = ceer_mpls_p->mpls_label1_label_id;
        mpls_data_p->mpls_labels[1].ttl = ceer_mpls_p->mpls_label1_ttl;
    }
    mpls_data_p->mpls_labels[2].label_valid = ceer_mpls_p->mpls_lb2_v;
    if (mpls_data_p->mpls_labels[2].label_valid == 1) {
        mpls_data_p->mpls_labels[2].label_id = ceer_mpls_p->mpls_label2_label_id;
        mpls_data_p->mpls_labels[2].ttl = ceer_mpls_p->mpls_label2_ttl;
    }
    mpls_data_p->mpls_labels[3].label_valid = ceer_mpls_p->mpls_lb3_v;
    if (mpls_data_p->mpls_labels[3].label_valid == 1) {
        mpls_data_p->mpls_labels[3].label_id = ceer_mpls_p->mpls_label3_label_id;
        mpls_data_p->mpls_labels[3].ttl = ceer_mpls_p->mpls_label3_ttl;
    }
    mpls_data_p->mpls_labels[4].label_valid = ceer_mpls_p->mpls_lb4_v;
    if (mpls_data_p->mpls_labels[4].label_valid == 1) {
        mpls_data_p->mpls_labels[4].label_id = ceer_mpls_p->mpls_label4_label_id;
        mpls_data_p->mpls_labels[4].ttl = ceer_mpls_p->mpls_label4_ttl;
    }

    return rc;
}

static int __ceer_extra_data_tunnel_map(sxd_ceer_tunnel_type_t                tunnel_type,
                                        ku_elephant_flow_tunnel_extra_data_t *tunnel_data_p,
                                        sxd_ceer_ceer_extra_fields_t         *ceer_tunnel_p)
{
    int rc = 0;

    if ((tunnel_type == SXD_CEER_TUNNEL_TYPE_VXLAN_E) ||
        (tunnel_type == SXD_CEER_TUNNEL_TYPE_NVGRE_E)) {
        tunnel_data_p->vni = ceer_tunnel_p->vni;
        memcpy(&tunnel_data_p->dmac, &ceer_tunnel_p->inner_dmac, sizeof(struct sx_ether_addr));
        memcpy(&tunnel_data_p->smac, &ceer_tunnel_p->inner_smac, sizeof(struct sx_ether_addr));
    }
    if (ceer_tunnel_p->inner_l3_type == SXD_CEER_INNER_L3_TYPE_IPV4_E) {
        tunnel_data_p->inner_l3.l3_type = SXD_CEER_INNER_L3_TYPE_IPV4_E;
        tunnel_data_p->inner_l3.dip.family = 1;
        tunnel_data_p->inner_l3.dip.ipv4 = ceer_tunnel_p->inner_dip[0];
        tunnel_data_p->inner_l3.sip.family = 1;
        tunnel_data_p->inner_l3.sip.ipv4 = ceer_tunnel_p->inner_sip[0];
        tunnel_data_p->inner_l3.dscp = ceer_tunnel_p->inner_dscp;
        tunnel_data_p->inner_l3.ecn = ceer_tunnel_p->inner_ecn;
        tunnel_data_p->inner_l3.ttl = ceer_tunnel_p->inner_ttl;
        tunnel_data_p->inner_l3.dont_frag = ceer_tunnel_p->inner_dont_frag;
    } else if (ceer_tunnel_p->inner_l3_type == SXD_CEER_INNER_L3_TYPE_OTHER_E) {
        tunnel_data_p->inner_l3.l3_type = SXD_CEER_L3_TYPE_FIBERCHANNEL_E + 1;
    } else {
        printk(KERN_ERR "CEER invalid inner l3 type (%d).\n", tunnel_data_p->inner_l3.l3_type);
        rc = -1;
    }

    return rc;
}

static int __get_port_index(struct sxd_bulk_cntr_buffer_layout_elephant *layout, u8 local, int *port_idx_p)
{
    int cnt = 0, i = 0, j = 0;
    int port_idx = local / 32;
    int port_bit = local % 32;

    /* Count number of turn-on bits up to local port bit */
    if (layout->port_mask[port_idx] & (1 << (port_bit))) {
        /* Count number of turn-on bits in dwords before local port dword (port_idx) */
        for (i = 0; i < port_idx; i++) {
            for (j = 0; j < 32; j++) {
                if (layout->port_mask[i] & (1 << j)) {
                    cnt++;
                }
            }
        }
        /* Count number of turn-on bits in local port dword before local port offset bit (port_bit) */
        for (j = 0; j < port_bit; j++) {
            if (layout->port_mask[port_idx] & (1 << j)) {
                cnt++;
            }
        }
    } else {
        printk(KERN_ERR "local port %d was not set in MOCS\n", local);
        return -1;
    }

    *port_idx_p = cnt;
    return 0;
}

static int __ceer_reg_to_data(ku_elephant_flow_data_t *data_p, struct ku_ceer_reg *ceer_p)
{
    int rc = 0;

    /* Set data key */
    data_p->data_key.flow_id = ceer_p->detection_entry;
    data_p->data_key.valid = ceer_p->det;

    /* Set data value */

    /* Hash data */
    data_p->data_value.hash.hash_valid = ceer_p->hash_valid;
    if (ceer_p->hash_valid == 1) {
        data_p->data_value.hash.lag_hash = ceer_p->lag_hash;
        data_p->data_value.hash.router_hash = ceer_p->router_hash;
    }

    /* L2 data */
    rc = __ceer_l2_type_map(ceer_p->l2_type, &data_p->data_value.l2.l2_type);
    if (rc != 0) {
        goto out;
    }
    if (ceer_p->l2_type == SXD_CEER_L2_TYPE_ETH_E) {
        rc = __ceer_vlan_type_map(ceer_p->vlan_type, &data_p->data_value.l2.vlan_type);
        if (rc != 0) {
            goto out;
        }
        memcpy(&data_p->data_value.l2.dmac, &ceer_p->dmac, sizeof(struct sx_ether_addr));
        memcpy(&data_p->data_value.l2.smac, &ceer_p->smac, sizeof(struct sx_ether_addr));
        if ((ceer_p->vlan_type == SXD_CEER_VLAN_TYPE_PRIO_TAG_E)
            || (ceer_p->vlan_type == SXD_CEER_VLAN_TYPE_VLAN_TAG_E)
            || (ceer_p->vlan_type == SXD_CEER_VLAN_TYPE_QINQ_E)) {
            data_p->data_value.l2.pcp_dei.dei = ceer_p->dei;
            data_p->data_value.l2.pcp_dei.pcp = ceer_p->pcp;
        }
        if ((ceer_p->vlan_type == SXD_CEER_VLAN_TYPE_VLAN_TAG_E)
            || (ceer_p->vlan_type == SXD_CEER_VLAN_TYPE_QINQ_E)) {
            data_p->data_value.l2.vid = ceer_p->vid;
        }
        if (ceer_p->vlan_type == SXD_CEER_VLAN_TYPE_QINQ_E) {
            data_p->data_value.l2.inner_pcp_dei.dei = ceer_p->inner_dei;
            data_p->data_value.l2.inner_pcp_dei.pcp = ceer_p->inner_pcp;
            data_p->data_value.l2.inner_vid = ceer_p->inner_vid;
        }
    }

    /* L3 data */
    rc = __ceer_l3_type_map(ceer_p->l3_type, &data_p->data_value.l3.l3_type);
    if (rc != 0) {
        goto out;
    }
    if ((ceer_p->l3_type == SXD_CEER_L3_TYPE_IPV4_E)
        || (ceer_p->l3_type == SXD_CEER_L3_TYPE_IPV4_E)
        || (ceer_p->l3_type == SXD_CEER_L3_TYPE_GRH_E)) {
        data_p->data_value.l3.dont_frag = ceer_p->dont_frag;
        data_p->data_value.l3.ttl = ceer_p->ttl;
        data_p->data_value.l3.ecn = ceer_p->ecn;
        data_p->data_value.l3.dscp = ceer_p->dscp;
        if (ceer_p->l3_type == SXD_CEER_L3_TYPE_IPV4_E) {
            data_p->data_value.l3.sip.family = 1;
            data_p->data_value.l3.sip.ipv4 = ceer_p->sip[0];
            data_p->data_value.l3.dip.family = 1;
            data_p->data_value.l3.dip.ipv4 = ceer_p->dip[0];
        } else {
            data_p->data_value.l3.sip.family = 2;
            memcpy(data_p->data_value.l3.sip.ipv6, ceer_p->sip, SXD_CEER_SIP_NUM * sizeof(uint32_t));
            data_p->data_value.l3.dip.family = 2;
            memcpy(data_p->data_value.l3.dip.ipv6, ceer_p->dip, SXD_CEER_DIP_NUM * sizeof(uint32_t));
        }
    }

    /* Extra data type - needed for L4 */
    rc = __ceer_extra_data_type_map(ceer_p->tunnel_type, &data_p->data_value.extra_data.extra_data_key);
    if (rc != 0) {
        goto out;
    }

    /* L4 data */
    rc = __ceer_l4_type_map(ceer_p->l4_type, &data_p->data_value.l4.l4_type);
    if (rc != 0) {
        goto out;
    }
    if ((ceer_p->l4_type == SXD_CEER_L4_TYPE_TCP_E)
        || (ceer_p->l4_type == SXD_CEER_L4_TYPE_UDP_E)) {
        data_p->data_value.l4.l4_source_port = ceer_p->sport;
        if (ceer_p->tunnel_type != SXD_CEER_TUNNEL_TYPE_VXLAN_E) {
            data_p->data_value.l4.l4_destination_port = ceer_p->dport;
        }
    }

    /* Extra data value */
    if (ceer_p->extra_data_type == SXD_CEER_EXTRA_DATA_TYPE_NONE_E) {
        goto out;
    }

    if (ceer_p->extra_data_type == SXD_CEER_EXTRA_DATA_TYPE_MPLS_E) {
        rc = __ceer_extra_data_mpls_map(&data_p->data_value.extra_data.mpls_extra_data,
                                        &ceer_p->extra_data.ceer_extra_fields_MPLS);
        if (rc != 0) {
            goto out;
        }
    } else if (ceer_p->extra_data_type == SXD_CEER_EXTRA_DATA_TYPE_TUNNEL_E) {
        rc = __ceer_extra_data_tunnel_map(data_p->data_value.extra_data.extra_data_key,
                                          &data_p->data_value.extra_data.tunnel_extra_data,
                                          &ceer_p->extra_data.ceer_extra_fields);
        if (rc != 0) {
            goto out;
        }
    } else {
        printk(KERN_ERR "CEER invalid extra data type (%d).\n", ceer_p->extra_data_type);
        rc = -1;
    }

out:
    return rc;
}

static void __ceer_parser(const void                                *ceer_reg,
                          u32                                        reg_len,
                          struct sxd_bulk_cntr_buffer_layout_common *layout_common,
                          void                                      *context)
{
    struct ku_ceer_reg                           ceer;
    struct sxd_bulk_cntr_buffer_layout_elephant *layout = (struct sxd_bulk_cntr_buffer_layout_elephant*)layout_common;
    int                                          ret = 0, port_idx = 0, data_idx = 0;

    memset(&ceer, 0, sizeof(ceer));

    ret = __CEER_decode((u8*)ceer_reg, &ceer, context);
    if (ret != 0) {
        printk(KERN_ERR "Failed to decode CEER\n");
        return;
    }

    if (ceer.det == 0) {
        printk(KERN_ERR "Local %d flow %d ID not elephant flow\n",
               ceer.local_port, ceer.detection_entry);
        return;
    }

    ret = __get_port_index(layout, ceer.local_port, &port_idx);
    if (ret != 0) {
        printk(KERN_ERR "Failed to get port index\n");
        return;
    }

    layout->port_flows[port_idx].flow_id_list[layout->port_flows[port_idx].flow_cnt] = ceer.detection_entry;
    layout->port_flows[port_idx].flow_cnt++;

    data_idx = SXD_COS_ELEPHANT_FLOW_ID_NUM_MAX * port_idx + ceer.detection_entry;

    ret = __ceer_reg_to_data(&layout->data[data_idx], &ceer);
    if (ret != 0) {
        printk(KERN_ERR "Failed to convert CEER to flow data\n");
        return;
    }
}

void sx_bulk_cntr_handle_ceer(struct completion_info *ci)
{
    __sx_bulk_cntr_multi_emad_parse(ci->skb->data, ci->skb->len, __ceer_parser, NULL);
}