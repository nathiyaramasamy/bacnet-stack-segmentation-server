/**
 * @file
 * @brief BACnet ReadPropertyMultiple-Request handler
 * @author Steve Karg <skarg@users.sourceforge.net>
 * @author John Stachler <John.Stachler@lennoxind.com>
 * @author Peter McShane <petermcs@users.sourceforge.net>
 * @author Roy Schneider <postmaster@overthehill.de>
 * @date 2007
 * @copyright SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
/* BACnet Stack defines - first */
#include "bacnet/bacdef.h"
/* BACnet Stack API */
#include "bacnet/memcopy.h"
#include "bacnet/bacdcode.h"
#include "bacnet/apdu.h"
#include "bacnet/npdu.h"
#include "bacnet/abort.h"
#include "bacnet/reject.h"
#include "bacnet/bacerror.h"
#include "bacnet/rpm.h"
/* basic objects, services, TSM, and datalink */
#include "bacnet/basic/object/device.h"
#if (BACNET_PROTOCOL_REVISION >= 17)
#include "bacnet/basic/object/netport.h"
#endif
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/basic/services.h"
#include "bacnet/basic/sys/debug.h"
#include "bacnet/datalink/datalink.h"

/**
 * @brief Fetches the lists of properties (array of BACNET_PROPERTY_ID's) for
 * this object type and the special properties ALL or REQUIRED or OPTIONAL.
 * @param pPropertyList reference for the list of ALL, REQUIRED, and OPTIONAL
 * properties.
 * @param special_property The special property ALL, REQUIRED, or OPTIONAL
 * to fetch.
 * @param index The index of the property to fetch.
 * @return The property ID or -1 if not found.
 */
static BACNET_PROPERTY_ID RPM_Object_Property(
    struct special_property_list_t *pPropertyList,
    BACNET_PROPERTY_ID special_property,
    unsigned index)
{
    int property = -1; /* return value */
    unsigned required, optional, proprietary;

    required = pPropertyList->Required.count;
    optional = pPropertyList->Optional.count;
    proprietary = pPropertyList->Proprietary.count;
    if (special_property == PROP_ALL) {
        if (index < required) {
            property = pPropertyList->Required.pList[index];
        } else if (index < (required + optional)) {
            index -= required;
            property = pPropertyList->Optional.pList[index];
        } else if (index < (required + optional + proprietary)) {
            index -= (required + optional);
            property = pPropertyList->Proprietary.pList[index];
        }
    } else if (special_property == PROP_REQUIRED) {
        if (index < required) {
            property = pPropertyList->Required.pList[index];
        }
    } else if (special_property == PROP_OPTIONAL) {
        if (index < optional) {
            property = pPropertyList->Optional.pList[index];
        }
    }

    return (BACNET_PROPERTY_ID)property;
}

/**
 * @brief Fetches the number of properties (array of BACNET_PROPERTY_ID's) for
 * this object type belonging to the special properties ALL or REQUIRED or
 * OPTIONAL.
 * @param pPropertyList reference for the list of ALL, REQUIRED, and OPTIONAL
 * properties.
 * @param special_property The special property ALL, REQUIRED, or OPTIONAL
 * to fetch.
 * @return The number of properties.
 */
static unsigned RPM_Object_Property_Count(
    struct special_property_list_t *pPropertyList,
    BACNET_PROPERTY_ID special_property)
{
    unsigned count = 0; /* return value */

    if (special_property == PROP_ALL) {
        count = pPropertyList->Required.count + pPropertyList->Optional.count +
            pPropertyList->Proprietary.count;
    } else if (special_property == PROP_REQUIRED) {
        count = pPropertyList->Required.count;
    } else if (special_property == PROP_OPTIONAL) {
        count = pPropertyList->Optional.count;
    }

    return count;
}

/**
 * @brief Encode the RPM property returning the length of the encoding,
 * or 0 if there is no room to fit the encoding.
 * @param apdu [out] The buffer to encode the property into.
 * @param offset [in] The offset into the buffer to start encoding.
 * @param max_apdu [in] The maximum length of the buffer.
 * @param rpmdata [in] The RPM data to encode.
 * @return The length of the encoding, or 0 if there is no room to fit the
 * encoding.
 */
static int RPM_Encode_Property(
    uint8_t *apdu, uint16_t offset, uint16_t max_apdu, BACNET_RPM_DATA *rpmdata)
{
    int len = 0;
    size_t copy_len = 0;
    int apdu_len = 0;
    BACNET_READ_PROPERTY_DATA rpdata;
    uint8_t Temp_Buf[MAX_APDU];

    len = rpm_ack_encode_apdu_object_property(
        &Temp_Buf[0], rpmdata->object_property, rpmdata->array_index);
    copy_len = memcopy(&apdu[0], &Temp_Buf[0], offset, len, max_apdu);
    if (copy_len == 0) {
        rpmdata->error_code = ERROR_CODE_ABORT_SEGMENTATION_NOT_SUPPORTED;
        return BACNET_STATUS_ABORT;
    }
    apdu_len += len;
    rpdata.error_class = ERROR_CLASS_OBJECT;
    rpdata.error_code = ERROR_CODE_UNKNOWN_OBJECT;
    rpdata.object_type = rpmdata->object_type;
    rpdata.object_instance = rpmdata->object_instance;
    rpdata.object_property = rpmdata->object_property;
    rpdata.array_index = rpmdata->array_index;
    rpdata.application_data = &Temp_Buf[0];
    rpdata.application_data_len = sizeof(Temp_Buf);

    if ((rpmdata->object_property == PROP_ALL) ||
        (rpmdata->object_property == PROP_REQUIRED) ||
        (rpmdata->object_property == PROP_OPTIONAL)) {
        /* special properties only get ERROR encoding */
        len = BACNET_STATUS_ERROR;
    } else if (!read_property_bacnet_array_valid(&rpdata)) {
        len = BACNET_STATUS_ERROR;
    } else {
        len = Device_Read_Property(&rpdata);
    }

    if (len < 0) {
        if ((len == BACNET_STATUS_ABORT) || (len == BACNET_STATUS_REJECT)) {
            rpmdata->error_code = rpdata.error_code;
            /* pass along aborts and rejects for now */
            return len; /* Ie, Abort */
        }
        /* error was returned - encode that for the response */
        len = rpm_ack_encode_apdu_object_property_error(
            &Temp_Buf[0], rpdata.error_class, rpdata.error_code);
        copy_len =
            memcopy(&apdu[0], &Temp_Buf[0], offset + apdu_len, len, max_apdu);

        if (copy_len == 0) {
            rpmdata->error_code = ERROR_CODE_ABORT_SEGMENTATION_NOT_SUPPORTED;
            return BACNET_STATUS_ABORT;
        }
    } else if ((offset + apdu_len + 1 + len + 1) < max_apdu) {
        /* enough room to fit the property value and tags */
        len = rpm_ack_encode_apdu_object_property_value(
            &apdu[offset + apdu_len], &Temp_Buf[0], len);
    } else {
        /* not enough room - abort! */
        rpmdata->error_code = ERROR_CODE_ABORT_SEGMENTATION_NOT_SUPPORTED;
        return BACNET_STATUS_ABORT;
    }
    apdu_len += len;

    return apdu_len;
}

/** Handler for a ReadPropertyMultiple Service request.
 * @ingroup DSRPM
 * This handler will be invoked by apdu_handler() if it has been enabled
 * by a call to apdu_set_confirmed_handler().
 * This handler builds a response packet, which is
 * - an Abort if
 *   - the message is segmented, when BACNET_SEGMENTATION_ENABLED is OFF(SEGMENTATION_NONE)
 *   - if decoding fails
 * - the result from each included read request, if it succeeds
 * - an Error if processing fails for all, or individual errors if only some
 * fail, or there isn't enough room in the APDU to fit the data.
 *
 * @param service_request [in] The contents of the service request.
 * @param service_len [in] The length of the service_request.
 * @param src [in] BACNET_ADDRESS of the source of the message
 * @param service_data [in] The BACNET_CONFIRMED_SERVICE_DATA information
 *                          decoded from the APDU header of this message.
 */
void handler_read_property_multiple(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src,
    BACNET_CONFIRMED_SERVICE_DATA *service_data)
{
    bool berror = false;
    int len = 0;
    uint16_t copy_len = 0;
    uint16_t decode_len = 0;
    int pdu_len = 0;
    BACNET_NPDU_DATA npdu_data;
    int bytes_sent;
    BACNET_ADDRESS my_address;
    BACNET_RPM_DATA rpmdata;
    int apdu_len = 0;
    int npdu_len = 0;
    int error = 0;
    int max_apdu_len = 0;
    BACNET_APDU_FIXED_HEADER apdu_fixed_header;
    int apdu_header_len = 3;
    int sizeOfBuffer = MAX_PDU - MAX_NPDU;
    uint8_t Temp_Buf_rpm[MAX_PDU - MAX_NPDU];

    if (service_data) {
        datalink_get_my_address(&my_address);
        npdu_encode_npdu_data(&npdu_data, false, service_data->priority);
        npdu_len = npdu_encode_pdu(
            &Handler_Transmit_Buffer[0], src, &my_address, &npdu_data);
        if (service_len == 0) {
            rpmdata.error_code = ERROR_CODE_REJECT_MISSING_REQUIRED_PARAMETER;
            error = BACNET_STATUS_REJECT;
            debug_print("RPM: Missing Required Parameter. Sending Reject!\n");
#if !BACNET_SEGMENTATION_ENABLED
        } else if (service_data->segmented_message) {
            rpmdata.error_code = ERROR_CODE_ABORT_SEGMENTATION_NOT_SUPPORTED;
            error = BACNET_STATUS_ABORT;
            debug_print("RPM: Segmented message. Sending Abort!\r\n");
#endif
        } else {
            /* decode apdu request & encode apdu reply
               encode complex ack, invoke id, service choice */
            apdu_len = rpm_ack_encode_apdu_init(
                &Handler_Transmit_Buffer[npdu_len], service_data->invoke_id);

            for (;;) {
                /* Start by looking for an object ID */
                len = rpm_decode_object_id(
                    &service_request[decode_len], service_len - decode_len,
                    &rpmdata);
                if (len >= 0) {
                    /* Got one so skip to next stage */
                    decode_len += len;
                } else {
                    /* bad encoding - skip to error/reject/abort handling */
                    debug_print("RPM: Bad Encoding.\n");
                    error = len;
                    /* The berror flag ensures that
                        both loops will be broken! */
                    berror = true;
                    break;
                }

                /* Test for case of indefinite Device object instance */
                if ((rpmdata.object_type == OBJECT_DEVICE) &&
                    (rpmdata.object_instance == BACNET_MAX_INSTANCE)) {
                    rpmdata.object_instance = Device_Object_Instance_Number();
                }
#if (BACNET_PROTOCOL_REVISION >= 17)
                /* When the object-type in the Object Identifier parameter
                   contains the value NETWORK_PORT and the instance in the
                   'Object Identifier' parameter contains the value 4194303,
                   the responding BACnet-user shall treat the Object Identifier
                   as if it correctly matched the local Network Port object
                   representing the network port through which the request was
                   received. This allows the network port instance of the
                   network port that was used to receive the request to be
                   determined. */
                if ((rpmdata.object_type == OBJECT_NETWORK_PORT) &&
                    (rpmdata.object_instance == BACNET_MAX_INSTANCE)) {
                    rpmdata.object_instance = Network_Port_Index_To_Instance(0);
                }
#endif
                /* Stick this object id into the reply - if it will fit */
                len = rpm_ack_encode_apdu_object_begin(&Temp_Buf_rpm[0], &rpmdata);
                copy_len = memcopy(
                    &Handler_Transmit_Buffer[npdu_len], &Temp_Buf_rpm[0], apdu_len,
                    len, sizeOfBuffer);
                if (copy_len == 0) {
                    debug_print("RPM: Response too big!\n");
#if !BACNET_SEGMENTATION_ENABLED
                    rpmdata.error_code =
                        ERROR_CODE_ABORT_SEGMENTATION_NOT_SUPPORTED;
#else
                    rpmdata.error_code =
                        ERROR_CODE_ABORT_BUFFER_OVERFLOW;
#endif
                    error = BACNET_STATUS_ABORT;
                    berror = true;
                    break;
                }
                apdu_len += copy_len;
                /* do each property of this object of the RPM request */
                for (;;) {
                    /* Fetch a property */
                    len = rpm_decode_object_property(
                        &service_request[decode_len], service_len - decode_len,
                        &rpmdata);
                    if (len < 0) {
                        /* bad encoding - skip to error/reject/abort handling */
                        debug_print("RPM: Bad Encoding.\n");
                        error = len;
                        /* The berror flag ensures that
                            both loops will be broken! */
                        berror = true;
                        break;
                    }
                    decode_len += len;
                    /* handle the special properties */
                    if ((rpmdata.object_property == PROP_ALL) ||
                        (rpmdata.object_property == PROP_REQUIRED) ||
                        (rpmdata.object_property == PROP_OPTIONAL)) {
                        struct special_property_list_t property_list;
                        unsigned property_count = 0;
                        unsigned index = 0;
                        BACNET_PROPERTY_ID special_object_property;

                        if (!Device_Valid_Object_Id(
                                rpmdata.object_type, rpmdata.object_instance)) {
                            len = RPM_Encode_Property(
                                &Handler_Transmit_Buffer[npdu_len],
                                (uint16_t)apdu_len, sizeOfBuffer, &rpmdata);
                            if (len > 0) {
                                apdu_len += len;
                            } else {
                                debug_print("RPM: Too full for property!\n");
                                error = len;
                                /* The berror flag ensures that
                                   both loops will be broken! */
                                berror = true;
                                break;
                            }
                        } else if (rpmdata.array_index != BACNET_ARRAY_ALL) {
                            /* No array index options for this special property.
                               Encode error for this object property response */
                            len = rpm_ack_encode_apdu_object_property(
                                &Temp_Buf_rpm[0], rpmdata.object_property,
                                rpmdata.array_index);

                            copy_len = memcopy(
                                &Handler_Transmit_Buffer[npdu_len],
                                &Temp_Buf_rpm[0], apdu_len, len, sizeOfBuffer);

                            if (copy_len == 0) {
                                debug_print(
                                    "RPM: Too full to encode property!\n");
#if !BACNET_SEGMENTATION_ENABLED
                                rpmdata.error_code =
                                    ERROR_CODE_ABORT_SEGMENTATION_NOT_SUPPORTED;
#else
                                rpmdata.error_code =
                                    ERROR_CODE_ABORT_BUFFER_OVERFLOW;
#endif
                                error = BACNET_STATUS_ABORT;
                                /* The berror flag ensures that
                                   both loops will be broken! */
                                berror = true;
                                break;
                            }

                            apdu_len += len;
                            len = rpm_ack_encode_apdu_object_property_error(
                                &Temp_Buf_rpm[0], ERROR_CLASS_PROPERTY,
                                ERROR_CODE_PROPERTY_IS_NOT_AN_ARRAY);

                            copy_len = memcopy(
                                &Handler_Transmit_Buffer[npdu_len],
                                &Temp_Buf_rpm[0], apdu_len, len, sizeOfBuffer);

                            if (copy_len == 0) {
                                debug_print("RPM: Too full to encode error!\n");
#if !BACNET_SEGMENTATION_ENABLED
                                rpmdata.error_code =
                                    ERROR_CODE_ABORT_SEGMENTATION_NOT_SUPPORTED;
#else
                                rpmdata.error_code =
                                    ERROR_CODE_ABORT_BUFFER_OVERFLOW;
#endif
                                error = BACNET_STATUS_ABORT;
                                /* The berror flag ensures that
                                   both loops will be broken! */
                                berror = true;
                                break;
                            }
                            apdu_len += len;
                        } else {
                            special_object_property = rpmdata.object_property;
                            Device_Objects_Property_List(
                                rpmdata.object_type, rpmdata.object_instance,
                                &property_list);
                            property_count = RPM_Object_Property_Count(
                                &property_list, special_object_property);

                            if (property_count == 0) {
                                /* Only happens with the OPTIONAL property */
                                /* 135-2016bl-2. Clarify ReadPropertyMultiple
                                   response on OPTIONAL when empty. */
                                /* If no optional properties are supported then
                                   an empty 'List of Results' shall be returned
                                   for the specified property, except if the
                                   object does not exist. */
                                if (!Device_Valid_Object_Id(
                                        rpmdata.object_type,
                                        rpmdata.object_instance)) {
                                    len = RPM_Encode_Property(
                                        &Handler_Transmit_Buffer[npdu_len],
                                        (uint16_t)apdu_len, sizeOfBuffer, &rpmdata);
                                    if (len > 0) {
                                        apdu_len += len;
                                    } else {
                                        debug_print(
                                            "RPM: Too full for property!\n");
                                        error = len;
                                        /* The berror flag ensures that
                                           both loops will be broken! */
                                        berror = true;
                                        break;
                                    }
                                }
                            } else {
                                for (index = 0; index < property_count;
                                     index++) {
                                    rpmdata.object_property =
                                        RPM_Object_Property(
                                            &property_list,
                                            special_object_property, index);
                                    len = RPM_Encode_Property(
                                        &Handler_Transmit_Buffer[npdu_len],
                                        (uint16_t)apdu_len, sizeOfBuffer, &rpmdata);
                                    if (len > 0) {
                                        apdu_len += len;
                                    } else {
                                        debug_print(
                                            "RPM: Too full for property!\n");
                                        error = len;
                                        /* The berror flag ensures that
                                           both loops will be broken! */
                                        berror = true;
                                        break;
                                    }
                                }
                            }
                        }
                    } else {
                        /* handle an individual property */
                        len = RPM_Encode_Property(
                            &Handler_Transmit_Buffer[npdu_len],
                            (uint16_t)apdu_len, sizeOfBuffer, &rpmdata);
                        if (len > 0) {
                            apdu_len += len;
                        } else {
                            debug_print(
                                "RPM: Too full for individual property!\n");
                            error = len;
                            /* The berror flag ensures that
                               both loops will be broken! */
                            berror = true;
                            break;
                        }
                    }

                    if (decode_is_closing_tag_number(
                            &service_request[decode_len], 1)) {
                        /* Reached end of property list so cap the result list
                         */
                        decode_len++;
                        len = rpm_ack_encode_apdu_object_end(&Temp_Buf_rpm[0]);
                        copy_len = memcopy(
                            &Handler_Transmit_Buffer[npdu_len], &Temp_Buf_rpm[0],
                            apdu_len, len, sizeOfBuffer);
                        if (copy_len == 0) {
                            debug_print(
                                "RPM: Too full to encode object end!\n");
#if !BACNET_SEGMENTATION_ENABLED
                            rpmdata.error_code =
                                ERROR_CODE_ABORT_SEGMENTATION_NOT_SUPPORTED;
#else
                            rpmdata.error_code =
                                ERROR_CODE_ABORT_BUFFER_OVERFLOW;
#endif
                            error = BACNET_STATUS_ABORT;
                            /* The berror flag ensures that
                               both loops will be broken! */
                            berror = true;
                            break;
                        } else {
                            apdu_len += copy_len;
                        }
                        /* finished with this property list */
                        break;
                    }
                }
                if (berror) {
                    break;
                }
                if (decode_len >= service_len) {
                    /* Reached the end so finish up */
                    break;
                }
            }
            /* If not having an error so far, check the remaining space. */
            if (!berror) {
                max_apdu_len =  service_data->max_resp < MAX_APDU ? service_data->max_resp : MAX_APDU;  //TODO: Danfoss Modification
                if (apdu_len >  max_apdu_len)
                {
#if BACNET_SEGMENTATION_ENABLED
                    if (service_data->segmented_response_accepted) {
                        apdu_init_fixed_header(
                            &apdu_fixed_header, PDU_TYPE_COMPLEX_ACK,
                            service_data->invoke_id,
                            SERVICE_CONFIRMED_READ_PROP_MULTIPLE,
                            service_data->max_resp);

                        npdu_encode_npdu_data(
                            &npdu_data, true, MESSAGE_PRIORITY_NORMAL);
                        npdu_len = npdu_encode_pdu(
                            &Handler_Transmit_Buffer[0], src, &my_address,
                            &npdu_data);

                        tsm_set_complexack_transaction(
                            src, &npdu_data, &apdu_fixed_header, service_data,
                            &Handler_Transmit_Buffer
                                [npdu_len + apdu_header_len],
                            (apdu_len - apdu_header_len));
                        error = false;
                        return;
                    } else {
                        // segmented response not accepted by the client
                        rpmdata.error_code =
                            ERROR_CODE_ABORT_SEGMENTATION_NOT_SUPPORTED;
                        error = BACNET_STATUS_ABORT;
                    }
#else
                    /* too big for the sender - send an abort */
                    rpmdata.error_code =
                        ERROR_CODE_ABORT_SEGMENTATION_NOT_SUPPORTED;
                    error = BACNET_STATUS_ABORT;
                    debug_print("RPM: Message too large.  Sending Abort!\n");
#endif
                }
            }
        }
        /* Error fallback. */
        if (error) {
            if (error == BACNET_STATUS_ABORT) {
                apdu_len = abort_encode_apdu(
                    &Handler_Transmit_Buffer[npdu_len], service_data->invoke_id,
                    abort_convert_error_code(rpmdata.error_code), true);
                debug_print("RPM: Sending Abort!\n");
            } else if (error == BACNET_STATUS_ERROR) {
                apdu_len = bacerror_encode_apdu(
                    &Handler_Transmit_Buffer[npdu_len], service_data->invoke_id,
                    SERVICE_CONFIRMED_READ_PROP_MULTIPLE, rpmdata.error_class,
                    rpmdata.error_code);
                debug_print("RPM: Sending Error!\n");
            } else if (error == BACNET_STATUS_REJECT) {
                apdu_len = reject_encode_apdu(
                    &Handler_Transmit_Buffer[npdu_len], service_data->invoke_id,
                    reject_convert_error_code(rpmdata.error_code));
                debug_print("RPM: Sending Reject!\n");
            }
        }
        pdu_len = apdu_len + npdu_len;
        bytes_sent = datalink_send_pdu(
            src, &npdu_data, &Handler_Transmit_Buffer[0], pdu_len);
        if (bytes_sent <= 0) {
            debug_perror("RPM: Failed to send PDU");
        }
    }
}
