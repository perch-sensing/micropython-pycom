/*
 * Copyright (c) 2020, Pycom Limited.
 *
 * This software is licensed under the GNU GPL version 3 or any
 * later version, with permitted additional terms. For more information
 * see the Pycom Licence v1.0 document supplied with this file, or
 * available at https://www.pycom.io/opensource/licensing
 */


#include "py/mpconfig.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "coap.h"
#include "coap_hashkey.h"

#include "modcoap.h"
#include "modnetwork.h"
#include "modusocket.h"
#include "lwipsocket.h"
#include "netutils.h"
#include "mpirq.h"

#include "freertos/semphr.h"

/******************************************************************************
 DEFINE CONSTANTS
 ******************************************************************************/

#define MODCOAP_IP4_MULTICAST        ("224.0.1.187")
#define MODCOAP_REQUEST_GET          (0x01)
#define MODCOAP_REQUEST_PUT          (0x02)
#define MODCOAP_REQUEST_POST         (0x04)
#define MODCOAP_REQUEST_DELETE       (0x08)
#define MODCOAP_TASK_STACK_SIZE      (5 * 1024)
#define MODCOAP_TASK_PRIORITY        (5)

/******************************************************************************
 DEFINE PRIVATE TYPES
 ******************************************************************************/

typedef struct mod_coap_response_handler_args_s {
    uint8_t code;
    uint16_t tid;
    uint8_t type;
    uint8_t* token;
    uint8_t token_length;
    uint8_t* data;
    uint16_t data_length;
}mod_coap_response_handler_args_t;

typedef struct mod_coap_resource_obj_s {
    mp_obj_base_t base;
    coap_resource_t* coap_resource;
    struct mod_coap_resource_obj_s* next;
    uint8_t* value;
    int32_t mediatype;
    int32_t max_age;
    uint16_t etag_value;
    uint16_t value_len;
    bool etag;
}mod_coap_resource_obj_t;

typedef struct mod_coap_client_session_obj_s {
    mp_obj_base_t base;
    struct mod_coap_client_session_obj_s* next;
    coap_session_t* session;
    mp_obj_t ip_addr;
    mp_obj_t port;
    mp_obj_t protocol;
}mod_coap_client_session_obj_t;

typedef struct mod_coap_obj_s {
    mp_obj_base_t base;
    coap_context_t* context;
    mod_coap_resource_obj_t* resources;
    mod_coap_client_session_obj_t* client_sessions;
    SemaphoreHandle_t semphr;
    mp_obj_t callback_response;
    mp_obj_t callback_new_resource;
}mod_coap_obj_t;



/******************************************************************************
 DECLARE PRIVATE FUNCTIONS
 ******************************************************************************/
STATIC mod_coap_client_session_obj_t* new_client_session(mp_obj_t ip_addr_in, mp_obj_t port_in, mp_obj_t key_in, mp_obj_t identity_in);
STATIC mod_coap_resource_obj_t* find_resource_by_uri(coap_str_const_t *uri_path);
STATIC mod_coap_resource_obj_t* add_resource(const char* uri, int32_t mediatype, int32_t max_age, mp_obj_t value, bool etag);
STATIC void remove_resource_by_uri(coap_str_const_t *uri_path);
STATIC void remove_resource(const char* uri);
STATIC void resource_update_value(mod_coap_resource_obj_t* resource, mp_obj_t new_value);
STATIC void coap_response_new_resource_handler_micropython(void* arg);
STATIC void TASK_MODCOAP (void *pvParameters);



STATIC void coap_resource_callback_get(coap_context_t * context,
                                       struct coap_resource_t * resource,
                                       coap_session_t * session,
                                       coap_pdu_t * request,
                                       coap_binary_t * token,
                                       coap_string_t * query_string,
                                       coap_pdu_t * response);

STATIC void coap_resource_callback_put(coap_context_t * context,
                                       struct coap_resource_t * resource,
                                       coap_session_t * session,
                                       coap_pdu_t * request,
                                       coap_binary_t * token,
                                       coap_string_t * query_string,
                                       coap_pdu_t * response);

STATIC void coap_resource_callback_post(coap_context_t * context,
                                        struct coap_resource_t * resource,
                                        coap_session_t * session,
                                        coap_pdu_t * request,
                                        coap_binary_t * token,
                                        coap_string_t * query_string,
                                        coap_pdu_t * response);

STATIC void coap_resource_callback_delete(coap_context_t * context,
                                          struct coap_resource_t * resource,
                                          coap_session_t * session,
                                          coap_pdu_t * request,
                                          coap_binary_t * token,
                                          coap_string_t * query_string,
                                          coap_pdu_t * response);

STATIC void coap_response_handler(struct coap_context_t *context,
                                  coap_session_t *session,
                                  coap_pdu_t *sent,
                                  coap_pdu_t *received,
                                  const coap_tid_t id);

STATIC coap_pdu_t * modcoap_new_request(coap_context_t *ctx,
                                        coap_session_t* session,
                                        unsigned int m,
                                        coap_optlist_t **options,
                                        const char* token,
                                        size_t token_length,
                                        const char *data,
                                        size_t length);
/******************************************************************************
 DEFINE PRIVATE VARIABLES
 ******************************************************************************/
STATIC const mp_obj_type_t mod_coap_resource_type;
STATIC const mp_obj_type_t mod_coap_client_session_type;
// Only 1 context is supported
STATIC mod_coap_obj_t* coap_obj_ptr;
STATIC bool initialized = false;
STATIC TaskHandle_t ModCoapTaskHandle;


/******************************************************************************
 DEFINE PRIVATE FUNCTIONS
 ******************************************************************************/

// This task handles the periodic and message reception functionality of the underlying esp-idf coap library
STATIC void TASK_MODCOAP (void *pvParameters) {
    while(1){
        // This function returns when there is a new packet arrived or after 100ms
        // Re-calling this function minimally after every 100ms is needed to perform periodic activities (e.g. retransmission of Confirmable packets)
        coap_run_once(coap_obj_ptr->context, 100);
    }
}

// Create a new client session in the scope of the only context
STATIC mod_coap_client_session_obj_t* new_client_session(mp_obj_t ip_addr_in, mp_obj_t port_in, mp_obj_t key_in, mp_obj_t identity_in) {

    // Currently only 1 context is supported
    mod_coap_obj_t* context = coap_obj_ptr;

    // Create a new Client Session object
    mod_coap_client_session_obj_t* client_session = m_new_obj(mod_coap_client_session_obj_t);
    client_session->base.type = &mod_coap_client_session_type;
    client_session->ip_addr = ip_addr_in;
    client_session->port = port_in;
    client_session->protocol = key_in == mp_const_none ? mp_obj_new_int(COAP_PROTO_UDP) : mp_obj_new_int(COAP_PROTO_DTLS);

    mp_obj_t address = mp_obj_new_list(0, NULL);

    // Compose the list of IP address and port needed by netutils_parse_inet_addr
    mp_obj_list_append(address, ip_addr_in);
    mp_obj_list_append(address, port_in);

    // Prepare the destination address where to send the request
    coap_address_t dst_address;
    coap_address_init(&dst_address);
    dst_address.addr.sin.sin_family = AF_INET;
    // The address will be in Big Endian order
    uint16_t port  = netutils_parse_inet_addr(address, (uint8_t*)&dst_address.addr.sin.sin_addr.s_addr, NETUTILS_BIG);
    // The port will be in Big Endian order
    dst_address.addr.sin.sin_port = htons(port);

    // Create a new Client Session in the scope of esp-idf Coap module based on the protocol
    mp_int_t protocol = mp_obj_get_int(client_session->protocol);
    if(protocol == COAP_PROTO_UDP) {
        client_session->session = coap_new_client_session(coap_obj_ptr->context, NULL, &dst_address, protocol);
    }
    else if(protocol == COAP_PROTO_DTLS) {
        const char *identity = mp_obj_str_get_str(identity_in);
        const uint8_t *key = (const uint8_t *)mp_obj_str_get_str(key_in);
        unsigned key_len = strlen((const char*)key);
        // PSK method is supported
        client_session->session = coap_new_client_session_psk(coap_obj_ptr->context, NULL, &dst_address, protocol, identity, key, key_len);
    }

    if(client_session->session != NULL) {
        // Add the Client Session to our context
        if(context->client_sessions == NULL) {
            // No resource exists, add as first element
            context->client_sessions = client_session;
        }
        else {
            mod_coap_client_session_obj_t* current = context->client_sessions;
            // Find the last client session
            for(; current->next != NULL; current = current->next) {}
            // Append the new resource to the end of the list
            current->next = client_session;
        }

        return client_session;
    }
    else {
        m_del_obj(mod_coap_client_session_obj_t, client_session);
        // Client Session cannot be created
        return NULL;
    }
}

// Remove the client session from the scope of the only context
STATIC bool remove_client_session(const char* ip_addr_in, const uint16_t port_in, const uint8_t protocol_in) {

    // Currently only 1 context is supported
    mod_coap_obj_t* context = coap_obj_ptr;

    if(context->client_sessions != NULL) {
        mod_coap_client_session_obj_t* current = context->client_sessions;
        mod_coap_client_session_obj_t* previous = context->client_sessions;
        for(; current != NULL; current = current->next) {

            const char* ip_addr = mp_obj_str_get_str(current->ip_addr);
            const uint16_t port = mp_obj_get_int(current->port);
            const uint8_t protocol = mp_obj_get_int(current->protocol);

            // Find the one to be removed
            if((port == port_in) && (protocol == protocol_in) && (strcmp(ip_addr, ip_addr_in) == 0)){
                // Client session found, remove from the list
                // Check if it is the first element in the list
                if(context->client_sessions == current) {
                    // If no more element in the list then invalidate the list
                    if(current->next == NULL) {
                        context->client_sessions = NULL;
                    }
                    // Other elements are in the list
                    else {
                        context->client_sessions = current->next;
                    }
                }
                else {
                    // It is not the first element
                    previous->next = current->next;
                }

                // Close the session in esp-idf
                coap_session_release(current->session);
                // Free the client session itself
                m_free(current);
                // Invalidate the client session object
                current = mp_const_none;

                return true;
            }
            // Mark the current element as previous, needed when removing the actual current element from the list
            previous = current;
        }
    }

    return false;
}

// Get the resource if exists by its uri
STATIC mod_coap_resource_obj_t* find_resource_by_uri(coap_str_const_t *uri_path) {

    if(coap_obj_ptr->resources != NULL) {
        mod_coap_resource_obj_t* current = coap_obj_ptr->resources;
        for(; current != NULL; current = current->next) {
            if(current->coap_resource->uri_path->length == uri_path->length){
                if(memcmp(current->coap_resource->uri_path->s, uri_path->s, uri_path->length) == 0) {
                    return current;
                }
            }
        }
    }
    return NULL;
}


// Create a new resource in the scope of the only context
STATIC mod_coap_resource_obj_t* add_resource(const char* uri, int32_t mediatype, int32_t max_age, mp_obj_t value, bool etag) {

    // Currently only 1 context is supported
    mod_coap_obj_t* context = coap_obj_ptr;

    coap_str_const_t coap_str;
    coap_str.s = (const uint8_t*)uri;
    coap_str.length = strlen(uri);

    if(NULL != find_resource_by_uri(&coap_str)) {
        return NULL;
    }

    // Resource does not exist, create a new resource object
    mod_coap_resource_obj_t* resource = m_new_obj(mod_coap_resource_obj_t);
    resource->base.type = &mod_coap_resource_type;

    // Get media type
    resource->mediatype = mediatype; // -1 means no media type is specified
    // Get max age
    resource->max_age = max_age; // -1 means no max_age is specified
    // Get ETAG
    resource->etag = etag; // by default it is false
    resource->etag_value = 0; // start with 0, resource_update_value() will update it (0 is incorrect for E-Tag value)

    // No next elem
    resource->next = NULL;

    resource->coap_resource = coap_resource_init(&coap_str, 0);
    if(resource->coap_resource != NULL) {
        // Add the resource to the Coap context
        coap_add_resource(context->context, resource->coap_resource);

        // If no default value is given set it to 0
        if(value == MP_OBJ_NULL) {
            value = mp_obj_new_int(0);
        }
        // Initialize default value
        resource_update_value(resource, value);

        // Add the resource to our context
        if(context->resources == NULL) {
            // No resource exists, add as first element
            context->resources = resource;
        }
        else {
            mod_coap_resource_obj_t* current = context->resources;
            // Find the last resource
            for(; current->next != NULL; current = current->next) {}
            // Append the new resource to the end of the list
            current->next = resource;
        }

        return resource;
    }
    else {
        m_del_obj(mod_coap_resource_obj_t, resource);
        // Resource cannot be created
        return NULL;
    }

    // Just for the compiler
    return mp_const_none;
}

// Remove the resource in the scope of the only context by its key
STATIC void remove_resource_by_uri(coap_str_const_t *uri_path) {

    // Currently only 1 context is supported
    mod_coap_obj_t* context = coap_obj_ptr;

    if(context->resources != NULL) {
        mod_coap_resource_obj_t* current = context->resources;
        mod_coap_resource_obj_t* previous = context->resources;
        for(; current != NULL; current = current->next) {

            if(current->coap_resource->uri_path->length == uri_path->length){
                if(memcmp(current->coap_resource->uri_path->s, uri_path->s, uri_path->length) == 0) {
                    // Resource found, remove from the list
                    // Check if it is the first element in the list
                    if(context->resources == current) {
                        // If no more element in the list then invalidate the list
                        if(current->next == NULL) {
                            context->resources = NULL;
                        }
                        // Other elements are in the list
                        else {
                            context->resources = current->next;
                        }
                    }
                    else {
                        // It is not the first element
                        previous->next = current->next;
                    }

                    // Free the resource in coap's scope
                    coap_delete_resource(context->context, current->coap_resource);
                    // Free the element in MP scope
                    m_free(current->value);
                    // Free the resource itself
                    m_free(current);
                    // Invalidate the resource object
                    current = mp_const_none;

                    return;
                }
            }
            // Mark the current element as previous, needed when removing the actual current element from the list
            previous = current;
        }
    }
}


// Remove the resource in the scope of the only context
STATIC void remove_resource(const char* uri) {

    coap_str_const_t coap_str;
    coap_str.s = (const uint8_t *)uri;
    coap_str.length = strlen(uri);

    remove_resource_by_uri(&coap_str);
}

// Update the value of a resource
STATIC void resource_update_value(mod_coap_resource_obj_t* resource, mp_obj_t new_value) {

    // If ETAG value is needed then update it
    if(resource->etag == true) {
        resource->etag_value += 1;
        // 0 as E-Tag value is not correct...
        if(resource->etag_value == 0) {
            resource->etag_value += 1;
        }
    }

    // Invalidate current data first
    resource->value_len = 0;
    m_free(resource->value);

    if (mp_obj_is_integer(new_value)) {

        uint32_t value = mp_obj_get_int_truncated(new_value);
        if (value > 0xFF) {
            resource->value_len = 2;
        } else if (value > 0xFFFF) {
            resource->value_len = 4;
        } else {
            resource->value_len = 1;
        }

        // Allocate memory for the new data
        resource->value = m_malloc(resource->value_len);
        memcpy(resource->value, &value, sizeof(value));

    } else {

        mp_buffer_info_t value_bufinfo;
        mp_get_buffer_raise(new_value, &value_bufinfo, MP_BUFFER_READ);
        resource->value_len = value_bufinfo.len;

        // Allocate memory for the new data
        resource->value = m_malloc(resource->value_len);
        memcpy(resource->value, value_bufinfo.buf, resource->value_len);
    }
}


// Callback function when GET method is received
STATIC void coap_resource_callback_get(coap_context_t * context,
                                       struct coap_resource_t * resource,
                                       coap_session_t * session,
                                       coap_pdu_t * request,
                                       coap_binary_t * token,
                                       coap_string_t * query_string,
                                       coap_pdu_t * response)
{
    xSemaphoreTake(coap_obj_ptr->semphr, portMAX_DELAY);

    mod_coap_resource_obj_t* resource_obj = find_resource_by_uri(resource->uri_path);

    // Check if the resource exists. (e.g.: has not been removed in the background before we got the semaphore in mod_coap_read())
    if(resource_obj != NULL) {

        // Check if media type of the resource is given
        if(resource_obj->mediatype != -1) {
            coap_opt_iterator_t opt_it;
            // Need to check if ACCEPT option is specified and we can serve it
            coap_opt_t *opt = coap_check_option(request, COAP_OPTION_ACCEPT, &opt_it);
            if(opt != NULL) {

                unsigned short length = coap_opt_length(opt);
                int32_t decoded = COAP_MEDIATYPE_TEXT_PLAIN;

                if(length != 0) { // 0 as length means the value is 0, which is MEDIATYPE TEXT PLAIN
                    const uint8_t* value = coap_opt_value(opt);
                    decoded = (int32_t)coap_decode_var_bytes(value, length);
                }

                // If the accepted media type and stored one does not match respond with 4.06 Not Acceptable
                if(decoded != resource_obj->mediatype) {
                    response->code = COAP_RESPONSE_CODE(406);
                    const char* error_message = coap_response_phrase(response->code);
                    coap_add_data(response, strlen(error_message), (unsigned char *)error_message);
                    return;
                }
            }
        }

        // If no ETAG option is specified in the request than the response code will be 205
        response->code = COAP_RESPONSE_CODE(205);

        // Check if ETAG value is maintained for the resource
        if(resource_obj->etag == true) {

            coap_opt_iterator_t opt_it;
            // Need to check if E-TAG option is specified and we can serve it
            coap_opt_t *opt = coap_check_option(request, COAP_OPTION_ETAG, &opt_it);
            if(opt != NULL) {

                unsigned short length = coap_opt_length(opt);
                unsigned int decoded = 0;

                if(length != 0) { // 0 as length means the value is 0
                    const uint8_t* value = coap_opt_value(opt);
                    decoded = coap_decode_var_bytes(value, length);
                }

                if(decoded == resource_obj->etag_value) {
                    // If the resource has not been updated since the last request
                    // Response must include the E-Tag option in this case, this is ensured to be happened
                    response->code = COAP_RESPONSE_CODE(203);
                }
            }
        }

        // Add the options if configured
        unsigned char buf[3];

        if(resource_obj->etag == true) {
            coap_add_option(response, COAP_OPTION_ETAG, coap_encode_var_safe(buf, sizeof(buf), resource_obj->etag_value), buf);
        }

        if(resource_obj->mediatype != -1) {
            coap_add_option(response, COAP_OPTION_CONTENT_TYPE, coap_encode_var_safe(buf, sizeof(buf), resource_obj->mediatype), buf);
        }

        if(resource_obj->max_age != -1) {
            coap_add_option(response, COAP_OPTION_MAXAGE, coap_encode_var_safe(buf, sizeof(buf), resource_obj->max_age), buf);
        }

        // Add the data itself if updated
        if(response->code == COAP_RESPONSE_CODE(205)) {
            coap_add_data(response, resource_obj->value_len, (unsigned char *)resource_obj->value);
        }
    }
    else {
        // 2.02 Deleted: The entry was deleted by another thread in the background
        response->code = COAP_RESPONSE_CODE(202);
        const char* error_message = coap_response_phrase(response->code);
        coap_add_data(response, strlen(error_message), (unsigned char *)error_message);
    }

    xSemaphoreGive(coap_obj_ptr->semphr);

}


// Callback function when PUT method is received
STATIC void coap_resource_callback_put(coap_context_t * context,
                                       struct coap_resource_t * resource,
                                       coap_session_t * session,
                                       coap_pdu_t * request,
                                       coap_binary_t * token,
                                       coap_string_t * query_string,
                                       coap_pdu_t * response)
{
    xSemaphoreTake(coap_obj_ptr->semphr, portMAX_DELAY);

    mod_coap_resource_obj_t* resource_obj = NULL;
    coap_opt_t* opt;
    coap_opt_iterator_t opt_it;
    const uint8_t* mediatype_opt_ptr = NULL;
    int32_t mediatype_opt_value = COAP_MEDIATYPE_TEXT_PLAIN;
    size_t data_size;
    unsigned char *data;

    // Get CONTENT-FORMAT option if specified
    opt = coap_check_option(request, COAP_OPTION_CONTENT_FORMAT, &opt_it);
    if(opt != NULL) {
        unsigned short length = coap_opt_length(opt);
        if(length != 0) { // 0 as length means the value is 0
            mediatype_opt_ptr = coap_opt_value(opt);
            mediatype_opt_value = (int32_t)coap_decode_var_bytes(mediatype_opt_ptr, length);
        }
        else {
            mediatype_opt_value = 0;
        }
    }
    // If no CONTENT-FORMAT is specified set the media type to unknown
    else {
        mediatype_opt_value = -1;
    }

    // Get the Data
    if(coap_get_data(request, &data_size, &data) == 0) {
        // Indicate that an error occurred
        data_size = -1;
    }

    resource_obj = find_resource_by_uri(resource->uri_path);

    // Check if the resource exists, if not then create it
    if(resource_obj == NULL) {
        // Get the URI-PATH from the message because in this case the libcoap does not provide it via resource->uri_path
        coap_opt_t *opt = coap_check_option(request, COAP_OPTION_URI_PATH, &opt_it);
        unsigned short uri_path_opt_length = 0;
        if(opt != NULL) {
            uri_path_opt_length = coap_opt_length(opt);
        }

        if(uri_path_opt_length > 0) {
            // The value is the URI-PATH
            const uint8_t* uri_opt_ptr = coap_opt_value(opt);
            // Transform it to a proper C String
            char* uri = malloc(uri_path_opt_length + 1);
            memcpy(uri, uri_opt_ptr, uri_path_opt_length);
            uri[uri_path_opt_length] = '\0';

            // Create new resource
            mp_obj_t mp_data = mp_obj_new_int(0); // Default value is 0
            if(data_size != -1) {
                // If value is specified in the request, use that
                mp_data = mp_obj_new_bytes(data, data_size);
            }

            /* Create new resource with the following parameters:
             * - URI: the given URI from request
             * - Mediatype: Mediatype from the request
             * - Max-Age: not specified
             * - Etag: no
             * - Default value: value from the request, if not specified it is 0
             */

            resource_obj = add_resource((const char *)uri, mediatype_opt_value, -1, mp_data, false);
            if(resource_obj) {
                // Resource has been created
                // Allow all methods
                coap_register_handler(resource_obj->coap_resource, COAP_REQUEST_GET, coap_resource_callback_get);
                coap_register_handler(resource_obj->coap_resource, COAP_REQUEST_PUT, coap_resource_callback_put);
                coap_register_handler(resource_obj->coap_resource, COAP_REQUEST_POST, coap_resource_callback_post);
                coap_register_handler(resource_obj->coap_resource, COAP_REQUEST_DELETE, coap_resource_callback_delete);

                // 2.01 Created
                response->code = COAP_RESPONSE_CODE(201);
                const char* message = coap_response_phrase(response->code);
                coap_add_data(response, strlen(message), (unsigned char *)message);

                // Call new resource callback if configured
                if(coap_obj_ptr->callback_new_resource != NULL) {
                    mp_irq_queue_interrupt_non_ISR(coap_response_new_resource_handler_micropython, (void *)resource_obj);
                }
            }
            else {
                // Resource has not been created due to internal error
                // 5.00 Internal Server error occurred
                response->code = COAP_RESPONSE_CODE(500);
                const char* error_message = coap_response_phrase(response->code);
                coap_add_data(response, strlen(error_message), (unsigned char *)error_message);
            }

            // No longer needed as stored elsewhere
            free(uri);
        }
        else {
            /* TODO: in this case we might generate the URI-PATH for the new resource but theoretically that should be done only in POST,
               and in POST libcoap does not support this. */

            // No URI-PATH is provided or it is empty, resource cannot be created
            // 4.00 Bad request
            response->code = COAP_RESPONSE_CODE(400);
            const char* error_message = coap_response_phrase(response->code);
            coap_add_data(response, strlen(error_message), (unsigned char *)error_message);
        }
    }
    // Resource already exists
    else {
        const uint8_t* etag_opt_ptr = NULL;
        unsigned int etag_opt_value = 0;
        bool precondition_check = true;

        // Check for If-Non-Match option, currently only 1 If-Non-Match option is supported
        coap_opt_t* opt = coap_check_option(request, COAP_OPTION_IF_NONE_MATCH, &opt_it);
        // If there is If-Non_match option added to the request (its value is always empty) then do not overwrite the resource
        if(opt != NULL){
            precondition_check = false;
        }

        if(precondition_check == true) {
            // Check for If-Match option, currently only 1 If-Match option is supported
            opt = coap_check_option(request, COAP_OPTION_IF_MATCH, &opt_it);

            if(opt != NULL) {
                unsigned short length = coap_opt_length(opt);
                if(length != 0) { // 0 as length means the value is 0
                    // The value is an E-TAG
                    etag_opt_ptr = coap_opt_value(opt);
                    etag_opt_value = coap_decode_var_bytes(etag_opt_ptr, length);
                }
            }

            // If we maintain the E-TAG of the resource then check for equality
            if((resource_obj->etag == true) && (etag_opt_value != resource_obj->etag_value)) {
                precondition_check = false;
            }
        }

        if(precondition_check == true) {

            if(data_size != -1) {
                mp_obj_t new_value = mp_obj_new_bytes(data, data_size);
                resource_update_value(resource_obj, new_value);
                resource_obj->mediatype = mediatype_opt_value;

                // Value is updated
                response->code = COAP_RESPONSE_CODE(204);

                // Add E-Tag option if configured
                unsigned char buf[3];
                if(resource_obj->etag == true) {
                    coap_add_option(response, COAP_OPTION_ETAG, coap_encode_var_safe(buf, sizeof(3), resource_obj->etag_value), buf);
                }
            }
            else {
                // There was a problem fetching the data from the message
                // 5.00 Internal Server error occurred
                response->code = COAP_RESPONSE_CODE(500);
                const char* error_message = coap_response_phrase(response->code);
                coap_add_data(response, strlen(error_message), (unsigned char *)error_message);
            }
        }
        else {
            // If-None-Match or If-Match precondition is not fulfilled
            // 4.12 Precondition failed
            response->code = COAP_RESPONSE_CODE(412);
            const char* error_message = coap_response_phrase(response->code);
            coap_add_data(response, strlen(error_message), (unsigned char *)error_message);
        }
    }

    xSemaphoreGive(coap_obj_ptr->semphr);

}


// Callback function when POST method is received
STATIC void coap_resource_callback_post(coap_context_t * context,
                                        struct coap_resource_t * resource,
                                        coap_session_t * session,
                                        coap_pdu_t * request,
                                        coap_binary_t * token,
                                        coap_string_t * query_string,
                                        coap_pdu_t * response)
{
    xSemaphoreTake(coap_obj_ptr->semphr, portMAX_DELAY);

    /* Post does not really make sense to use over PUT as the URI of the resource must be specified by the requester
     * Due to limitation of libcoap, a previously not existed resource cannot be created with POST.
     * Currently the PUT handler will be called for creating a not existing resource
    */

    mod_coap_resource_obj_t* resource_obj = find_resource_by_uri(resource->uri_path);

    // Check if the resource exists. (e.g.: has not been removed in the background before we got the semaphore in mod_coap_read())
    if(resource_obj != NULL) {

        coap_opt_iterator_t opt_it;
        // Need to check if CONTENT-FORMAT option is specified and update the stored media type accordingly
        coap_opt_t *opt = coap_check_option(request, COAP_OPTION_CONTENT_FORMAT, &opt_it);

        if(opt != NULL) {

            unsigned short length = coap_opt_length(opt);

            if(length != 0) { // 0 as length means the value is 0
                const uint8_t* value = coap_opt_value(opt);
                resource_obj->mediatype = coap_decode_var_bytes(value, length);
            }
            else {
                resource_obj->mediatype = 0;
            }
        }
        // If no CONTENT-FORMAT is specified set the media type to unknown
        else {
            resource_obj->mediatype = -1;
        }

        // Update the data and set response code and add E-Tag option if needed
        size_t size;
        unsigned char *data;
        int ret = coap_get_data(request, &size, &data);
        if(ret == 1) {
            mp_obj_t new_value = mp_obj_new_str((const char*)data, size);
            resource_update_value(resource_obj, new_value);

            // Value is updated
            response->code = COAP_RESPONSE_CODE(204);

            // Add E-Tag option if configured
            unsigned char buf[3];
            if(resource_obj->etag == true) {
                coap_add_option(response, COAP_OPTION_ETAG, coap_encode_var_safe(buf, sizeof(buf), resource_obj->etag_value), buf);
            }
        }
        else {
            // 5.00 Internal Server error occurred
            response->code = COAP_RESPONSE_CODE(500);
            const char* error_message = coap_response_phrase(response->code);
            coap_add_data(response, strlen(error_message), (unsigned char *)error_message);
        }
    }
    else {
        // 2.02 Deleted: The entry was deleted by another thread in the background
        response->code = COAP_RESPONSE_CODE(202);
        const char* error_message = coap_response_phrase(response->code);
        coap_add_data(response, strlen(error_message), (unsigned char *)error_message);
    }

    xSemaphoreGive(coap_obj_ptr->semphr);
}


// Callback function when DELETE method is received
STATIC void coap_resource_callback_delete(coap_context_t * context,
                                          struct coap_resource_t * resource,
                                          coap_session_t * session,
                                          coap_pdu_t * request,
                                          coap_binary_t * token,
                                          coap_string_t * query_string,
                                          coap_pdu_t * response)
{
    xSemaphoreTake(coap_obj_ptr->semphr, portMAX_DELAY);

    mod_coap_resource_obj_t* resource_obj = find_resource_by_uri(resource->uri_path);

    if(resource_obj != NULL) {
        // Remove it if exists
        remove_resource_by_uri(resource->uri_path);
    }

    // Reply with DELETED response
    response->code = COAP_RESPONSE_CODE(202);

    xSemaphoreGive(coap_obj_ptr->semphr);

}

STATIC void coap_response_handler_micropython(void* arg) {

    mod_coap_response_handler_args_t* params = (mod_coap_response_handler_args_t*)arg;
    mp_obj_t args[5];
    args[0] = mp_obj_new_int(params->code);
    args[1] = mp_obj_new_int(params->tid);
    args[2] = mp_obj_new_int(params->type);
    args[3] = mp_obj_new_bytes(params->token, params->token_length);
    args[4] = mp_obj_new_bytes(params->data, params->data_length);

    // Objects in C no longer needed, they are all transformed to MicroPython objects
    free(params->token);
    free(params->data);
    free(params);

    // Call the registered function, it must have 5 parameters:
    mp_call_function_n_kw(coap_obj_ptr->callback_response, 5, 0, args);

}

// Callback function for new resources created by PUT
STATIC void coap_response_new_resource_handler_micropython(void* arg)
{
    // The only arg is the newly created resource object
    mp_obj_t args[1] = {(mod_coap_resource_obj_t*)arg};
    // Call the registered function, it must have 1 parameter:
    mp_call_function_n_kw(coap_obj_ptr->callback_new_resource, 1, 0, args);
}


// Callback function for responses of requests
STATIC void coap_response_handler(struct coap_context_t *context,
                                  coap_session_t *session,
                                  coap_pdu_t *sent,
                                  coap_pdu_t *received,
                                  const coap_tid_t id)
{
    size_t len;
    uint8_t *databuf;
    int ret = coap_get_data(received, &len, &databuf);

    if(ret == 1){

        // This will be freed in coap_response_handler_micropython()
        mod_coap_response_handler_args_t* params = malloc(sizeof(mod_coap_response_handler_args_t));
        params->code = received->code;
        params->tid = received->tid;
        params->type = received->type;
        params->token_length = received->token_length;
        // This will be freed in coap_response_handler_micropython()
        params->token = malloc(params->token_length);
        memcpy(params->token, received->token, params->token_length);
        params->data_length = len;
        // This will be freed in coap_response_handler_micropython()
        params->data = malloc(params->data_length);
        memcpy(params->data, databuf, params->data_length);

        mp_irq_queue_interrupt_non_ISR(coap_response_handler_micropython, (void *)params);
    }
}

// Helper function to create a new request message
STATIC coap_pdu_t * modcoap_new_request(coap_context_t *ctx,
                                        coap_session_t* session,
                                        unsigned int method,
                                        coap_optlist_t **options,
                                        const char* token,
                                        size_t token_length,
                                        const char *data,
                                        size_t length)
{
    // TODO: get the type of the PDU as a parameter, currently only Confirmable message is supported
    // TODO: calculate the proper size of the PDU
    coap_pdu_t *pdu = coap_pdu_init(COAP_MESSAGE_CON, method, coap_new_message_id(session), coap_session_max_pdu_size(session));

    if(pdu == NULL){
        return NULL;
    }

    pdu->token_length = token_length;
    if (0 == coap_add_token(pdu, token_length, (const uint8_t*)token)) {
        return NULL;
    }

    if (options) {
        coap_add_optlist_pdu(pdu, options);
    }

    if (length) {
      coap_add_data(pdu, length, (const unsigned char*)data);
    }

    return pdu;
}

/******************************************************************************
 DEFINE COAP CLIENT SESSION CLASS FUNCTIONS
 ******************************************************************************/

STATIC const mp_arg_t mod_coap_client_session_send_request_args[] = {
        { MP_QSTR_self,                     MP_ARG_REQUIRED | MP_ARG_OBJ, },
        { MP_QSTR_method,                   MP_ARG_REQUIRED | MP_ARG_INT, },
        { MP_QSTR_uri_path,                 MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        { MP_QSTR_content_format,           MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = -1}},
        { MP_QSTR_payload,                  MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        { MP_QSTR_token,                    MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        { MP_QSTR_include_options,          MP_ARG_KW_ONLY  | MP_ARG_BOOL,{.u_bool = true}}
};


// Send a new request on the client session
STATIC mp_obj_t mod_coap_client_session_send_request(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    // The Coap module should have been already initialized
    if(initialized == true) {

        mp_arg_val_t args[MP_ARRAY_SIZE(mod_coap_client_session_send_request_args)];
        mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(args), mod_coap_client_session_send_request_args, args);

        uint8_t* uri_path = NULL;
        size_t uri_path_length = 0;
        coap_optlist_t *optlist = NULL;

        mod_coap_client_session_obj_t* self = args[0].u_obj;

        // Get the method
        mp_int_t method;
        switch (args[1].u_int){
            case MODCOAP_REQUEST_GET:
                method = COAP_REQUEST_GET;
                break;
            case MODCOAP_REQUEST_PUT:
                method = COAP_REQUEST_PUT;
                break;
            case MODCOAP_REQUEST_POST:
                method = COAP_REQUEST_POST;
                break;
            case MODCOAP_REQUEST_DELETE:
                method = COAP_REQUEST_DELETE;
                break;
            default:
                nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, "Invalid \"method\" parameter value!"));
        }


        // Get the path
        if(args[2].u_obj != MP_OBJ_NULL) {
            uri_path = (uint8_t*)mp_obj_str_get_data(args[2].u_obj, &uri_path_length);
        }

        // Get the content format
       mp_int_t content_format = args[3].u_int;
       switch (content_format){
           case -1:
           case COAP_MEDIATYPE_TEXT_PLAIN:
           case COAP_MEDIATYPE_APPLICATION_CBOR:
           case COAP_MEDIATYPE_APPLICATION_EXI:
           case COAP_MEDIATYPE_APPLICATION_JSON:
           case COAP_MEDIATYPE_APPLICATION_LINK_FORMAT:
           case COAP_MEDIATYPE_APPLICATION_OCTET_STREAM:
           case COAP_MEDIATYPE_APPLICATION_RDF_XML:
           case COAP_MEDIATYPE_APPLICATION_XML:
               break;
           default:
               nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, "Invalid \"content_format\" parameter value!"));
       }

        // Get the payload
        const char* payload = NULL;
        size_t payload_length = 0;
        if(args[4].u_obj != MP_OBJ_NULL) {
            payload = mp_obj_str_get_data(args[4].u_obj, &payload_length);
        }

        // Get the token
        const char* token = NULL;
        size_t token_length = 0;
        if(args[5].u_obj != MP_OBJ_NULL) {
            token = mp_obj_str_get_data(args[5].u_obj, &token_length);
        }

        // Get the include_options parameter
        bool include_options = args[6].u_bool;

        // Take the context's semaphore to avoid concurrent access
        xSemaphoreTake(coap_obj_ptr->semphr, portMAX_DELAY);

        if(include_options == true) {

            // Put the URI-HOST as an option
            coap_insert_optlist(&optlist, coap_new_optlist(COAP_OPTION_URI_HOST, 4, (const uint8_t*)(&self->session->remote_addr.addr.sin.sin_addr.s_addr)));

            // Put the URI-PORT as an option
            // Already stored in Big Endian
            uint8_t portbuf[2];
            memcpy(portbuf, &self->session->remote_addr.addr.sin.sin_port, sizeof(portbuf));
            coap_insert_optlist(&optlist, coap_new_optlist(COAP_OPTION_URI_PORT, sizeof(portbuf), portbuf));

            // Split up the URI-PATH into more segments if needed
            //TODO: allocate the proper length
            size_t length = 300;
            uint8_t* path = malloc(length);
            uint8_t* path_start = path;
            int segments = coap_split_path(uri_path, uri_path_length, path, &length);

            // Insert the segments as separate URI-Path options
            while (segments--) {
                coap_insert_optlist(&optlist, coap_new_optlist(COAP_OPTION_URI_PATH, coap_opt_length(path), coap_opt_value(path)));
                path += coap_opt_size(path);
            }

            // Free up the allocated space, at this point path does not point to the beginning
            free(path_start);

            // Put Content Format option if given
            if(content_format != -1) {
                uint8_t content_format_buf[2];
                // Store it in Big Endian
                content_format_buf[0] = (content_format >> 8) & 0xFF;
                content_format_buf[1] = content_format & 0xFF;
                coap_insert_optlist(&optlist, coap_new_optlist(COAP_OPTION_CONTENT_FORMAT, sizeof(content_format_buf), content_format_buf));
            }
        }

        // Create new request
        coap_pdu_t *pdu = modcoap_new_request(coap_obj_ptr->context, self->session, method, &optlist, token, token_length, payload, payload_length);

        // Clean up optlist, they are already part of the PDU if it has been created
        coap_delete_optlist(optlist);

        if (pdu == NULL) {
            xSemaphoreGive(coap_obj_ptr->semphr);
            nlr_raise(mp_obj_new_exception_msg(&mp_type_OSError, "Cannot create request"));
        }

        // Send out the request
        // The session contains the destination address and port
        coap_tid_t ret = coap_send(self->session, pdu);

        // Sending the packet failed
        if(ret == COAP_INVALID_TID) {
            xSemaphoreGive(coap_obj_ptr->semphr);
            nlr_raise(mp_obj_new_exception_msg(&mp_type_OSError, "Sending message failed!"));
        }

        // Fetch the message ID to be used from MicroPython to match the request with response
        mp_obj_t id = mp_obj_new_int(pdu->tid);

        xSemaphoreGive(coap_obj_ptr->semphr);

        return id;
    }
    else {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "Coap module has not been initialized!"));
        // Just to fulfill the compiler's needs
        return mp_const_none;
    }
}

STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_coap_client_session_send_request_obj, 2, mod_coap_client_session_send_request);

// Get the details of this Client Session
STATIC mp_obj_t mod_coap_client_session_get_details(mp_obj_t self_in) {

    // The Coap module should have been already initialized
    if(initialized == true) {

        mp_obj_t list = mp_obj_new_list(0, NULL);

        xSemaphoreTake(coap_obj_ptr->semphr, portMAX_DELAY);

        mod_coap_client_session_obj_t* self = (mod_coap_client_session_obj_t*)self_in;

        mp_obj_list_append(list, self->ip_addr);
        mp_obj_list_append(list, self->port);
        mp_obj_list_append(list, self->protocol);

        xSemaphoreGive(coap_obj_ptr->semphr);

        return list;

    }
    else {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "Coap module has not been initialized!"));
        // Just to fulfill the compiler's needs
        return mp_const_none;
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mod_coap_client_session_get_details_obj, mod_coap_client_session_get_details);

STATIC const mp_map_elem_t coap_client_session_locals_table[] = {
    // instance methods
    { MP_OBJ_NEW_QSTR(MP_QSTR_send_request),               (mp_obj_t)&mod_coap_client_session_send_request_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_get_details),               (mp_obj_t)&mod_coap_client_session_get_details_obj },
};
STATIC MP_DEFINE_CONST_DICT(coap_client_session_locals, coap_client_session_locals_table);

STATIC const mp_obj_type_t mod_coap_client_session_type = {
    { &mp_type_type },
    .name = MP_QSTR_CoapClientSession,
    .locals_dict = (mp_obj_t)&coap_client_session_locals,
};


/******************************************************************************
 DEFINE COAP RESOURCE CLASS FUNCTIONS
 ******************************************************************************/

// Add attribute to a resource
STATIC mp_obj_t mod_coap_resource_add_attribute(mp_obj_t self_in, mp_obj_t name, mp_obj_t val) {

    mod_coap_resource_obj_t* self = (mod_coap_resource_obj_t*)self_in;

    coap_str_const_t name_coap_str;
    name_coap_str.s = (const uint8_t *)mp_obj_str_get_str(name);
    name_coap_str.length = strlen((const char*)name_coap_str.s);

    coap_str_const_t val_coap_str;
    val_coap_str.s = (const uint8_t *)mp_obj_str_get_str(val);
    val_coap_str.length = strlen((const char*)val_coap_str.s);

    coap_attr_t * attribute = coap_add_attr(self->coap_resource,
                                           &name_coap_str,
                                           &val_coap_str,
                                           0);

    if(attribute == NULL) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_MemoryError, "Attribute cannot be added"));
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(mod_coap_resource_add_attribute_obj, mod_coap_resource_add_attribute);


// Gets or sets the value of a resource
STATIC mp_obj_t mod_coap_resource_value(mp_uint_t n_args, const mp_obj_t *args) {

    mod_coap_resource_obj_t* self = (mod_coap_resource_obj_t*)args[0];
    mp_obj_t ret = mp_const_none;

    xSemaphoreTake(coap_obj_ptr->semphr, portMAX_DELAY);
    // If the value exists, e.g.: not deleted from another task before we got the semaphore
    if(self->value != NULL) {
        if (n_args == 1) {
            // get
            ret = mp_obj_new_bytes(self->value, self->value_len);
        } else {
            // set
            resource_update_value(self, (mp_obj_t)args[1]);
        }
    }
    xSemaphoreGive(coap_obj_ptr->semphr);

    return ret;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_coap_resource_value_obj, 1, 2, mod_coap_resource_value);


// Enable or disable a specific action on a resource
STATIC mp_obj_t mod_coap_resource_callback_enable(mp_obj_t self_in, mp_obj_t request_type_in, mp_obj_t enable_in) {

    mod_coap_resource_obj_t* self = (mod_coap_resource_obj_t*)self_in;

    mp_int_t request_type = mp_obj_get_int(request_type_in);
    bool enable = mp_obj_get_int(enable_in) == 0 ? false : true;

    if(request_type & MODCOAP_REQUEST_GET) {
        if(enable)    coap_register_handler(self->coap_resource, COAP_REQUEST_GET, coap_resource_callback_get);
        else          coap_register_handler(self->coap_resource, COAP_REQUEST_GET, NULL);
    }

    if(request_type & MODCOAP_REQUEST_PUT) {
        if(enable)    coap_register_handler(self->coap_resource, COAP_REQUEST_PUT, coap_resource_callback_put);
        else          coap_register_handler(self->coap_resource, COAP_REQUEST_PUT, NULL);
    }

    if(request_type & MODCOAP_REQUEST_POST) {
        if(enable)    coap_register_handler(self->coap_resource, COAP_REQUEST_POST, coap_resource_callback_post);
        else          coap_register_handler(self->coap_resource, COAP_REQUEST_POST, NULL);
    }

    if(request_type & MODCOAP_REQUEST_DELETE) {
        if(enable)    coap_register_handler(self->coap_resource, COAP_REQUEST_DELETE, coap_resource_callback_delete);
        else          coap_register_handler(self->coap_resource, COAP_REQUEST_DELETE, NULL);
    }

    return mp_const_none;

}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(mod_coap_resource_callback_enable_obj, mod_coap_resource_callback_enable);


// Get the details of this resource
STATIC mp_obj_t mod_coap_resource_get_details(mp_obj_t self_in) {

    mod_coap_resource_obj_t* self = (mod_coap_resource_obj_t*)self_in;
    mp_obj_t list = mp_obj_new_list(0, NULL);


    xSemaphoreTake(coap_obj_ptr->semphr, portMAX_DELAY);

        mp_obj_list_append(list, mp_obj_new_str((const char*)self->coap_resource->uri_path->s, self->coap_resource->uri_path->length));
        mp_obj_list_append(list, MP_OBJ_NEW_SMALL_INT(self->mediatype));
        mp_obj_list_append(list, MP_OBJ_NEW_SMALL_INT(self->max_age));
        mp_obj_list_append(list, MP_OBJ_NEW_SMALL_INT(self->etag));
        mp_obj_list_append(list, MP_OBJ_NEW_SMALL_INT(self->etag_value));

    xSemaphoreGive(coap_obj_ptr->semphr);

    return list;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mod_coap_resource_get_details_obj, mod_coap_resource_get_details);

STATIC const mp_arg_t mod_coap_resource_set_details_args[] = {
        { MP_QSTR_self,                      MP_ARG_REQUIRED | MP_ARG_OBJ, },
        { MP_QSTR_mediatype,                 MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_obj = mp_const_none}},
        { MP_QSTR_max_age,                   MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_obj = mp_const_none}},
        { MP_QSTR_etag,                      MP_ARG_KW_ONLY  | MP_ARG_BOOL, {.u_obj = mp_const_none}}
};

// Set the details of this resource
STATIC mp_obj_t mod_coap_resource_set_details(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    mp_arg_val_t args[MP_ARRAY_SIZE(mod_coap_resource_set_details_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(args), mod_coap_resource_set_details_args, args);

    mod_coap_resource_obj_t* self = (mod_coap_resource_obj_t*)args[0].u_obj;

    xSemaphoreTake(coap_obj_ptr->semphr, portMAX_DELAY);

    // Set mediatype if given
    if(args[1].u_obj != mp_const_none) {
        self->mediatype = args[1].u_int;
    }

    // Set max age if given
    if(args[2].u_obj != mp_const_none) {
        self->max_age = args[2].u_int;
    }

    // Set etag if given
    if(args[3].u_obj != mp_const_none) {
        self->etag = args[3].u_bool;
        self->etag_value = self->etag ? 1 : 0;
    }

    xSemaphoreGive(coap_obj_ptr->semphr);

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_coap_resource_set_details_obj, 1, mod_coap_resource_set_details);


STATIC const mp_map_elem_t coap_resource_locals_table[] = {
    // instance methods
        { MP_OBJ_NEW_QSTR(MP_QSTR_add_attribute),               (mp_obj_t)&mod_coap_resource_add_attribute_obj },
        { MP_OBJ_NEW_QSTR(MP_QSTR_value),                       (mp_obj_t)&mod_coap_resource_value_obj },
        { MP_OBJ_NEW_QSTR(MP_QSTR_callback),                    (mp_obj_t)&mod_coap_resource_callback_enable_obj },
        { MP_OBJ_NEW_QSTR(MP_QSTR_get_details),                 (mp_obj_t)&mod_coap_resource_get_details_obj },
        { MP_OBJ_NEW_QSTR(MP_QSTR_set_details),                 (mp_obj_t)&mod_coap_resource_set_details_obj }

};
STATIC MP_DEFINE_CONST_DICT(coap_resource_locals, coap_resource_locals_table);

STATIC const mp_obj_type_t mod_coap_resource_type = {
    { &mp_type_type },
    .name = MP_QSTR_CoapResource,
    .locals_dict = (mp_obj_t)&coap_resource_locals,
};

/******************************************************************************
 DEFINE COAP CLASS FUNCTIONS
 ******************************************************************************/


STATIC void mod_coap_init_helper(mp_obj_t address, bool service_discovery, bool dynamic_resources, mp_obj_t key_in, mp_obj_t hint_in) {

    // Initialize Context without address, in Server mode address will be added
    coap_obj_ptr->context = coap_new_context(NULL);

    // Configure Server mode
    if(address != NULL) {
        coap_address_t server_address;

        // Create a new Coap context
        coap_address_init(&server_address);
        server_address.addr.sin.sin_family = AF_INET;
        // The address will be in Big Endian order
        uint16_t port  = netutils_parse_inet_addr(address, (uint8_t*)&server_address.addr.sin.sin_addr.s_addr, NETUTILS_BIG);
        // Store the port in network byte order
        server_address.addr.sin.sin_port = htons(port);

        // Configure PSK if given
        if(key_in != mp_const_none && hint_in != mp_const_none) {
            const char *hint = mp_obj_str_get_str(hint_in);
            const uint8_t *key = (const uint8_t *)mp_obj_str_get_str(key_in);
            size_t key_len = strlen((const char*)key);
            coap_context_set_psk(coap_obj_ptr->context, hint, key, key_len);
            coap_new_endpoint(coap_obj_ptr->context, &server_address, COAP_PROTO_DTLS);
        }
        else {
            coap_new_endpoint(coap_obj_ptr->context, &server_address, COAP_PROTO_UDP);
        }

        // Listen on coap multicast ip address for service discovery if enabled
        if(service_discovery == true) {
            // Compose the address structure
            struct ip_mreq mreq;
            memcpy(&mreq.imr_interface, &server_address.addr.sin.sin_addr, sizeof(mreq.imr_interface));
            mp_obj_t list = mp_obj_new_list(0, NULL);
            mp_obj_list_append(list, mp_obj_new_str(MODCOAP_IP4_MULTICAST, strlen(MODCOAP_IP4_MULTICAST)));
            mp_obj_list_append(list, mp_obj_new_int(0)); // Port does not matter
            netutils_parse_inet_addr(list, (uint8_t*)&mreq.imr_multiaddr, NETUTILS_BIG);

            // Set socket option to join multicast group
            lwip_setsockopt(coap_obj_ptr->context->endpoint->sock.fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
        }

        // These will be registered on demand by the user
        coap_obj_ptr->callback_response = NULL;
        coap_obj_ptr->callback_new_resource = NULL;
        if(dynamic_resources == true) {
            // Create a dummy resource to handle PUTs to unknown URIs
            coap_resource_t* unknown_resource = coap_resource_unknown_init(coap_resource_callback_put);
            coap_add_resource(coap_obj_ptr->context, unknown_resource);
        }
    }
}

STATIC const mp_arg_t mod_coap_init_args[] = {
        { MP_QSTR_address,                  MP_ARG_OBJ                  , {.u_obj = NULL}},
        { MP_QSTR_port,                     MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_obj = mp_const_none}},
        { MP_QSTR_service_discovery,        MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false}},
        { MP_QSTR_dynamic_resources,        MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false}},
        { MP_QSTR_psk,                      MP_ARG_OBJ  | MP_ARG_KW_ONLY, {.u_obj = mp_const_none}},
        { MP_QSTR_hint,                     MP_ARG_OBJ  | MP_ARG_KW_ONLY, {.u_obj = mp_const_none}},
};

// Initialize the module
STATIC mp_obj_t mod_coap_init(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    // The Coap module should be initialized only once
    // Only 1 context is supported currently
    if(initialized == false) {

        MP_STATE_PORT(coap_ptr) = m_malloc(sizeof(mod_coap_obj_t));
        coap_obj_ptr = MP_STATE_PORT(coap_ptr);
        coap_obj_ptr->context = NULL;
        coap_obj_ptr->resources = NULL;
        coap_obj_ptr->client_sessions = NULL;
        coap_obj_ptr->semphr = NULL;

        mp_arg_val_t args[MP_ARRAY_SIZE(mod_coap_init_args)];
        mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(args), mod_coap_init_args, args);

        // The module is used in Coap Server and Client mode
        if(args[0].u_obj != NULL) {

            // Check security parameters
            mp_obj_t psk = args[4].u_obj;
            mp_obj_t hint = args[5].u_obj;

            // If neither PSK nor Hint is defined that means no security is enabled, otherwise security is used
            if((psk == mp_const_none && hint != mp_const_none) || (psk != mp_const_none && hint == mp_const_none)) {
                nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, "Both PSK and Hint must be defined"));
            }

            // Get the address
            mp_obj_t address = args[0].u_obj;
            // Get the port
            mp_obj_t port = args[1].u_obj;
            // If no value is specified for the port then use default port value depending on whether security is enabled
            if(port == mp_const_none) {
                if(psk != mp_const_none && hint != mp_const_none) {
                    port = mp_obj_new_int(COAPS_DEFAULT_PORT);
                }
                else if((psk == mp_const_none && hint == mp_const_none)){
                    port = mp_obj_new_int(COAP_DEFAULT_PORT);
                }
            }
            // Get whether service discovery is supported
            bool service_discovery = args[2].u_bool;
            // Get whether dynamic resource creation is allowed via PUT
            bool dynamic_resources = args[3].u_bool;

            // Compose a list containing the address and the port
            mp_obj_t list = mp_obj_new_list(0, NULL);
            mp_obj_list_append(list, address);
            mp_obj_list_append(list, port);

            mod_coap_init_helper(list, service_discovery, dynamic_resources, psk, hint);
        }
        // The module is used in Coap Client only mode
        else {
            mod_coap_init_helper(NULL, false, false, mp_const_none, mp_const_none);
        }

        xTaskCreatePinnedToCore(TASK_MODCOAP, "Coap", MODCOAP_TASK_STACK_SIZE / sizeof(StackType_t), NULL, MODCOAP_TASK_PRIORITY, &ModCoapTaskHandle, 1);

        coap_obj_ptr->semphr = xSemaphoreCreateBinary();
        xSemaphoreGive(coap_obj_ptr->semphr);

        initialized = true;
    }
    else {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "Coap module already initialized!"));
    }

    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_coap_init_obj, 0, mod_coap_init);

STATIC const mp_arg_t mod_coap_add_resource_args[] = {
        { MP_QSTR_uri,                      MP_ARG_REQUIRED | MP_ARG_OBJ, },
        { MP_QSTR_media_type,               MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = -1}},
        { MP_QSTR_max_age,                  MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = -1}},
        { MP_QSTR_value,                    MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        { MP_QSTR_etag,                     MP_ARG_KW_ONLY  | MP_ARG_BOOL,{.u_bool = false}},
};

// Add a new resource to the context if not exists
STATIC mp_obj_t mod_coap_add_resource(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    // The Coap module should have been already initialized
    if(initialized == true) {

        xSemaphoreTake(coap_obj_ptr->semphr, portMAX_DELAY);

        mp_arg_val_t args[MP_ARRAY_SIZE(mod_coap_add_resource_args)];
        mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(args), mod_coap_add_resource_args, args);

        mod_coap_resource_obj_t* res = add_resource(mp_obj_str_get_str(args[0].u_obj), args[1].u_int, args[2].u_int, args[3].u_obj, args[4].u_bool);

        xSemaphoreGive(coap_obj_ptr->semphr);

        // Theoretically it can happen that the resource was created successfully but deleted right after releasing the semaphore above, but
        // the result is the same: the new resource does not exist when this function returns
        if(res == NULL) {
            nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "Resource already exists or cannot be created!"));
        }

        return res;
    }
    else {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "Coap module has not been initialized!"));
    }

    return mp_const_none;

}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_coap_add_resource_obj, 1, mod_coap_add_resource);

// Remove a resource from the context if exists
STATIC mp_obj_t mod_coap_remove_resource(mp_obj_t uri) {

    // The Coap module should have been already initialized
    if(initialized == true) {
        xSemaphoreTake(coap_obj_ptr->semphr, portMAX_DELAY);
        remove_resource(mp_obj_str_get_str(uri));
        xSemaphoreGive(coap_obj_ptr->semphr);
    }
    else {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "Coap module has not been initialized!"));
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mod_coap_remove_resource_obj, mod_coap_remove_resource);

// Get a resource from the context if exists
STATIC mp_obj_t mod_coap_get_resource(mp_obj_t uri_in) {

    mp_obj_t res = NULL;
    // The Coap module should have been already initialized
    if(initialized == true) {
        xSemaphoreTake(coap_obj_ptr->semphr, portMAX_DELAY);
        coap_str_const_t coap_str;
        coap_str.s = (const uint8_t*)mp_obj_str_get_str(uri_in);
        coap_str.length = strlen((const char*)coap_str.s);
        res = find_resource_by_uri(&coap_str);
        xSemaphoreGive(coap_obj_ptr->semphr);
    }
    else {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "Coap module has not been initialized!"));
    }

    return res == NULL ? mp_const_none : res;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mod_coap_get_resource_obj, mod_coap_get_resource);

// Register the user's callback handler to be called when response is received to a request
STATIC mp_obj_t mod_coap_register_response_handler(mp_obj_t callback) {

    // The Coap module should have been already initialized
    if(initialized == true) {
        // Take the context's semaphore to avoid concurrent access, this will guard the handler functions too
        xSemaphoreTake(coap_obj_ptr->semphr, portMAX_DELAY);
        // Register the user's callback handler
        coap_obj_ptr->callback_response = callback;
        coap_register_response_handler(coap_obj_ptr->context, coap_response_handler);
        xSemaphoreGive(coap_obj_ptr->semphr);
    }
    else {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "Coap module has not been initialized!"));
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mod_coap_register_response_handler_obj, mod_coap_register_response_handler);

// Register the user's callback handler to be called when new resource is created via PUT
STATIC mp_obj_t mod_coap_register_new_resource_handler(mp_obj_t callback) {

    // The Coap module should have been already initialized
    if(initialized == true) {
        // Take the context's semaphore to avoid concurrent access, this will guard the handler functions too
        xSemaphoreTake(coap_obj_ptr->semphr, portMAX_DELAY);
        // Register the user's callback handler
        coap_obj_ptr->callback_new_resource = callback;
        xSemaphoreGive(coap_obj_ptr->semphr);
    }
    else {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "Coap module has not been initialized!"));
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mod_coap_register_new_resource_handler_obj, mod_coap_register_new_resource_handler);

STATIC const mp_arg_t mod_coap_new_client_session_args[] = {
        { MP_QSTR_address,                  MP_ARG_OBJ                  , {.u_obj = NULL}},
        { MP_QSTR_port,                     MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_obj = mp_const_none}},
        { MP_QSTR_psk,                      MP_ARG_OBJ  | MP_ARG_KW_ONLY, {.u_obj = mp_const_none}},
        { MP_QSTR_identity,                 MP_ARG_OBJ  | MP_ARG_KW_ONLY, {.u_obj = mp_const_none}},
};
// Create a new client session
STATIC mp_obj_t mod_coap_new_client_session(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    // The Coap module should have been already initialized
    if(initialized == true) {

        mp_arg_val_t args[MP_ARRAY_SIZE(mod_coap_new_client_session_args)];
        mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(args), mod_coap_new_client_session_args, args);

        mp_obj_t ip_addr = args[0].u_obj;
        mp_obj_t port = args[1].u_obj;
        mp_obj_t psk = args[2].u_obj;
        mp_obj_t identity = args[3].u_obj;

        // If port not defined, select appropriate default port
        if(port == mp_const_none) {
            if(psk == mp_const_none) {
                port =  mp_obj_new_int(COAP_DEFAULT_PORT);
            }
            else {
                port =  mp_obj_new_int(COAPS_DEFAULT_PORT);
            }
        }

        // If security is used then port must be different than 5683
        if(psk != mp_const_none && mp_obj_get_int(port) == COAP_DEFAULT_PORT) {
            nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, "If Security is configured port must not be 5683!"));
        }
        // If security is NOT used then port must be different than 5684
        else if(psk == mp_const_none && mp_obj_get_int(port) == COAPS_DEFAULT_PORT) {
            nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, "If Security is not configured port must not be 5684!"));
        }

        // Check if both PSK and Identity are configured
        if((psk != mp_const_none && identity == mp_const_none) || (psk == mp_const_none && identity != mp_const_none)){
            nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, "For Security both PSK and Identity must be configured!"));
        }


        xSemaphoreTake(coap_obj_ptr->semphr, portMAX_DELAY);

        mod_coap_client_session_obj_t* session = new_client_session(ip_addr, port, psk, identity);

        xSemaphoreGive(coap_obj_ptr->semphr);

        if(session == NULL) {
            nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "Client Session has not been created!"));
        }

        return session;
    }
    else {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "Coap module has not been initialized!"));
        // Just to fulfill the compiler's needs
        return mp_const_none;
    }
}

STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_coap_new_client_session_obj, 1, mod_coap_new_client_session);

// Remove a client session
STATIC mp_obj_t mod_coap_remove_client_session(mp_obj_t ip_addr_in, mp_obj_t port_in, mp_obj_t protocol_in) {

    // The Coap module should have been already initialized
    if(initialized == true) {

        xSemaphoreTake(coap_obj_ptr->semphr, portMAX_DELAY);

        const char* ip_addr = mp_obj_str_get_str(ip_addr_in);
        const uint16_t port = mp_obj_get_int(port_in);
        const uint8_t protocol = mp_obj_get_int(protocol_in);

        bool ret = remove_client_session(ip_addr, port, protocol);

        xSemaphoreGive(coap_obj_ptr->semphr);

        if(ret == false) {
            nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "Client Session has not been removed"));
        }

        return mp_const_none;
    }
    else {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "Coap module has not been initialized!"));
        // Just to fulfill the compiler's needs
        return mp_const_none;
    }
}

STATIC MP_DEFINE_CONST_FUN_OBJ_3(mod_coap_remove_client_session_obj, mod_coap_remove_client_session);

// Get all client sessions
STATIC mp_obj_t mod_coap_get_client_sessions() {

    // The Coap module should have been already initialized
    if(initialized == true) {

        mp_obj_t list = mp_obj_new_list(0, NULL);

        xSemaphoreTake(coap_obj_ptr->semphr, portMAX_DELAY);

        mod_coap_client_session_obj_t* client_session = coap_obj_ptr->client_sessions;
        while(client_session != NULL) {
            mp_obj_list_append(list, client_session);
            client_session = client_session->next;
        }

        xSemaphoreGive(coap_obj_ptr->semphr);

        return list;
    }
    else {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "Coap module has not been initialized!"));
        // Just to fulfill the compiler's needs
        return mp_const_none;
    }
}

STATIC MP_DEFINE_CONST_FUN_OBJ_0(mod_coap_get_client_sessions_obj, mod_coap_get_client_sessions);


STATIC const mp_map_elem_t mod_coap_globals_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___name__),                        MP_OBJ_NEW_QSTR(MP_QSTR_coap) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_init),                            (mp_obj_t)&mod_coap_init_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_add_resource),                    (mp_obj_t)&mod_coap_add_resource_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_remove_resource),                 (mp_obj_t)&mod_coap_remove_resource_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_get_resource),                    (mp_obj_t)&mod_coap_get_resource_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_register_response_handler),       (mp_obj_t)&mod_coap_register_response_handler_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_register_new_resource_handler),   (mp_obj_t)&mod_coap_register_new_resource_handler_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_new_client_session),              (mp_obj_t)&mod_coap_new_client_session_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_remove_client_session),           (mp_obj_t)&mod_coap_remove_client_session_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_get_client_sessions),             (mp_obj_t)&mod_coap_get_client_sessions_obj },


    // class constants
    { MP_OBJ_NEW_QSTR(MP_QSTR_REQUEST_GET),                     MP_OBJ_NEW_SMALL_INT(MODCOAP_REQUEST_GET) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_REQUEST_PUT),                     MP_OBJ_NEW_SMALL_INT(MODCOAP_REQUEST_PUT) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_REQUEST_POST),                    MP_OBJ_NEW_SMALL_INT(MODCOAP_REQUEST_POST) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_REQUEST_DELETE),                  MP_OBJ_NEW_SMALL_INT(MODCOAP_REQUEST_DELETE) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_MEDIATYPE_TEXT_PLAIN),            MP_OBJ_NEW_SMALL_INT(COAP_MEDIATYPE_TEXT_PLAIN) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_MEDIATYPE_APP_LINK_FORMAT),       MP_OBJ_NEW_SMALL_INT(COAP_MEDIATYPE_APPLICATION_LINK_FORMAT) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_MEDIATYPE_APP_XML),               MP_OBJ_NEW_SMALL_INT(COAP_MEDIATYPE_APPLICATION_XML) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_MEDIATYPE_APP_OCTET_STREAM),      MP_OBJ_NEW_SMALL_INT(COAP_MEDIATYPE_APPLICATION_OCTET_STREAM) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_MEDIATYPE_APP_RDF_XML),           MP_OBJ_NEW_SMALL_INT(COAP_MEDIATYPE_APPLICATION_RDF_XML) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_MEDIATYPE_APP_EXI),               MP_OBJ_NEW_SMALL_INT(COAP_MEDIATYPE_APPLICATION_EXI) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_MEDIATYPE_APP_JSON),              MP_OBJ_NEW_SMALL_INT(COAP_MEDIATYPE_APPLICATION_JSON) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_MEDIATYPE_APP_CBOR),              MP_OBJ_NEW_SMALL_INT(COAP_MEDIATYPE_APPLICATION_CBOR) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_PROTOCOL_UDP),                    MP_OBJ_NEW_SMALL_INT(COAP_PROTO_UDP) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_PROTOCOL_DTLS),                   MP_OBJ_NEW_SMALL_INT(COAP_PROTO_DTLS) },

};

STATIC MP_DEFINE_CONST_DICT(mod_coap_globals, mod_coap_globals_table);

const mp_obj_module_t mod_coap = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&mod_coap_globals,
};
