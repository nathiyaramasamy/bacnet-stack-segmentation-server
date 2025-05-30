/**************************************************************************
 *
 * Copyright (C) 2005 Steve Karg
 * Corrections by Ferran Arumi, 2007, Barcelona, Spain
 *
 * SPDX-License-Identifier: GPL-2.0-or-later WITH GCC-exception-2.0
 *
 *********************************************************************/
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/* BACnet Stack defines - first */
#include "bacnet/bacdef.h"
/* BACnet Stack API */
#include "bacnet/apdu.h"
#include "bacnet/bacaddr.h"
#include "bacnet/bacdcode.h"
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/datalink/datalink.h"
#include "bacnet/basic/services.h"
#include "bacnet/basic/binding/address.h"
#if BACNET_SEGMENTATION_ENABLED
#include <stdlib.h>
#include "bacnet/segmentack.h"
#include "bacnet/abort.h"
#include "bacnet/basic/sys/platform.h"

#define DEFAULT_WINDOW_SIZE 32

/*Number of Duplicate Segments Received. */
static uint8_t Duplicate_Count = 0;

/* Indirection of state machine data with peer unique id values */
static BACNET_TSM_INDIRECT_DATA TSM_Peer_Ids[MAX_TSM_PEERS];
#endif // BACNET_SEGMENTATION_ENABLED

/** @file tsm.c  BACnet Transaction State Machine operations  */
/* FIXME: modify basic service handlers to use TSM rather than this buffer! */
uint8_t Handler_Transmit_Buffer[MAX_PDU];

#if (MAX_TSM_TRANSACTIONS)
/* Really only needed for segmented messages */
/* and a little for sending confirmed messages */
/* If we are only a server and only initiate broadcasts, */
/* then we don't need a TSM layer. */

/* FIXME: not coded for segmentation */

/* declare space for the TSM transactions, and set it up in the init. */
/* table rules: an Invoke ID = 0 is an unused spot in the table */
static BACNET_TSM_DATA TSM_List[MAX_TSM_TRANSACTIONS];

/* invoke ID for incrementing between subsequent calls. */
static uint8_t Current_Invoke_ID = 1;

static tsm_timeout_function Timeout_Function;

void tsm_set_timeout_handler(tsm_timeout_function pFunction)
{
    Timeout_Function = pFunction;
}

/** Find the given Invoke-Id in the list and
 *  return the index.
 *
 * @param invokeID  Invoke Id
 *
 * @return Index of the id or MAX_TSM_TRANSACTIONS
 *         if not found
 */
static uint8_t tsm_find_invokeID_index(uint8_t invokeID)
{
    unsigned i = 0; /* counter */
    uint8_t index = MAX_TSM_TRANSACTIONS; /* return value */

    const BACNET_TSM_DATA *plist = TSM_List;

    for (i = 0; i < MAX_TSM_TRANSACTIONS; i++, plist++) {
        if (plist->InvokeID == invokeID) {
            index = (uint8_t)i;
            break;
        }
    }

    return index;
}

/** Find the first free index in the TSM table.
 *
 * @return Index of the id or MAX_TSM_TRANSACTIONS
 *         if no entry is free.
 */
static uint8_t tsm_find_first_free_index(void)
{
    unsigned i = 0; /* counter */
    uint8_t index = MAX_TSM_TRANSACTIONS; /* return value */

    const BACNET_TSM_DATA *plist = TSM_List;

    for (i = 0; i < MAX_TSM_TRANSACTIONS; i++, plist++) {
        if (plist->InvokeID == 0) {
            index = (uint8_t)i;
            break;
        }
    }

    return index;
}

/** Check if space for transactions is available.
 *
 * @return true/false
 */
bool tsm_transaction_available(void)
{
    bool status = false; /* return value */
    unsigned i = 0; /* counter */

    const BACNET_TSM_DATA *plist = TSM_List;

    for (i = 0; i < MAX_TSM_TRANSACTIONS; i++, plist++) {
        if (plist->InvokeID == 0) {
            /* one is available! */
            status = true;
            break;
        }
    }

    return status;
}

/** Return the count of idle transaction.
 *
 * @return Count of idle transaction.
 */
uint8_t tsm_transaction_idle_count(void)
{
    uint8_t count = 0; /* return value */
    unsigned i = 0; /* counter */

    const BACNET_TSM_DATA *plist = TSM_List;

    for (i = 0; i < MAX_TSM_TRANSACTIONS; i++, plist++) {
        if ((plist->InvokeID == 0) && (plist->state == TSM_STATE_IDLE)) {
            /* one is available! */
            count++;
        }
    }

    return count;
}

/**
 * Sets the current invokeID.
 *
 * @param invokeID  Invoke ID
 */
void tsm_invokeID_set(uint8_t invokeID)
{
    if (invokeID == 0) {
        invokeID = 1;
    }
    Current_Invoke_ID = invokeID;
}

/** Gets the next free invokeID,
 * and reserves a spot in the table
 * returns 0 if none are available.
 *
 * @return free invoke ID
 */
uint8_t tsm_next_free_invokeID(void)
{
    uint8_t index = 0;
    uint8_t invokeID = 0;
    bool found = false;
    BACNET_TSM_DATA *plist = NULL;

    /* Is there even space available? */
    if (tsm_transaction_available()) {
        while (!found) {
            index = tsm_find_invokeID_index(Current_Invoke_ID);
            if (index == MAX_TSM_TRANSACTIONS) {
                /* Not found, so this invokeID is not used */
                found = true;
                /* set this id into the table */
                index = tsm_find_first_free_index();
                if (index != MAX_TSM_TRANSACTIONS) {
                    plist = &TSM_List[index];
                    plist->InvokeID = invokeID = Current_Invoke_ID;
                    plist->state = TSM_STATE_IDLE;
                    plist->RequestTimer = apdu_timeout();
                    /* update for the next call or check */
                    Current_Invoke_ID++;
                    /* skip zero - we treat that internally as invalid or no
                     * free */
                    if (Current_Invoke_ID == 0) {
                        Current_Invoke_ID = 1;
                    }
                }
            } else {
                /* found! This invokeID is already used */
                /* try next one */
                Current_Invoke_ID++;
                /* skip zero - we treat that internally as invalid or no free */
                if (Current_Invoke_ID == 0) {
                    Current_Invoke_ID = 1;
                }
            }
        }
    }

    return invokeID;
}

/** Set for an unsegmented transaction
 *  the state to await confirmation.
 *
 * @param invokeID  Invoke-ID
 * @param dest  Pointer to the BACnet destination address.
 * @param ndpu_data  Pointer to the NPDU structure.
 * @param apdu  Pointer to the received message.
 * @param apdu_len  Bytes valid in the received message.
 */
void tsm_set_confirmed_unsegmented_transaction(
    uint8_t invokeID,
    const BACNET_ADDRESS *dest,
    const BACNET_NPDU_DATA *ndpu_data,
    const uint8_t *apdu,
    uint16_t apdu_len)
{
    uint16_t j = 0;
    uint8_t index;
    BACNET_TSM_DATA *plist;

    if (invokeID && ndpu_data && apdu && (apdu_len > 0)) {
        index = tsm_find_invokeID_index(invokeID);
        if (index < MAX_TSM_TRANSACTIONS) {
            plist = &TSM_List[index];
            /* SendConfirmedUnsegmented */
            plist->state = TSM_STATE_AWAIT_CONFIRMATION;
            plist->RetryCount = 0;
            /* start the timer */
            plist->RequestTimer = apdu_timeout();
            /* copy the data */
            for (j = 0; j < apdu_len; j++) {
                plist->apdu[j] = apdu[j];
            }
            plist->apdu_len = apdu_len;
            npdu_copy_data(&plist->npdu_data, ndpu_data);
            bacnet_address_copy(&plist->dest, dest);
        }
    }

    return;
}

/** Used to retrieve the transaction payload. Used
 *  if we wanted to find out what we sent (i.e. when
 *  we get an ack).
 *
 * @param invokeID  Invoke-ID
 * @param dest  Pointer to the BACnet destination address.
 * @param ndpu_data  Pointer to the NPDU structure.
 * @param apdu  Pointer to the received message.
 * @param apdu_len  Pointer to a variable, that takes
 *                  the count of bytes valid in the
 *                  received message.
 */
bool tsm_get_transaction_pdu(
    uint8_t invokeID,
    BACNET_ADDRESS *dest,
    BACNET_NPDU_DATA *ndpu_data,
    uint8_t *apdu,
    uint16_t *apdu_len)
{
    uint16_t j = 0;
    uint8_t index;
    bool found = false;
    BACNET_TSM_DATA *plist;

    if (invokeID && apdu && ndpu_data && apdu_len) {
        index = tsm_find_invokeID_index(invokeID);
        /* how much checking is needed?  state?  dest match? just invokeID? */
        if (index < MAX_TSM_TRANSACTIONS) {
            /* FIXME: we may want to free the transaction so it doesn't timeout
             */
            /* retrieve the transaction */
            plist = &TSM_List[index];
            *apdu_len = (uint16_t)plist->apdu_len;
            if (*apdu_len > MAX_PDU) {
                *apdu_len = MAX_PDU;
            }
            for (j = 0; j < *apdu_len; j++) {
                apdu[j] = plist->apdu[j];
            }
            npdu_copy_data(ndpu_data, &plist->npdu_data);
            bacnet_address_copy(dest, &plist->dest);
            found = true;
        }
    }

    return found;
}

/** Frees the invokeID and sets its state to IDLE
 *
 * @param invokeID  Invoke-ID
 */
void tsm_free_invoke_id(uint8_t invokeID)
{
    uint8_t index;
    BACNET_TSM_DATA *plist;

    index = tsm_find_invokeID_index(invokeID);
    if (index < MAX_TSM_TRANSACTIONS) {
        plist = &TSM_List[index];
        plist->state = TSM_STATE_IDLE;
        plist->InvokeID = 0;
    }
}

/** Check if the invoke ID has been made free by the Transaction State Machine.
 * @param invokeID [in] The invokeID to be checked, normally of last message
 * sent.
 * @return True if it is free (done with), False if still pending in the TSM.
 */
bool tsm_invoke_id_free(uint8_t invokeID)
{
    bool status = true;
    uint8_t index;

    index = tsm_find_invokeID_index(invokeID);
    if (index < MAX_TSM_TRANSACTIONS) {
        status = false;
    }

    return status;
}

/** See if we failed get a confirmation for the message associated
 *  with this invoke ID.
 * @param invokeID [in] The invokeID to be checked, normally of last message
 * sent.
 * @return True if already failed, False if done or segmented or still waiting
 *         for a confirmation.
 */
bool tsm_invoke_id_failed(uint8_t invokeID)
{
    bool status = false;
    uint8_t index;

    index = tsm_find_invokeID_index(invokeID);
    if (index < MAX_TSM_TRANSACTIONS) {
        /* a valid invoke ID and the state is IDLE is a
           message that failed to confirm */
        if (TSM_List[index].state == TSM_STATE_IDLE) {
            status = true;
        }
    }

    return status;
}

#if BACNET_SEGMENTATION_ENABLED
void segmentack_pdu_send(
    BACNET_ADDRESS *dest,
    bool negativeack,
    bool server,
    uint8_t invoke_id,
    uint8_t sequence_number,
    uint8_t actual_window_size)
{
    int pdu_len = 0;
    BACNET_NPDU_DATA npdu_data;
    int bytes_sent;
    BACNET_ADDRESS my_address;
    int apdu_len = 0;
    int npdu_len = 0;
    uint8_t Transmit_Buffer[MAX_PDU] = { 0 };
    datalink_get_my_address(&my_address);
    npdu_encode_npdu_data(&npdu_data, false, MESSAGE_PRIORITY_NORMAL);
    npdu_len = npdu_encode_pdu(&Transmit_Buffer[0], dest, &my_address, &npdu_data);
    apdu_len = segmentack_encode_apdu(
        &Transmit_Buffer[npdu_len], negativeack, server, invoke_id,
        sequence_number, actual_window_size);
    pdu_len = apdu_len + npdu_len;
    bytes_sent = datalink_send_pdu(dest, &npdu_data, &Transmit_Buffer[0], pdu_len);
#if PRINT_ENABLED
    fprintf(stderr, "bytes sent=%d\n", bytes_sent);
#endif
}

/* theorical size of apdu fixed header */
uint32_t
get_apdu_header_typical_size(BACNET_APDU_FIXED_HEADER *header, bool segmented)
{
    int segmented_ack = 5;
    int unsegmented_ack = 3;
    int segmented_request = 6;
    int unsegmented_request = 4;
    switch (header->pdu_type) {
        case PDU_TYPE_COMPLEX_ACK:
            return segmented ? segmented_ack : unsegmented_ack;
        case PDU_TYPE_CONFIRMED_SERVICE_REQUEST:
            return segmented ? segmented_request : unsegmented_request;
        default:
            break;
    }
    return unsegmented_ack;
}

/* Free allocated blob data */
void free_blob(BACNET_TSM_DATA *data)
{
    /* Free received data blobs */
    if (data->apdu_blob) {
        free(data->apdu_blob);
    }
    data->apdu_blob = NULL;
    data->apdu_blob_allocated = 0;
    data->apdu_blob_size = 0;
    /* Free sent data blobs */
    if (data->apdu) {
        free(data->apdu);
    }
    data->apdu = NULL;
    data->apdu_len = 0;
}

/* keeps allocated blob data, but reset data & current size */
void reset_blob(BACNET_TSM_DATA *data)
{
    data->apdu_blob_size = 0;
}

/* allocate new data if necessary, keeps existing bytes */
void ensure_extra_blob_size(BACNET_TSM_DATA *data, uint32_t allocation_unit)
{
    if (!allocation_unit) { /* NOP */
        return;
    }
    /* allocation needed ? */
    if ((!data->apdu_blob) || (!data->apdu_blob_allocated) ||
        ((allocation_unit + data->apdu_blob_size) >
         data->apdu_blob_allocated)) {
        /* stupid idiot allocation algorithm : space allocated shall augment
         * exponentially */
        /* (nb: here there may be extra space remaining) */
        uint8_t *apdu_new_blob =
            calloc(1, data->apdu_blob_allocated + allocation_unit);
        /* recopy old data */
        if (data->apdu_blob_size) {
            memcpy(apdu_new_blob, data->apdu_blob, data->apdu_blob_size);
        }
        /* new values */
        if (data->apdu_blob) {
            free(data->apdu_blob);
        }
        data->apdu_blob = apdu_new_blob;
        data->apdu_blob_allocated = data->apdu_blob_allocated + allocation_unit;
    }
}

/* add new data to current blob (allocate extra space if necessary) */
void add_blob_data(BACNET_TSM_DATA *data, uint8_t *bdata, uint32_t data_len)
{
    ensure_extra_blob_size(data, data_len);
    memcpy(&data->apdu_blob[data->apdu_blob_size], bdata, data_len);
    data->apdu_blob_size += data_len;
}

/* gets current blob data */
uint8_t *get_blob_data(BACNET_TSM_DATA *data, uint16_t *data_len)
{
    *data_len = data->apdu_blob_size;
    return data->apdu_blob;
}

/* Copy new data to current APDU sending blob data */
void copy_apdu_blob_data(
    BACNET_TSM_DATA *data, uint8_t *bdata, uint32_t data_len)
{
    if (data->apdu) {
        free(data->apdu);
    }
    data->apdu = NULL;
    data->apdu = calloc(1, data_len);
    if (data->apdu != NULL)
    {
        memcpy(data->apdu, bdata, data_len);
        data->apdu_len = data_len;
    }
}

/* gets Nth packet data to send in a segmented operation, or get the only data
 * packet in unsegmented world. */
uint8_t *get_apdu_blob_data_segment(
    BACNET_TSM_DATA *data, int segment_number, uint32_t *data_len)
{
    /* Data is splitted in N blocks of, at maximum, ( APDU_MAX - APDU_HEADER )
     * bytes */
    bool segmented =
        data->apdu_fixed_header.service_data.common_data.segmented_message;
    int header_size =
        get_apdu_header_typical_size(&data->apdu_fixed_header, segmented);
    int block_request_size = (data->apdu_maximum_length - header_size);
    block_request_size = block_request_size;
    int data_position = segment_number * block_request_size;
    int remaining_size = (int)data->apdu_len - data_position;
    *data_len = (uint32_t)max(0, min(remaining_size, block_request_size));
    return data->apdu + data_position;
}

/** Clear TSM Peer data */
void tsm_clear_peer_id(uint8_t InternalInvokeID)
{
    int ix;

    /* look for a matching internal invoke ID */
    for (ix = 0; ix < MAX_TSM_PEERS; ix++) {
        /* see if it matches the internal number */
        if (TSM_Peer_Ids[ix].InternalInvokeID == InternalInvokeID) {
            TSM_Peer_Ids[ix].InternalInvokeID = 0;
        }
    }
}

/* frees the invokeID and sets its state to IDLE */
void tsm_free_invoke_id_check(
    uint8_t invokeID, BACNET_ADDRESS *peer_address, bool cleanup)
{
    uint8_t index;

    index = tsm_find_invokeID_index(invokeID);

    if ((index < MAX_TSM_TRANSACTIONS) &&
        (!peer_address || address_match(peer_address, &TSM_List[index].dest))) {
        /* check "double-free" cases */
        TSM_List[index].state = TSM_STATE_IDLE;
        /* Clear Peer data, if any. Lookup with our internal ID status. */
        tsm_clear_peer_id(invokeID);
        /* flag slot as "unused" */
        TSM_List[index].InvokeID = 0;

        if (cleanup) {
            /* Release segmented data */
            free_blob(&TSM_List[index]);
        }
    }
}

/** Finds (optionally creates) an existing peer data */
static BACNET_TSM_INDIRECT_DATA *
tsm_get_peer_id_data(BACNET_ADDRESS *src, uint8_t invokeID, bool createPeerId)
{
    int ix;
    int index;
    int free_ix_found = -1;
    BACNET_TSM_INDIRECT_DATA *item = NULL;

    /* look for an empty slot, or a matching (address,peer invoke ID) */
    for (ix = 0; ix < MAX_TSM_PEERS && !item; ix++) {
        /* not free : see if it matches */
        if (TSM_Peer_Ids[ix].InternalInvokeID != 0) {
            if (invokeID == TSM_Peer_Ids[ix].PeerInvokeID &&
                address_match(src, &TSM_Peer_Ids[ix].PeerAddress)) {
                item = &TSM_Peer_Ids[ix];
            }
        } else if (free_ix_found < 0) {
            /* mark free slot found */
            free_ix_found = ix;
        }
    }

    /* create new data */
    if ((!item) && createPeerId && (free_ix_found > -1)) {
        /* memorize peer data */
        TSM_Peer_Ids[free_ix_found].PeerInvokeID = invokeID;
        TSM_Peer_Ids[free_ix_found].PeerAddress = *src;
        /* create an internal TSM slot (with internal invokeID number which is
         * not relevant) */
        TSM_Peer_Ids[free_ix_found].InternalInvokeID = tsm_next_free_invokeID();
        index = tsm_find_invokeID_index(
            TSM_Peer_Ids[free_ix_found].InternalInvokeID);
        if (index < MAX_TSM_TRANSACTIONS) {
            /* explicitely memorize peer InvokeID */
            TSM_List[index].InvokeID =
                TSM_Peer_Ids[free_ix_found].InternalInvokeID;
            TSM_List[index].dest = *src;
            item = &TSM_Peer_Ids[free_ix_found];
        } else {
            /* problem : reset slot (NULL returned) */
            TSM_Peer_Ids[free_ix_found].InternalInvokeID = 0;
        }
    }

    return item;
}

/** Associates a Peer address and invoke ID with our TSM
@returns A local InvokeID unique number, 0 in case of error. */
uint8_t tsm_get_peer_id(BACNET_ADDRESS *src, uint8_t invokeID)
{
    BACNET_TSM_INDIRECT_DATA *peer_data;
    peer_data = tsm_get_peer_id_data(src, invokeID, true);
    if (peer_data) {
        return peer_data->InternalInvokeID;
    }
    return 0;
}

bool DuplicateInWindow(
    BACNET_TSM_DATA *tsm_data,
    uint8_t seqA,
    uint32_t first_sequence_number,
    uint32_t last_sequence_number)
{
    uint8_t received_count =
        (last_sequence_number - first_sequence_number) % 256;
    if (received_count > tsm_data->ActualWindowSize) {
        return false;
    } else if ((seqA - first_sequence_number) % 256 <= received_count) {
        return true;
    } else if (
        (received_count == 0) &&
        ((first_sequence_number - seqA) % 256 <= tsm_data->ActualWindowSize)) {
        return true;
    } else {
        return false;
    }
}

bool duplicate_segment_received(
    uint8_t index,
    BACNET_CONFIRMED_SERVICE_DATA *service_data,
    BACNET_ADDRESS *src)
{
    BACNET_NPDU_DATA npdu_data;
    bool isDuplicate = false;
    uint8_t Ndup = TSM_List[index].ActualWindowSize;
    // DuplicateSegmentReceived
    if (Duplicate_Count < Ndup) {
        TSM_List[index].SegmentTimer = apdu_segment_timeout();
        Duplicate_Count++;
        isDuplicate = true;
    }
    // TooManyDuplicateSegmentsReceived
    else if (Duplicate_Count == Ndup) {
        npdu_encode_npdu_data(&npdu_data, false, MESSAGE_PRIORITY_NORMAL);
        segmentack_pdu_send(
            src, true, true, service_data->invoke_id,
            TSM_List[index].LastSequenceNumber,
            TSM_List[index].ActualWindowSize);
        TSM_List[index].SegmentTimer = apdu_segment_timeout();
        TSM_List[index].InitialSequenceNumber =
            TSM_List[index].LastSequenceNumber;
        Duplicate_Count = 0;
        isDuplicate = true;
    }
    return isDuplicate;
}

/* send an Abort-PDU message because of incorrect segment/PDU received */
void abort_pdu_send(
    uint8_t invoke_id, BACNET_ADDRESS *dest, uint8_t reason, bool server)
{
    int pdu_len = 0;
    BACNET_NPDU_DATA npdu_data;
    int bytes_sent;
    BACNET_ADDRESS my_address;
    int apdu_len = 0;
    int npdu_len = 0;
    uint8_t Transmit_Buffer[MAX_PDU] = { 0 };

    datalink_get_my_address(&my_address);
    npdu_encode_npdu_data(&npdu_data, false, MESSAGE_PRIORITY_NORMAL);
    npdu_len = npdu_encode_pdu(&Transmit_Buffer[0], dest, &my_address, &npdu_data);
    apdu_len = abort_encode_apdu(
        &Transmit_Buffer[npdu_len], invoke_id, reason, server);
    pdu_len = apdu_len + npdu_len;
    bytes_sent = datalink_send_pdu(dest, &npdu_data, &Transmit_Buffer[0], pdu_len);
}

/** We received a segment of a ConfirmedService packet, check TSM state and
 * reassemble the full packet */
bool tsm_set_segmented_confirmed_service_received(
    BACNET_ADDRESS *src,
    BACNET_CONFIRMED_SERVICE_DATA *service_data,
    uint8_t *internal_invoke_id,
    uint8_t **pservice_request, /* IN/OUT */
    uint16_t *pservice_request_len /* IN/OUT */
)
{
    uint8_t index;
    uint8_t *service_request = *pservice_request;
    uint32_t service_request_len = *pservice_request_len;
    bool result = false;
    bool ack_needed = false;
    uint8_t internal_service_id = tsm_get_peer_id(src, service_data->invoke_id);
    *internal_invoke_id = internal_service_id;
    if (!internal_service_id) {
        /* failed : could not allocate enough slot for this transaction */
        abort_pdu_send(
            service_data->invoke_id, src,
            ABORT_REASON_PREEMPTED_BY_HIGHER_PRIORITY_TASK, true);
        return false;
    }
    index = tsm_find_invokeID_index(internal_service_id);
    if (index >= MAX_TSM_TRANSACTIONS) { /* shall not fail */
        abort_pdu_send(service_data->invoke_id, src, ABORT_REASON_OTHER, true);
        return false;
    }
    /* check states */
    switch (TSM_List[index].state) {
            /* Initial state: ConfirmedSegmentReceived */
        case TSM_STATE_IDLE:
            /* We never stay in IDLE state */
            TSM_List[index].state = TSM_STATE_SEGMENTED_REQUEST_SERVER;
            /* First time : Compute Actual WindowSize */
            /* we automatically accept the proposed window size */
            TSM_List[index].ActualWindowSize =
                TSM_List[index].ProposedWindowSize =
                    service_data->proposed_window_number;
            /* Init sequence numbers */
            TSM_List[index].InitialSequenceNumber = 0;
            TSM_List[index].LastSequenceNumber = 0;
            /* resets counters */
            TSM_List[index].RetryCount = 0;
            TSM_List[index].SegmentRetryCount = 0;
            TSM_List[index].ReceivedSegmentsCount = 1;
            /* stop unsegmented timer */
            TSM_List[index].RequestTimer = 0; /* unused */
            /* start the segmented timer */
            TSM_List[index].SegmentTimer = apdu_segment_timeout() * 4;
            /* reset memorized data */
            reset_blob(&TSM_List[index]);
            
            // ConfirmedSegmentedReceivedWindowSizeOutofRange
            if (service_data->sequence_number == 0 &&
                (TSM_List[index].ProposedWindowSize == 0 ||
                 TSM_List[index].ProposedWindowSize > 127)) {
                abort_pdu_send(
                    service_data->invoke_id, src,
                    ABORT_REASON_WINDOW_SIZE_OUT_OF_RANGE, true);

                TSM_List[index].state = TSM_STATE_IDLE;

                break;
            }

            /* Test : sequence number MUST be 0 */
            /* UnexpectedPDU_Received */
            if (service_data->sequence_number != 0) {
                /* Release data */
                free_blob(&TSM_List[index]);
                /* Abort */
                abort_pdu_send(
                    service_data->invoke_id, src,
                    ABORT_REASON_INVALID_APDU_IN_THIS_STATE, true);
                /* We must free invoke_id ! */
                tsm_free_invoke_id_check(internal_service_id, NULL, true);
            } else {
                /* Okay : memorize data */
                add_blob_data(
                    &TSM_List[index], service_request, service_request_len);
                /* We ACK the first segment of the segmented message */
                segmentack_pdu_send(
                    src, false, true, service_data->invoke_id,
                    TSM_List[index].LastSequenceNumber,
                    TSM_List[index].ActualWindowSize);
            }
            break;
            /* New segments  */
        case TSM_STATE_SEGMENTED_REQUEST_SERVER:
            /* reset the segment timer */
            TSM_List[index].RequestTimer = 0; /* unused */
            /* ANSI/ASHRAE 135-2008 5.4.5.2 SEGMENTED_REQUEST / Timeout */
            /* If SegmentTimer becomes greater than Tseg times four, [...] enter
             * IDLE state */
            TSM_List[index].SegmentTimer = apdu_segment_timeout() * 4;
            /* Sequence number MUST be (LastSequenceNumber+1 modulo 256) */

            if (TSM_List[index].SegmentTimer > apdu_timeout()) {
                abort_pdu_send(
                    service_data->invoke_id, src,
                    ABORT_REASON_APPLICATION_EXCEEDED_REPLY_TIME, true);

                /* Enter IDLE state */
                TSM_List[index].state = TSM_STATE_IDLE;
            }

            // DuplicateSegmentReceived
            if ((service_data->sequence_number !=
                 (uint8_t)(TSM_List[index].LastSequenceNumber + 1) % 256))

            {
                if (DuplicateInWindow(
                        &TSM_List[index], service_data->sequence_number,
                        (TSM_List[index].InitialSequenceNumber) % 256,
                        TSM_List[index].LastSequenceNumber)) {
                    // DuplicateSegmentReceived
                    if (duplicate_segment_received(index, service_data, src)) {
                        // state is in TSM_STATE_SEGMENTED_REQUEST_SERVER
                        break;
                    }
                } else {
                    /* Recoverable Error: SegmentReceivedOutOfOrder */
                    /* ACK of last segment correctly received. */
                    segmentack_pdu_send(
                        src, true, true, service_data->invoke_id,
                        TSM_List[index].LastSequenceNumber,
                        TSM_List[index].ActualWindowSize);

                    Duplicate_Count = 0;
                }
            } else {
                /* Count maximum segments */
                if (++TSM_List[index].ReceivedSegmentsCount >
                    MAX_SEGMENTS_ACCEPTED) {
                    /* ABORT: SegmentReceivedOutOfSpace */
                    abort_pdu_send(
                        service_data->invoke_id, src,
                        ABORT_REASON_BUFFER_OVERFLOW, true);
                    /* Release data */
                    free_blob(&TSM_List[index]);
                    /* Enter IDLE state */
                    TSM_List[index].state = TSM_STATE_IDLE;
                    /* We must free invoke_id ! */
                    tsm_free_invoke_id_check(internal_service_id, NULL, true);
                } else {
                    /* NewSegmentReceived */
                    TSM_List[index].LastSequenceNumber =
                        service_data->sequence_number;
                    add_blob_data(
                        &TSM_List[index], service_request, service_request_len);
                    /* LastSegmentOfComplexACK_Received */
                    if (service_data->sequence_number ==
                        (uint8_t)(TSM_List[index].InitialSequenceNumber +
                                  TSM_List[index].ActualWindowSize)) {
                        ack_needed = true;
                        TSM_List[index].InitialSequenceNumber =
                            service_data->sequence_number;
                    }
                    /* LastSegmentOfComplexACK_Received */
                    if (!service_data->more_follows) {
                        /* Resulting segment data */
                        *pservice_request = get_blob_data(
                            &TSM_List[index], pservice_request_len);
                        result =
                            true; /* Returns true on final segment received */
                        ack_needed = true;
                    }
                    /* LastSegmentOfComplexACK_Received or
                     * LastSegmentOfGroupReceived */
                    if (ack_needed) {
                        /* ACK received segment */
                        segmentack_pdu_send(
                            src, false, true, service_data->invoke_id,
                            TSM_List[index].LastSequenceNumber,
                            TSM_List[index].ActualWindowSize);
                    }
                }
            }
            break;
        default: 
            break;
    }
    return result;
}

/* calculates how many segments will be used to send data in this TSM slot
@return 1 : No segmentation needed, >1 segmentation needed (number of segments).
*/
uint32_t get_apdu_max_segments(BACNET_TSM_DATA *data)
{
    uint32_t header_size;
    uint32_t packets;

    /* Are we unsegmented ? */
    header_size = get_apdu_header_typical_size(&data->apdu_fixed_header, false);
    if (header_size + data->apdu_len <= data->apdu_maximum_length) {
        return 1;
    }

    /* We are segmented : calculate how many segments to use */
    header_size = get_apdu_header_typical_size(&data->apdu_fixed_header, true);

    /* Number of packets to use formula : p = ( ( total_length - 1 ) /
     * packet_length ) + 1; */
    packets =
        ((data->apdu_len - 1) / (data->apdu_maximum_length - header_size)) + 1;

    return packets;
}

void bacnet_calc_transmittable_length(
    BACNET_ADDRESS *dest,
    BACNET_CONFIRMED_SERVICE_DATA *confirmed_service_data,
    uint32_t *apdu_max,
    uint32_t *total_max)
{
    uint32_t deviceId = 0;
    unsigned max_apdu;
    uint8_t segmentation = 0;
    uint16_t maxsegments = 0;
    BACNET_ADDRESS src;

    /* either we are replying to a confirmed service, so we use prompted values
       ; either we are requesting a peer, so we use memorised informations about
       the peer device.
     */
    if (confirmed_service_data) {
        /* use maximum available APDU */
        *total_max = *apdu_max =
            min(confirmed_service_data->max_resp, MAX_APDU);
        /* segmented : compute maximum number of packets */
        if (confirmed_service_data->segmented_response_accepted) {
            maxsegments = confirmed_service_data->max_segs;
            /* if unspecified, try the maximum available, not just 2 segments */
            if (!maxsegments || maxsegments > 64) {
                maxsegments = MAX_SEGMENTS_ACCEPTED;
            }
            /* maximum size we are able to transmit */
            *total_max = min(maxsegments, MAX_SEGMENTS_ACCEPTED) * (*apdu_max);
        }
        return;
    }

    if (address_get_device_id(dest, &deviceId)) {
        if (address_get_by_device(
                deviceId, &max_apdu, &src, &segmentation, &maxsegments)) {
            /* Best possible APDU size */
            *total_max = *apdu_max = min(max_apdu, MAX_APDU);
            /* IIf device is able to receive segments */
            if (segmentation == SEGMENTATION_BOTH ||
                segmentation == SEGMENTATION_RECEIVE) {
                /* XXX - TODO: Number of segments accepted by peer device :
                   If zero segments we should fallback to 2 segments.
                   Or Maybe we just didn't ask the device about the maximum
                   segments supported.
                 */
                if (!maxsegments) {
                    maxsegments = MAX_SEGMENTS_ACCEPTED;
                }
                /* maximum size we are able to transmit */
                if (maxsegments) {
                    *total_max =
                        min(maxsegments, MAX_SEGMENTS_ACCEPTED) * (*apdu_max);
                }
            }
            return;
        }
    }
    *apdu_max = MAX_APDU;
    *total_max = *apdu_max * MAX_SEGMENTS_ACCEPTED;
}

/* room checks to prevent buffer overflows */
bool check_write_apdu_space(int apdu_len, int max_apdu, int space_needed)
{
    return (apdu_len + space_needed) < max_apdu;
}

/* send a packet to peer */
int tsm_pdu_send(BACNET_TSM_DATA *tsm_data, uint32_t segment_number)
{
    uint8_t Transmit_Buffer[MAX_PDU] = { 0 };
    BACNET_ADDRESS my_address;
    int len = 0;
    int pdu_len = 0;
    uint8_t *service_data = NULL;
    uint32_t service_len = 0;
    uint32_t total_segments = 0;

    /* Rebuild PDU */
    datalink_get_my_address(&my_address);
    len = npdu_encode_pdu(
        &Transmit_Buffer[pdu_len], &tsm_data->dest, &my_address,
        &tsm_data->npdu_data);
    if (len < 0) {
        return -1;
    }
    pdu_len += len;
    /* Header tweaks ! */
    total_segments = get_apdu_max_segments(tsm_data);
    /* Index out of bounds */
    if (segment_number >= total_segments) {
        return -1;
    }
    if (total_segments == 1) {
        tsm_data->apdu_fixed_header.service_data.common_data.segmented_message =
            false;
    } else {
        /* SEG */
        tsm_data->apdu_fixed_header.service_data.common_data.segmented_message =
            true;
        /* MORE */
        tsm_data->apdu_fixed_header.service_data.common_data.more_follows =
            (segment_number < total_segments - 1);
        /* Window size : do not modify */
        /* tsm_data->apdu_fixed_header.service_data.common_data.proposed_window_number
         * = 127; */
        /* SEQ# */
        tsm_data->apdu_fixed_header.service_data.common_data.sequence_number =
            segment_number;
    }
    /* Rebuild APDU Header */
    len = apdu_encode_fixed_header(
        &Transmit_Buffer[pdu_len], &tsm_data->apdu_fixed_header);
    if (len < 0) {
        return -1;
    }
    pdu_len += len;
    /* Rebuild APDU service data */
    /* gets Nth packet data */
    service_data =
        get_apdu_blob_data_segment(tsm_data, segment_number, &service_len);
    if (!service_data) { /* May be zero-size ! */
        return -1;
    }
    /* enough room ? */
    if (!check_write_apdu_space(
            pdu_len, sizeof(Transmit_Buffer), service_len)) {
        return -1;
    }
    memcpy(&Transmit_Buffer[pdu_len], service_data, service_len);
    pdu_len += service_len;
    return datalink_send_pdu(
        &tsm_data->dest, &tsm_data->npdu_data, &Transmit_Buffer[0],
        pdu_len);
}


/* Process and send segmented/unsegmented complex acknoweldegement based on the
response data length For unsegemented response, send the whole data For
segmented response, send the 1st segment of response data*/
int tsm_set_complexack_transaction(
    BACNET_ADDRESS *dest,
    BACNET_NPDU_DATA *npdu_data,
    BACNET_APDU_FIXED_HEADER *apdu_fixed_header,
    BACNET_CONFIRMED_SERVICE_DATA *confirmed_service_data,
    uint8_t *pdu,
    uint32_t pdu_len)
{
    uint8_t index;
    int bytes_sent;
    BACNET_TSM_DATA *tsm_data;
    uint32_t apdu_segments;
    uint8_t internal_service_id =
        tsm_get_peer_id(dest, confirmed_service_data->invoke_id);

    if (!internal_service_id) {
        /* failed : could not allocate enough slot for this transaction */
        abort_pdu_send(
            confirmed_service_data->invoke_id, dest,
            ABORT_REASON_PREEMPTED_BY_HIGHER_PRIORITY_TASK, true);
        return -1;
    }
    index = tsm_find_invokeID_index(internal_service_id);
    if (index >= MAX_TSM_TRANSACTIONS) { /* shall not fail */
        abort_pdu_send(
            confirmed_service_data->invoke_id, dest, ABORT_REASON_OTHER, true);
        return -1;
    }
    tsm_data = &TSM_List[index];

    /* Choice between a segmented or a non-segmented transaction */

    /* fill in maximum fill values */
    bacnet_calc_transmittable_length(
        dest, confirmed_service_data, &tsm_data->apdu_maximum_length,
        &tsm_data->maximum_transmittable_length);
    /* copy the apdu service data */
    copy_apdu_blob_data(tsm_data, &pdu[0], pdu_len);
    /* copy npdu data */
    npdu_copy_data(&tsm_data->npdu_data, npdu_data);
    /* copy apdu header data */
    tsm_data->apdu_fixed_header = *apdu_fixed_header;
    /* destination address */
    bacnet_address_copy(&tsm_data->dest, dest);
    /* absolute "retry" count : won't be reinitialized later */
    tsm_data->RetryCount = apdu_retries();

    tsm_data->ActualWindowSize = 1;
    tsm_data->ProposedWindowSize = DEFAULT_WINDOW_SIZE;
    tsm_data->InitialSequenceNumber = 0;
    tsm_data->SentAllSegments = false;

    /* Choice between a segmented or a non-segmented transaction */
    if (1 == (apdu_segments = get_apdu_max_segments(tsm_data))) {
        /* UNSEGMENTED MODE : Free transaction afterwards */
        bytes_sent = tsm_pdu_send(tsm_data, 0);
        if (bytes_sent > 0) {
            tsm_free_invoke_id_check(internal_service_id, dest, true);
        }
    } else {
        /* SEGMENTED-MODE */
        /* Take into account the fact that the APDU header is repeated on every
         * segment */
        if (pdu_len +
                apdu_segments *
                    get_apdu_header_typical_size(apdu_fixed_header, true) >
            tsm_data->maximum_transmittable_length) {
            /* Too much data : we cannot send that much, or the API cannot
             * receive that much ! */
           free_blob(&TSM_List[index]);
           /* Abort */
           abort_pdu_send(
               confirmed_service_data->invoke_id, dest,ABORT_REASON_BUFFER_OVERFLOW, true);
           bytes_sent = -2;
        } else {
            /* Window size proposal */
            tsm_data->apdu_fixed_header.service_data.common_data
                .proposed_window_number = tsm_data->ProposedWindowSize;
            /* assign the transaction */
            tsm_data->state = TSM_STATE_SEGMENTED_RESPONSE_SERVER;
            tsm_data->SegmentRetryCount = apdu_retries();
            /* start the timer */
            tsm_data->RequestTimer = 0;
            tsm_data->SegmentTimer = apdu_segment_timeout();
            /* Send first packet */
            bytes_sent = tsm_pdu_send(tsm_data, 0);
        }
    }
    /* If we cannot initiate, free transaction so we don't wait on a timeout to
       realize it has failed. Caller don't free invoke ID : we must clear it
       now. */
    if (bytes_sent <= 0) {
        tsm_free_invoke_id_check(internal_service_id, dest, true);
    }
    return bytes_sent;
}

/* Sends PDU segments either until the window is full or
 until the last segment of a message has been sent.*/
void FillWindow(BACNET_TSM_DATA *tsm_data, uint32_t sequence_number)
{
    uint32_t ix;
    uint32_t total_segments = get_apdu_max_segments(tsm_data);
    for (ix = 0; (ix < tsm_data->ActualWindowSize) &&
         (sequence_number + ix < total_segments);
         ix++) {
        tsm_pdu_send(tsm_data, sequence_number + ix);
    }
    /* sent all segments ? */
    if (ix + sequence_number >= total_segments) {
        tsm_data->SentAllSegments = true;
    }
}

bool InWindow(BACNET_TSM_DATA *data, uint8_t seqA, uint8_t seqB)
{
    uint8_t requiredWindowSize = seqA - seqB;
    return requiredWindowSize < data->ActualWindowSize;
}

/*Process the received segment ack and send the next segment accordingly if
 * available*/
void tsm_segmentack_received(
    uint8_t invoke_id,
    uint8_t sequence_number,
    uint8_t actual_window_size,
    bool nak,
    bool server,
    BACNET_ADDRESS *src)
{
    uint8_t index;
    uint32_t big_segment_number;
    uint8_t window;
    bool some_segment_remains;
    BACNET_TSM_INDIRECT_DATA *peer_data;

    (void)nak;

    /* bad invoke number from server peer (we never use 0) */
    if (server && !invoke_id) {
        return;
    }
    /* Peer invoke id number : translate to our internal numbers */
    if (!server) {
        peer_data = tsm_get_peer_id_data(src, invoke_id, false);
        if (!peer_data) {
            /* failed : unknown message */
            return;
        }
        /* now we use our internal number */
        invoke_id = peer_data->InternalInvokeID;
    }
    /* Find an active TSM slot that matches the Segment-Ack */
    index = tsm_find_invokeID_index(invoke_id);
    if (index >= MAX_TSM_TRANSACTIONS) {
        return;
    }

    /* Almost the same code for segment handling between segmented requests and
     * responses */
    if (!server &&
        TSM_List[index].state == TSM_STATE_SEGMENTED_RESPONSE_SERVER) {
        /* DuplicateAck_Received */
        if (!InWindow(
                &TSM_List[index], sequence_number,
                TSM_List[index].InitialSequenceNumber)) {
            /* Restart timer */
            TSM_List[index].SegmentTimer = apdu_segment_timeout();
        } else {
            /* total segment number (not modulo 256) */
            window = sequence_number -
                (uint8_t)TSM_List[index].InitialSequenceNumber;
            big_segment_number = TSM_List[index].InitialSequenceNumber + window;

            /* 1..N segment number < number of segments ? */
            some_segment_remains = (big_segment_number + 1) <
                get_apdu_max_segments(&TSM_List[index]);
            if (some_segment_remains) {
                /* NewAck_Received : do we have a segment remaining to send */
                TSM_List[index].InitialSequenceNumber = big_segment_number + 1;
                TSM_List[index].ActualWindowSize = actual_window_size;
                TSM_List[index].SegmentRetryCount = apdu_retries();
                TSM_List[index].SegmentTimer = apdu_segment_timeout();
                FillWindow(
                    &TSM_List[index], TSM_List[index].InitialSequenceNumber);
                TSM_List[index].SegmentTimer = apdu_segment_timeout();
            } else {
                /* FinalAck_Received */
                TSM_List[index].SegmentTimer = 0;
                if (TSM_List[index].state ==
                    TSM_STATE_SEGMENTED_RESPONSE_SERVER) {
                    /* Response : end communications */
                    /* Release data */
                    TSM_List[index].state = TSM_STATE_IDLE;
                    /* Completely free data */
                    tsm_free_invoke_id_check(invoke_id, NULL, true);
                } else {
                    /* Request : Wait confirmation */
                    TSM_List[index].RequestTimer = apdu_timeout();
                    TSM_List[index].state = TSM_STATE_AWAIT_CONFIRMATION;
                }
            }
        }
    } else {
        /* UnexpectedPDU_Received */
        /* Release data */
        free_blob(&TSM_List[index]);
        /* Abort */
        abort_pdu_send(
            invoke_id, src,
            ABORT_REASON_INVALID_APDU_IN_THIS_STATE, true);
        /* We must free invoke_id ! */
        tsm_free_invoke_id_check(invoke_id, NULL, true);
    }
}

/* Check unexpected PDU is received in active TSM state other than idle state for server */
bool check_unexpected_pdu_received(
    BACNET_ADDRESS *src, 
    BACNET_CONFIRMED_SERVICE_DATA *service_data )
{
    uint8_t index = 0;
    bool status = false;
    BACNET_TSM_INDIRECT_DATA *peer_data;
    peer_data = tsm_get_peer_id_data(src, service_data->invoke_id, false);
    if (peer_data) {
        index = tsm_find_invokeID_index(peer_data->InternalInvokeID);
        if (index < MAX_TSM_TRANSACTIONS) {
            BACNET_TSM_DATA *plist = &TSM_List[index];
            if ((plist->state == TSM_STATE_SEGMENTED_RESPONSE_SERVER) ||
                (plist->state == TSM_STATE_SEGMENTED_REQUEST_SERVER)) {
                abort_pdu_send(
                    service_data->invoke_id, src,
                    ABORT_REASON_INVALID_APDU_IN_THIS_STATE, true);
                tsm_free_invoke_id_check(plist->InvokeID, src, true);
                status = true;
            }
        }
    }
    return status;
}

/*frees the invokeID for segemented messages */
void tsm_free_invoke_id_segmentation(
    BACNET_ADDRESS* src,
    uint8_t invoke_id)
{
    uint8_t peer_id = 0;
    peer_id = tsm_get_peer_id(src, invoke_id);
    tsm_free_invoke_id_check(peer_id, src, true);
}
#endif

/** Called once a millisecond or slower.
 *  This function calls the handler for a
 *  timeout 'Timeout_Function', if necessary.
 *  Here, Stack is updated only to support segmentation for Server and implementing only two states
 *  TSM_STATE_SEGMENTED_RESPONSE_SERVER and TSM_STATE_SEGMENTED_REQUEST_SERVER
 *  Client segmentation is not updated.
 *
 * @param milliseconds - Count of milliseconds passed, since the last call.
 */
void tsm_timer_milliseconds(uint16_t milliseconds)
{
    unsigned i = 0; /* counter */
    for (i = 0; i < MAX_TSM_TRANSACTIONS; i++) {
        BACNET_TSM_DATA *plist = &TSM_List[i];
        if (plist->state == TSM_STATE_AWAIT_CONFIRMATION) {
            if (plist->RequestTimer > milliseconds) {
                plist->RequestTimer -= milliseconds;
            } else {
                plist->RequestTimer = 0;
            }
            /* AWAIT_CONFIRMATION */
            if (plist->RequestTimer == 0) {
                if (plist->RetryCount < apdu_retries()) {
                    plist->RequestTimer = apdu_timeout();
                    plist->RetryCount++;
                    datalink_send_pdu(
                        &plist->dest, &plist->npdu_data, &plist->apdu[0],
                        plist->apdu_len);
                } else {
                    /* note: the invoke id has not been cleared yet
                       and this indicates a failed message:
                       IDLE and a valid invoke id */
                    plist->state = TSM_STATE_IDLE;
                    if (plist->InvokeID != 0) {
                        if (Timeout_Function) {
                            Timeout_Function(plist->InvokeID);
                        }
                    }
                }
            }
        }

#if BACNET_SEGMENTATION_ENABLED
        if (plist->state == TSM_STATE_SEGMENTED_RESPONSE_SERVER) {
            /* RequestTimer stopped in this state */
            if (plist->SegmentTimer > milliseconds) {
                plist->SegmentTimer -= milliseconds;
            } else {
                plist->SegmentTimer = 0;
            }
            /* timeout.  retry? */
            if (plist->SegmentTimer == 0) {
                plist->SegmentRetryCount--;
                plist->SegmentTimer = apdu_segment_timeout();
                if (plist->SegmentRetryCount) {
                    /* Re-send PDU data */
                    FillWindow(&TSM_List[i], plist->InitialSequenceNumber);
                } else {
                    /* note: the invoke id has not been cleared yet
                       and this indicates a failed message:
                       IDLE and a valid invoke id */
                    plist->state = TSM_STATE_IDLE;
                }
            }
        }

        if (plist->state == TSM_STATE_SEGMENTED_REQUEST_SERVER) {
            /* RequestTimer stopped in this state */
            if (plist->SegmentTimer > milliseconds) {
                plist->SegmentTimer -= milliseconds;
            } else {
                plist->SegmentTimer = 0;
            }
            /* timeout : free memory */
            if (plist->SegmentTimer == 0) {
                /* Clear Peer data, if any. Lookup with our internal ID
                status. */
                tsm_clear_peer_id(plist->InvokeID);
                /* Release segmented data */
                free_blob(&TSM_List[i]);

                /* flag slot as "unused" */
                plist->InvokeID = 0;
                /* free all memory associated */
                plist->state = TSM_STATE_IDLE;
            }
        }
#endif
    }
}
#endif
