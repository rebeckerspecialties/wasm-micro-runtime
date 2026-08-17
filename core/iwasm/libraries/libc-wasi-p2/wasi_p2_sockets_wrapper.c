/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasi_p2_sockets_wrapper.h"
#include "bh_common.h"
#include "bh_hashmap.h"
#include "wasm_runtime_common.h"

#include "wasi_p2_io.h"
#include "wasi_p2_common.h"
#include "wasi_p2_types.h"
#include "component-model/wasm_component_host_resource.h"
#include "component-model/wasm_component_resource.h"
#include "component-model/wasm_canonical_abi.h"
#include "component-model/wasm_component_canonical.h"
#include "component-model/wasm_component_flat.h"
#include "../../../product-mini/platforms/common/libc_wasi.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <pthread.h>
#include <limits.h>
#include <unistd.h>

#ifndef UIO_MAXIOV
#define UIO_MAXIOV IOV_MAX
#endif

// Helper function

static bool
init_owned_fd_pollable(wasm_exec_env_t exec_env, HostResource *resource,
                       int source_fd, wasi_pollable_type_t type)
{
    WasiP2NativeFdQuotaLease fd_lease = { 0 };
    int poll_fd;

    if (!resource || !resource->data || source_fd < 0
        || !wasi_p2_native_fd_quota_reserve(exec_env, 1, &fd_lease)) {
        return false;
    }
    poll_fd = fcntl(source_fd, F_DUPFD_CLOEXEC, 0);
    if (poll_fd < 0) {
        wasi_p2_native_fd_quota_release(&fd_lease);
        return false;
    }
    SET_POLLABLE_CTX((wasi_pollable_context_t *)resource->data, poll_fd, true,
                     type);
    wasi_p2_native_fd_quota_transfer_to_host_resource(resource, &fd_lease);
    return true;
}

static HostResource *
create_tcp_stream_resource(HostResourceType type, int32_t fd)
{
    HostResource *resource =
        host_resource_create(type, sizeof(StreamResourceType));
    if (!resource) {
        return NULL;
    }

    ((StreamResourceType *)resource->data)->fd = (uint32_t)fd;
    ((StreamResourceType *)resource->data)->type = STREAM_TYPE_SOCKET;
    ((StreamResourceType *)resource->data)->position = 0;
    ((StreamResourceType *)resource->data)->position_valid = false;
    ((StreamResourceType *)resource->data)->append = false;
    host_resource_set_dtor(resource, tcp_owned_stream_dtor);
    return resource;
}

static uint32_t
add_tcp_stream_resource(HostResourceTable *table, HostResourceType type,
                        int32_t fd)
{
    HostResource *resource;
    uint32_t rep;

    if (fd < 0) {
        return 0;
    }

    resource = create_tcp_stream_resource(type, fd);
    if (!resource) {
        close(fd);
        return 0;
    }

    rep = host_resource_table_add(table, resource);
    if (rep == 0) {
        destroy_host_resource(resource);
    }
    return rep;
}

static uint32_t
add_udp_datagram_stream_resource(HostResourceTable *table,
                                 HostResourceType type, int32_t fd,
                                 WasiP2NativeFdQuotaLease *fd_lease)
{
    HostResource *resource;
    uint32_t rep;

    if (fd < 0) {
        wasi_p2_native_fd_quota_release(fd_lease);
        return 0;
    }

    resource = host_resource_create(type, sizeof(uint32_t));
    if (!resource) {
        close(fd);
        wasi_p2_native_fd_quota_release(fd_lease);
        return 0;
    }

    *(uint32_t *)resource->data = (uint32_t)fd;
    host_resource_set_dtor(resource, udp_datagram_stream_dtor);
    wasi_p2_native_fd_quota_transfer_to_host_resource(resource, fd_lease);

    rep = host_resource_table_add(table, resource);
    if (rep == 0) {
        destroy_host_resource(resource);
    }
    return rep;
}

static wit_value_t
make_owned_resource_result(uint32_t rep)
{
    wit_value_t resource = wit_resource_ctor(rep);
    wit_value_t result;

    if (!resource) {
        return NULL;
    }
    result = wit_result_ctor(false, resource);
    if (!result) {
        free_wit_value(resource);
    }
    return result;
}

/* Consume a non-NULL payload on both success and failure. */
static wit_value_t
make_owned_ok_result(wit_value_t payload)
{
    wit_value_t result;

    if (!payload) {
        return NULL;
    }
    result = wit_result_ctor(false, payload);
    if (!result) {
        free_wit_value(payload);
    }
    return result;
}

static wit_value_t
make_resource_tuple_result(const uint32_t *reps, uint32_t count)
{
    wit_value_t *elems;
    wit_value_t tuple;
    wit_value_t result;
    uint32_t i;

    if (!reps || count == 0) {
        return NULL;
    }

    elems = wasm_runtime_malloc(count * sizeof(wit_value_t));
    if (!elems) {
        return NULL;
    }

    for (i = 0; i < count; i++) {
        elems[i] = wit_resource_ctor(reps[i]);
        if (!elems[i]) {
            while (i > 0) {
                free_wit_value(elems[--i]);
            }
            wasm_runtime_free(elems);
            return NULL;
        }
    }

    tuple = wit_tuple_ctor(elems, count);
    if (!tuple) {
        for (i = 0; i < count; i++) {
            free_wit_value(elems[i]);
        }
        wasm_runtime_free(elems);
        return NULL;
    }

    result = wit_result_ctor(false, tuple);
    if (!result) {
        free_wit_value(tuple);
    }
    return result;
}

/* Lower a fixed result<unit, network-error> success value before a socket
   operation can mutate externally visible native state. The outer canonical
   transaction stays pending until the caller reports whether the syscall
   succeeded. */
static bool
stage_unit_socket_success(wasm_exec_env_t exec_env, uint32_t offset_addr,
                          WASMComponentTypeInstance *result_type,
                          wit_value_t *result, wit_value_t *error_payload,
                          CanonicalResourceTransferScope *lower_scope)
{
    if (!exec_env || !exec_env->cx || !result_type || !result || !error_payload
        || !lower_scope) {
        return false;
    }

    /* Reserve the only payload needed by the failure result too. If the
       syscall fails, the staged Ok can then be replaced by Err without a
       quota-visible allocation or a stale-success fallback. */
    *error_payload = wit_enum_ctor(WASI_NETWORK_ERROR_CODE_UNKNOWN);
    if (!*error_payload) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to allocate socket error result");
        return false;
    }
    *result = wit_result_ctor(false, NULL);
    if (!*result) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to allocate socket success result");
        return false;
    }
    if (!canonical_resource_transfer_scope_enter(
            lower_scope, exec_env->cx,
            WASM_COMPONENT_TABLE_TRANSACTION_LOWER)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to start socket result transfer");
        return false;
    }
    bool stored = store(exec_env->cx, offset_addr, result_type, *result);
    bool can_commit = canonical_resource_transfer_scope_can_commit(lower_scope);
    if (!stored || !can_commit) {
        (void)canonical_resource_transfer_scope_leave(lower_scope, false);
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to stage socket result");
        return false;
    }
    return true;
}

static bool
finish_unit_socket_result(wasm_exec_env_t exec_env, uint32_t offset_addr,
                          WASMComponentTypeInstance *result_type,
                          wit_value_t result, wit_value_t *error_payload,
                          CanonicalResourceTransferScope *lower_scope,
                          int native_error, const char *failure_message)
{
    if (native_error != 0) {
        (*error_payload)->value.enum_value.value =
            errno_to_wasi_network(native_error);
        result->value.result_value.is_err = true;
        result->value.result_value.result.err = *error_payload;
        *error_payload = NULL;
        if (!store(exec_env->cx, offset_addr, result_type, result)
            || !canonical_resource_transfer_scope_can_commit(lower_scope)) {
            (void)canonical_resource_transfer_scope_leave(lower_scope, false);
            wasm_runtime_set_exception(exec_env->module_inst, failure_message);
            return false;
        }
    }

    if (!canonical_resource_transfer_scope_leave(lower_scope, true)) {
        wasm_runtime_set_exception(exec_env->module_inst, failure_message);
        return false;
    }
    return true;
}

typedef int (*socket_unit_mutation_fn)(wasi_socket_context_t *socket,
                                       uint64_t value);

static int
set_tcp_backlog(wasi_socket_context_t *socket, uint64_t value)
{
    socket->tcp_listen_backlog = (int64_t)value;
    return 0;
}

static int
set_tcp_keep_alive_enabled(wasi_socket_context_t *socket, uint64_t value)
{
    return wasi_sockets_tcp_set_keep_alive_enabled(socket->fd, value != 0);
}

static int
set_tcp_keep_alive_idle_time(wasi_socket_context_t *socket, uint64_t value)
{
    return wasi_sockets_tcp_set_keep_alive_idle_time(socket->fd, value);
}

static int
set_tcp_keep_alive_interval(wasi_socket_context_t *socket, uint64_t value)
{
    return wasi_sockets_tcp_set_keep_alive_interval(socket->fd, value);
}

static int
set_tcp_keep_alive_count(wasi_socket_context_t *socket, uint64_t value)
{
    return wasi_sockets_tcp_set_keep_alive_count(socket->fd, (uint32_t)value);
}

static int
set_tcp_hop_limit(wasi_socket_context_t *socket, uint64_t value)
{
    return wasi_sockets_tcp_set_hop_limit(socket->fd, (uint8_t)value);
}

static int
set_tcp_receive_buffer_size(wasi_socket_context_t *socket, uint64_t value)
{
    return wasi_sockets_tcp_set_receive_buffer_size(socket->fd, value);
}

static int
set_tcp_send_buffer_size(wasi_socket_context_t *socket, uint64_t value)
{
    return wasi_sockets_tcp_set_send_buffer_size(socket->fd, value);
}

static int
set_udp_unicast_hop_limit(wasi_socket_context_t *socket, uint64_t value)
{
    return wasi_sockets_udp_set_unicast_hop_limit(socket->fd, (uint8_t)value);
}

static int
set_udp_receive_buffer_size(wasi_socket_context_t *socket, uint64_t value)
{
    return wasi_sockets_udp_set_receive_buffer_size(socket->fd, value);
}

static int
set_udp_send_buffer_size(wasi_socket_context_t *socket, uint64_t value)
{
    return wasi_sockets_udp_set_send_buffer_size(socket->fd, value);
}

static bool
publish_socket_unit_mutation(wasm_exec_env_t exec_env, uint32_t offset_addr,
                             WASMComponentTypeInstance *result_type,
                             wasi_socket_context_t *socket, uint64_t value,
                             socket_unit_mutation_fn mutate,
                             const char *failure_message)
{
    wit_value_t result = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };
    bool success = false;
    int err;

    if (!socket || !mutate
        || !stage_unit_socket_success(exec_env, offset_addr, result_type,
                                      &result, &error_payload, &lower_scope)) {
        goto cleanup;
    }

    err = mutate(socket, value);
    success = finish_unit_socket_result(exec_env, offset_addr, result_type,
                                        result, &error_payload, &lower_scope,
                                        err, failure_message);

cleanup:
    if (lower_scope.active)
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
    free_wit_value(error_payload);
    free_wit_value(result);
    return success;
}

/**
 * @brief Helper function to convert a WASI IP socket address struct into a WIT
 * Variant value.
 * @param address Pointer to the `wasi_ip_socket_address_t` struct to convert.
 * @return wit_value_t The constructed WIT Variant value representing an IPv4 or
 * IPv6 socket address.
 */
wit_value_t
get_ip_sock_addr_value(wasi_ip_socket_address_t *address)
{
    ComponentWITRecordField *fields = NULL;
    wit_value_t *elems = NULL;
    wit_value_t address_tuple = NULL;
    wit_value_t port = NULL;
    wit_value_t flow_info = NULL;
    wit_value_t scope_id = NULL;
    wit_value_t record = NULL;
    wit_value_t variant = NULL;
    uint32_t elem_count = 0;
    uint32_t field_count = 0;
    uint32_t i;

    if (!address)
        return NULL;

    if (address->tag == WASI_IP_SOCKET_ADDRESS_TAG_IPV4) {
        elem_count = 4;
        field_count = 2;
        elems = wasm_runtime_calloc(elem_count, sizeof(wit_value_t));
        if (!elems)
            goto fail;
        elems[0] = wit_u8_ctor(address->val.ipv4.address.f0);
        elems[1] = wit_u8_ctor(address->val.ipv4.address.f1);
        elems[2] = wit_u8_ctor(address->val.ipv4.address.f2);
        elems[3] = wit_u8_ctor(address->val.ipv4.address.f3);
        for (i = 0; i < elem_count; i++) {
            if (!elems[i])
                goto fail;
        }
        address_tuple = wit_tuple_ctor(elems, elem_count);
        if (!address_tuple)
            goto fail;
        elems = NULL;
        port = wit_u16_ctor(address->val.ipv4.port);
        if (!port)
            goto fail;
        fields =
            wasm_runtime_calloc(field_count, sizeof(ComponentWITRecordField));
        if (!fields)
            goto fail;
        init_record_field(&fields[0], "port", 4, port);
        if (!fields[0].key)
            goto fail;
        port = NULL;
        init_record_field(&fields[1], "address", 7, address_tuple);
        if (!fields[1].key)
            goto fail;
        address_tuple = NULL;
        record = wit_record_ctor(fields, field_count);
        if (!record)
            goto fail;
        fields = NULL;
        variant = wit_variant_ctor("ipv4", 4, record);
    }
    else if (address->tag == WASI_IP_SOCKET_ADDRESS_TAG_IPV6) {
        const uint16_t segments[8] = {
            address->val.ipv6.address.f0, address->val.ipv6.address.f1,
            address->val.ipv6.address.f2, address->val.ipv6.address.f3,
            address->val.ipv6.address.f4, address->val.ipv6.address.f5,
            address->val.ipv6.address.f6, address->val.ipv6.address.f7
        };

        elem_count = 8;
        field_count = 4;
        elems = wasm_runtime_calloc(elem_count, sizeof(wit_value_t));
        if (!elems)
            goto fail;
        for (i = 0; i < elem_count; i++) {
            elems[i] = wit_u16_ctor(segments[i]);
            if (!elems[i])
                goto fail;
        }
        address_tuple = wit_tuple_ctor(elems, elem_count);
        if (!address_tuple)
            goto fail;
        elems = NULL;
        port = wit_u16_ctor(address->val.ipv6.port);
        flow_info = wit_u32_ctor(address->val.ipv6.flow_info);
        scope_id = wit_u32_ctor(address->val.ipv6.scope_id);
        if (!port || !flow_info || !scope_id)
            goto fail;
        fields =
            wasm_runtime_calloc(field_count, sizeof(ComponentWITRecordField));
        if (!fields)
            goto fail;
        init_record_field(&fields[0], "port", 4, port);
        if (!fields[0].key)
            goto fail;
        port = NULL;
        init_record_field(&fields[1], "flow-info", 9, flow_info);
        if (!fields[1].key)
            goto fail;
        flow_info = NULL;
        init_record_field(&fields[2], "address", 7, address_tuple);
        if (!fields[2].key)
            goto fail;
        address_tuple = NULL;
        init_record_field(&fields[3], "scope-id", 8, scope_id);
        if (!fields[3].key)
            goto fail;
        scope_id = NULL;
        record = wit_record_ctor(fields, field_count);
        if (!record)
            goto fail;
        fields = NULL;
        variant = wit_variant_ctor("ipv6", 4, record);
    }
    else {
        return NULL;
    }

    if (!variant)
        goto fail;
    return variant;

fail:
    if (elems) {
        for (i = 0; i < elem_count; i++)
            free_wit_value(elems[i]);
        wasm_runtime_free(elems);
    }
    if (fields) {
        for (i = 0; i < field_count; i++) {
            wasm_runtime_free(fields[i].key);
            free_wit_value(fields[i].value);
        }
        wasm_runtime_free(fields);
    }
    free_wit_value(address_tuple);
    free_wit_value(port);
    free_wit_value(flow_info);
    free_wit_value(scope_id);
    free_wit_value(record);
    return NULL;
}

/**
 * @brief Helper function to convert a WASI IP address struct into a WIT Option
 * value.
 * @param address Pointer to the `wasi_ip_address_t` struct to convert.
 * @return wit_value_t The constructed WIT Option value wrapping the IP address
 * Variant.
 */
wit_value_t
get_ip_addr_value(wasi_ip_address_t *address)
{
    wit_value_t *elems = NULL;
    wit_value_t tuple = NULL;
    wit_value_t variant = NULL;
    wit_value_t option = NULL;
    uint32_t elem_count;
    uint32_t i;

    if (!address)
        return NULL;
    elem_count = address->kind == IPv4 ? 4 : 8;
    elems = wasm_runtime_calloc(elem_count, sizeof(wit_value_t));
    if (!elems)
        goto fail;
    if (address->kind == IPv4) {
        elems[0] = wit_u8_ctor(address->addr.ip4.n0);
        elems[1] = wit_u8_ctor(address->addr.ip4.n1);
        elems[2] = wit_u8_ctor(address->addr.ip4.n2);
        elems[3] = wit_u8_ctor(address->addr.ip4.n3);
    }
    else {
        const uint16_t segments[8] = {
            address->addr.ip6.n0, address->addr.ip6.n1, address->addr.ip6.n2,
            address->addr.ip6.n3, address->addr.ip6.h0, address->addr.ip6.h1,
            address->addr.ip6.h2, address->addr.ip6.h3
        };
        for (i = 0; i < elem_count; i++)
            elems[i] = wit_u16_ctor(segments[i]);
    }
    for (i = 0; i < elem_count; i++) {
        if (!elems[i])
            goto fail;
    }
    tuple = wit_tuple_ctor(elems, elem_count);
    if (!tuple)
        goto fail;
    elems = NULL;
    variant =
        wit_variant_ctor(address->kind == IPv4 ? "ipv4" : "ipv6", 4, tuple);
    if (!variant)
        goto fail;
    tuple = NULL;
    option = wit_option_ctor(variant);
    if (!option)
        goto fail;
    return option;

fail:
    if (elems) {
        for (i = 0; i < elem_count; i++)
            free_wit_value(elems[i]);
        wasm_runtime_free(elems);
    }
    free_wit_value(tuple);
    free_wit_value(variant);
    return NULL;
}

/**
 * @brief Helper function to convert an incoming datagram struct into a WIT
 * Record value.
 * @param datagram Pointer to the `wasi_incoming_datagram_t` struct to convert.
 * @return wit_value_t The constructed WIT Record value containing the data and
 * remote address.
 */
wit_value_t
get_incoming_datagram_value(wasi_incoming_datagram_t *datagram)
{
    ComponentWITRecordField *fields = NULL;
    wit_value_t *elems = NULL;
    wit_value_t data = NULL;
    wit_value_t remote_address = NULL;
    wit_value_t record = NULL;
    uint32_t data_len;
    uint32_t idx;

    if (!datagram || datagram->data_len > UINT32_MAX
        || (datagram->data_len > 0 && !datagram->data)) {
        return NULL;
    }
    data_len = (uint32_t)datagram->data_len;
    if (data_len > 0) {
        elems = wasm_runtime_calloc(data_len, sizeof(wit_value_t));
        if (!elems)
            goto fail;
        for (idx = 0; idx < data_len; idx++) {
            elems[idx] = wit_u8_ctor(datagram->data[idx]);
            if (!elems[idx])
                goto fail;
        }
    }
    data = wit_list_ctor(elems, data_len);
    if (!data)
        goto fail;
    elems = NULL;
    remote_address = get_ip_sock_addr_value(&datagram->remote_address);
    if (!remote_address)
        goto fail;
    fields = wasm_runtime_calloc(2, sizeof(ComponentWITRecordField));
    if (!fields)
        goto fail;
    init_record_field(&fields[0], "data", 4, data);
    if (!fields[0].key)
        goto fail;
    data = NULL;
    init_record_field(&fields[1], "remote-address", 14, remote_address);
    if (!fields[1].key)
        goto fail;
    remote_address = NULL;
    record = wit_record_ctor(fields, 2);
    if (!record)
        goto fail;
    return record;

fail:
    if (elems) {
        for (idx = 0; idx < data_len; idx++)
            free_wit_value(elems[idx]);
        wasm_runtime_free(elems);
    }
    if (fields) {
        for (idx = 0; idx < 2; idx++) {
            wasm_runtime_free(fields[idx].key);
            free_wit_value(fields[idx].value);
        }
        wasm_runtime_free(fields);
    }
    free_wit_value(data);
    free_wit_value(remote_address);
    return NULL;
}

/* wasi:sockets/instance-network */

/**
 * @brief Wrapper for the `instance-network` function of the
 * `wasi:sockets/instance-network` interface.
 * @param exec_env The execution environment.
 * @return A handle to the default network resource.
 */
uint32_t
wasi_sockets_instance_network_instance_network_wrapper(wasm_exec_env_t exec_env)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network) {
        return 0;
    }

    wasi_network_t instance_network = wasi_sockets_instance_network();

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr =
        host_resource_create(WASI_P2_NETWORK, sizeof(instance_network));

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not create network resource");
        return 0;
    }

    *((uint32_t *)hr->data) = instance_network;

    uint32_t out = host_resource_table_add(hr_table, hr);
    if (out < 1) {
        destroy_host_resource(hr); // Clean up the HostResource on failure
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not add network resource to HR table");
        return 0;
    }

    if (!lower_owned_host_resource(
            exec_env,
            func_type->results->result[0].type_specific.resource_handle,
            hr_table, out, &out)) {
        return 0;
    }
    return out;
}

/* wasi:sockets/ip-name-lookup */

/**
 * @brief Wrapper for the `resolve-addresses` function of the
 * `wasi:sockets/ip-name-lookup` interface.
 * @details This function resolves a hostname to a stream of IP addresses.
 * @param exec_env The execution environment.
 * @param network_handle The handle of the network resource.
 * @param name_ptr A pointer to the hostname string in the guest's memory.
 * @param name_len The length of the hostname string.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_ip_name_lookup_resolve_addresses_wrapper(wasm_exec_env_t exec_env,
                                                      uint32_t network_handle,
                                                      uint32_t name_ptr,
                                                      uint32_t name_len,
                                                      uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;
    wit_value_t name_val = NULL;
    uint32_t owned_rep = 0;
    WasiP2NativeFdQuotaLease fd_lease = { 0 };
    WasiP2NativeFdQuotaLease read_fd_lease = { 0 };
    WasiP2NativeFdQuotaLease write_fd_lease = { 0 };
    if (!lift_borrow(
            exec_env->cx, network_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->allow_ip_name_lookup) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    wasi_resolve_address_stream_t stream;
    int err = 0;

    if (!load_string_from_range(exec_env->cx, name_ptr, name_len, &name_val)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    char *name = name_val->value.string_value.chars;

    if (!name) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    // Get the actual network fd from the host resource
    wasi_network_t network_fd = *((uint32_t *)hr->data);

    if (!wasi_p2_native_fd_quota_reserve(exec_env, 2, &fd_lease)
        || !wasi_p2_native_fd_quota_lease_take(&fd_lease, 1, &read_fd_lease)
        || !wasi_p2_native_fd_quota_lease_take(&fd_lease, 1, &write_fd_lease)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NEW_SOCKET_LIMIT);
        goto end;
    }

    wasi_sockets_resolve_addresses_with_fd_quota(
        network_fd, name, &stream, &err, &read_fd_lease, &write_fd_lease);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
        goto end;
    }
    else {
        HostResource *hr_stream = host_resource_create(
            WASI_P2_RESOLVE_ADDRESS_STREAM, sizeof(stream));

        if (!hr_stream) {
            wasi_sockets_drop_resolve_stream(stream);
            result =
                get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
            goto end;
        }

        *((uint32_t *)hr_stream->data) = stream;
        host_resource_set_dtor(hr_stream, resolve_stream_dtor);

        uint32_t out = host_resource_table_add(hr_table, hr_stream);
        if (out < 1) {
            destroy_host_resource(
                hr_stream); // Clean up the HostResource on failure
            result =
                get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
            goto end;
        }
        result = make_owned_resource_result(out);
        if (!result) {
            (void)host_resource_table_delete(hr_table, out);
            result =
                get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
        }
        else {
            owned_rep = out;
        }
        goto end;
    }

end:
    wasi_p2_native_fd_quota_release(&fd_lease);
    wasi_p2_native_fd_quota_release(&read_fd_lease);
    wasi_p2_native_fd_quota_release(&write_fd_lease);
    (void)wasi_p2_store_owned_host_resource_result(
        exec_env, offset_addr, func_type->results->result, result, &owned_rep,
        owned_rep != 0 ? 1 : 0);
    free_wit_value(result);
    free_wit_value(lifted_handle);
    free_wit_value(name_val);
}

/**
 * @brief Wrapper for the `resolve-next-address` method of the
 * `wasi:sockets/ip-name-lookup.resolve-address-stream` resource.
 * @param exec_env The execution environment.
 * @param stream_handle The handle of the resolve-address-stream resource.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_ip_name_lookup_resolve_address_stream_resolve_next_address_wrapper(
    wasm_exec_env_t exec_env, uint32_t stream_handle, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;
    wit_value_t ip_addr;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->allow_ip_name_lookup) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    wasi_ip_address_t ip_address;
    bool is_some;
    int err = 0;

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    // Get the actual stream fd from the host resource
    wasi_network_t stream_fd = *((uint32_t *)hr->data);

    wasi_sockets_resolve_next_address(stream_fd, &ip_address, &is_some, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
        goto end;
    }
    if (is_some) {
        ip_addr = get_ip_addr_value(&ip_address);
        result = make_owned_ok_result(ip_addr);
        goto end;
    }
    else {
        result = wit_result_ctor(false, NULL);
        goto end;
    }

end:
    if (result) {
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    }
    else {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "failed to construct resolve-next-address result");
    }
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `subscribe` method of the
 * `wasi:sockets/ip-name-lookup.resolve-address-stream` resource.
 * @param exec_env The execution environment.
 * @param stream_handle The rep of the resolve-address-stream resource.
 * @return A new pollable resource rep.
 */
uint32_t
wasi_sockets_ip_name_lookup_resolve_address_stream_subscribe_wrapper(
    wasm_exec_env_t exec_env, uint32_t stream_handle)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    WasiP2NativeFdQuotaLease fd_lease = { 0 };

    if (!lift_borrow(
            exec_env->cx, stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        return 0;
    }

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->allow_ip_name_lookup) {
        free_wit_value(lifted_handle);
        return 0;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);
    free_wit_value(lifted_handle);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get stream resource");
        return 0;
    }

    // Get the actual stream fd from the host resource
    wasi_network_t stream_fd = *((uint32_t *)hr->data);

    if (!wasi_p2_native_fd_quota_reserve(exec_env, 1, &fd_lease)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "native file descriptor quota exceeded");
        return 0;
    }

    wasi_pollable_context_t pollable =
        wasi_sockets_resolve_address_stream_subscribe(stream_fd);
    if (pollable.fd < 0) {
        wasi_p2_native_fd_quota_release(&fd_lease);
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get pollable FD");
        return 0;
    }

    HostResource *hr_poll =
        host_resource_create(WASI_P2_POLLABLE, sizeof(wasi_pollable_context_t));

    if (!hr_poll) {
        if (pollable.own_fd)
            close(pollable.fd);
        wasi_p2_native_fd_quota_release(&fd_lease);
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not create pollable resource");
        return 0;
    }
    *((wasi_pollable_context_t *)hr_poll->data) = pollable;
    host_resource_set_dtor(hr_poll, pollable_dtor);
    wasi_p2_native_fd_quota_transfer_to_host_resource(hr_poll, &fd_lease);

    uint32_t index_rep = host_resource_table_add(hr_table, hr_poll);
    if (index_rep < 1) {
        destroy_host_resource(hr_poll); // Clean up the HostResource on failure
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not add pollable resource to HR table");
        return 0;
    }

    if (!lower_owned_host_resource(
            exec_env,
            func_type->results->result[0].type_specific.resource_handle,
            hr_table, index_rep, &index_rep)) {
        return 0;
    }

    return index_rep;
}

/* wasi:sockets/tcp-create-socket */

/**
 * @brief Wrapper for the `create-tcp-socket` function of the
 * `wasi:sockets/tcp-create-socket` interface.
 * @param exec_env The execution environment.
 * @param address_family The IP address family.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_create_socket_create_tcp_socket_wrapper(
    wasm_exec_env_t exec_env, uint32_t address_family, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t result = NULL;
    uint32_t owned_rep = 0;
    WasiP2NativeFdQuotaLease fd_lease = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!wasi_p2_native_fd_quota_reserve(exec_env, 1, &fd_lease)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NEW_SOCKET_LIMIT);
        goto end;
    }

    wasi_tcp_socket_t socket;
    int err = 0;

    wasi_sockets_create_tcp_socket(address_family, &socket, &err);

    if (err != 0) {
        wasi_p2_native_fd_quota_release(&fd_lease);
        result = get_result_error_val(errno_to_wasi_network(err));
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr =
        host_resource_create(WASI_P2_TCP_SOCKET, sizeof(wasi_socket_context_t));

    if (!hr) {
        close(socket);
        wasi_p2_native_fd_quota_release(&fd_lease);
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
        goto end;
    }

    ((wasi_socket_context_t *)hr->data)->family = address_family;
    ((wasi_socket_context_t *)hr->data)->fd = socket;
    ((wasi_socket_context_t *)hr->data)->tcp_listen_backlog = SOMAXCONN;
    ((wasi_socket_context_t *)hr->data)->tcp_is_listening = false;
    host_resource_set_dtor(hr, tcp_socket_dtor);
    wasi_p2_native_fd_quota_transfer_to_host_resource(hr, &fd_lease);

    uint32_t index_rep = host_resource_table_add(hr_table, hr);
    if (index_rep < 1) {
        destroy_host_resource(hr); // Clean up the HostResource on failure
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
        goto end;
    }
    else {
        result = make_owned_resource_result(index_rep);
        if (!result) {
            (void)host_resource_table_delete(hr_table, index_rep);
            result =
                get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
        }
        else {
            owned_rep = index_rep;
        }
        goto end;
    }
end:
    wasi_p2_native_fd_quota_release(&fd_lease);
    (void)wasi_p2_store_owned_host_resource_result(
        exec_env, offset_addr, func_type->results->result, result, &owned_rep,
        owned_rep != 0 ? 1 : 0);
    free_wit_value(result);
}

/* wasi:sockets/tcp */

/**
 * @brief Helper function to convert flattened IP socket address fields into a
 * structure.
 */
static void
read_flattened_ip_socket_address(wasi_ip_socket_address_t *addr,
                                 uint32_t addr_tag, uint32_t p0, uint32_t p1,
                                 uint32_t p2, uint32_t p3, uint32_t p4,
                                 uint32_t p5, uint32_t p6, uint32_t p7,
                                 uint32_t p8, uint32_t p9, uint32_t p10)
{
    addr->tag = addr_tag;
    if (addr->tag == WASI_IP_SOCKET_ADDRESS_TAG_IPV4) {
        addr->val.ipv4.port = (uint16_t)p0;
        addr->val.ipv4.address.f0 = (uint8_t)p1;
        addr->val.ipv4.address.f1 = (uint8_t)p2;
        addr->val.ipv4.address.f2 = (uint8_t)p3;
        addr->val.ipv4.address.f3 = (uint8_t)p4;
    }
    else { /* IPV6 */
        addr->val.ipv6.port = (uint16_t)p0;
        addr->val.ipv6.flow_info = (uint32_t)p1;
        addr->val.ipv6.address.f0 = (uint16_t)p2;
        addr->val.ipv6.address.f1 = (uint16_t)p3;
        addr->val.ipv6.address.f2 = (uint16_t)p4;
        addr->val.ipv6.address.f3 = (uint16_t)p5;
        addr->val.ipv6.address.f4 = (uint16_t)p6;
        addr->val.ipv6.address.f5 = (uint16_t)p7;
        addr->val.ipv6.address.f6 = (uint16_t)p8;
        addr->val.ipv6.address.f7 = (uint16_t)p9;
        addr->val.ipv6.scope_id = (uint32_t)p10;
    }
}

/**
 * @brief Wrapper for the `start-bind` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param network_handle The handle of the network resource.
 * @param addr_tag The tag of the IP socket address.
 * @param ... The flattened fields of the IP socket address.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_start_bind_wrapper(
    wasm_exec_env_t exec_env, uint32_t socket_handle, uint32_t network_handle,
    uint32_t addr_tag, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3,
    uint32_t p4, uint32_t p5, uint32_t p6, uint32_t p7, uint32_t p8,
    uint32_t p9, uint32_t p10, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_network_handle = NULL;
    wit_value_t lifted_socket_handle = NULL;
    wit_value_t result = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    wasi_ip_socket_address_t local_address;
    int err = 0;

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_socket_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, network_handle,
            func_type->params->params[1].type->type_specific.resource_handle,
            &lifted_network_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    read_flattened_ip_socket_address(&local_address, addr_tag, p0, p1, p2, p3,
                                     p4, p5, p6, p7, p8, p9, p10);

    HostResourceTable *hr_table = get_global_host_resource_table();

    HostResource *socket_hr = host_resource_table_get(
        hr_table, lifted_socket_handle->value.resource_value.value);
    HostResource *network_hr = host_resource_table_get(
        hr_table, lifted_network_handle->value.resource_value.value);

    if (!(socket_hr && network_hr)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    // Get the actual socket fd and network fd from the host resource
    wasi_network_t network_fd = *((uint32_t *)network_hr->data);
    wasi_tcp_socket_t socket_fd =
        ((wasi_socket_context_t *)socket_hr->data)->fd;

    if (!stage_unit_socket_success(exec_env, offset_addr,
                                   func_type->results->result, &result,
                                   &error_payload, &lower_scope)) {
        goto cleanup;
    }

    err = wasi_sockets_tcp_start_bind(socket_fd, network_fd, &local_address);
    (void)finish_unit_socket_result(
        exec_env, offset_addr, func_type->results->result, result,
        &error_payload, &lower_scope, err, "failed to publish TCP bind result");
    goto cleanup;

end:
    if (result) {
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    }
cleanup:
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_socket_handle);
    free_wit_value(lifted_network_handle);
}

/**
 * @brief Wrapper for the `finish-bind` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_finish_bind_wrapper(wasm_exec_env_t exec_env,
                                                uint32_t socket_handle,
                                                uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *socket_hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!socket_hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    // Get the actual socket fd from the host resource
    wasi_tcp_socket_t socket_fd =
        ((wasi_socket_context_t *)socket_hr->data)->fd;

    int err = wasi_sockets_tcp_finish_bind(socket_fd);
    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
    }
    else {
        result = wit_result_ctor(false, NULL);
    }

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Helper function to write an IP socket address back into WASM native
 * memory.
 */
static void
write_ip_socket_address(WASMExecEnv *exec_env,
                        const wasi_ip_socket_address_t *addr,
                        uint64_t wasm_addr_offset)
{
    char *addr_ptr =
        wasm_runtime_addr_app_to_native_p2(exec_env, wasm_addr_offset);
    memset(addr_ptr, 0, 4 + sizeof(wasi_ipv6_socket_address_t));
    *(uint8_t *)addr_ptr = addr->tag;
    if (addr->tag == WASI_IP_SOCKET_ADDRESS_TAG_IPV4) {
        *(wasi_ipv4_socket_address_t *)(addr_ptr + 4) = addr->val.ipv4;
    }
    else {
        *(wasi_ipv6_socket_address_t *)(addr_ptr + 4) = addr->val.ipv6;
    }
}

/**
 * @brief Wrapper for the `start-connect` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param network_handle The handle of the network resource.
 * @param addr_tag The tag of the IP socket address.
 * @param ... The flattened fields of the IP socket address.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_start_connect_wrapper(
    wasm_exec_env_t exec_env, uint32_t socket_handle, uint32_t network_handle,
    uint32_t addr_tag, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3,
    uint32_t p4, uint32_t p5, uint32_t p6, uint32_t p7, uint32_t p8,
    uint32_t p9, uint32_t p10, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_network_handle = NULL;
    wit_value_t lifted_socket_handle = NULL;
    wit_value_t result = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_socket_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, network_handle,
            func_type->params->params[1].type->type_specific.resource_handle,
            &lifted_network_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    wasi_ip_socket_address_t remote_address;
    int err = 0;

    read_flattened_ip_socket_address(&remote_address, addr_tag, p0, p1, p2, p3,
                                     p4, p5, p6, p7, p8, p9, p10);

    HostResourceTable *hr_table = get_global_host_resource_table();

    HostResource *socket_hr = host_resource_table_get(
        hr_table, lifted_socket_handle->value.resource_value.value);
    HostResource *network_hr = host_resource_table_get(
        hr_table, lifted_network_handle->value.resource_value.value);

    if (!(socket_hr && network_hr)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    // Get the actual socket fd and network fd from the host resource
    wasi_network_t network_fd = *((wasi_network_t *)network_hr->data);
    uint32_t socket_fd = ((wasi_socket_context_t *)socket_hr->data)->fd;

    if (!stage_unit_socket_success(exec_env, offset_addr,
                                   func_type->results->result, &result,
                                   &error_payload, &lower_scope)) {
        goto cleanup;
    }

    err =
        wasi_sockets_tcp_start_connect(socket_fd, network_fd, &remote_address);

    if (err == EINPROGRESS) {
        err = 0;
    }
    (void)finish_unit_socket_result(exec_env, offset_addr,
                                    func_type->results->result, result,
                                    &error_payload, &lower_scope, err,
                                    "failed to publish TCP connect result");
    goto cleanup;

end:
    if (result) {
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    }
cleanup:
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_socket_handle);
    free_wit_value(lifted_network_handle);
}

/**
 * @brief Wrapper for the `finish-connect` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_finish_connect_wrapper(wasm_exec_env_t exec_env,
                                                   uint32_t socket_handle,
                                                   uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };
    uint32_t owned_reps[2] = { 0 };
    uint32_t owned_rep_count = 0;
    HostResourceTable *hr_table = NULL;
    HostResource *input_hr = NULL;
    HostResource *output_hr = NULL;
    bool lower_scope_active = false;
    WasiP2NativeFdQuotaLease fd_lease = { 0 };
    WasiP2NativeFdQuotaLease input_fd_lease = { 0 };
    WasiP2NativeFdQuotaLease output_fd_lease = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    wasi_input_stream_t input_stream_fd = -1;
    wasi_output_stream_t output_stream_fd = -1;
    int err = 0;

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    /* Prepare both HostResource/table entries and the complete canonical lower
       before observing connection completion or duplicating the socket. Once
       the native status is consumed, success publication is allocation-free. */
    wasi_tcp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;
    error_payload = wit_enum_ctor(WASI_NETWORK_ERROR_CODE_UNKNOWN);
    if (!error_payload) {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "failed to allocate TCP connect error result");
        goto cleanup;
    }
    input_hr =
        create_tcp_stream_resource(WASI_P2_IO_INPUT_STREAM, input_stream_fd);
    if (!input_hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
        goto end;
    }
    owned_reps[0] = host_resource_table_add(hr_table, input_hr);
    if (owned_reps[0] == 0) {
        destroy_host_resource(input_hr);
        input_hr = NULL;
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
        goto end;
    }

    output_hr =
        create_tcp_stream_resource(WASI_P2_IO_OUTPUT_STREAM, output_stream_fd);
    if (!output_hr) {
        (void)host_resource_table_delete(hr_table, owned_reps[0]);
        owned_reps[0] = 0;
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
        goto end;
    }
    owned_reps[1] = host_resource_table_add(hr_table, output_hr);
    if (owned_reps[1] == 0) {
        destroy_host_resource(output_hr);
        output_hr = NULL;
        (void)host_resource_table_delete(hr_table, owned_reps[0]);
        owned_reps[0] = 0;
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
        goto end;
    }

    result = make_resource_tuple_result(owned_reps, 2);
    if (!result) {
        (void)host_resource_table_delete(hr_table, owned_reps[1]);
        (void)host_resource_table_delete(hr_table, owned_reps[0]);
        owned_reps[0] = owned_reps[1] = 0;
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
        goto end;
    }
    if (!canonical_resource_transfer_scope_enter(
            &lower_scope, exec_env->cx,
            WASM_COMPONENT_TABLE_TRANSACTION_LOWER)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to start TCP stream transfer");
        goto rollback_resources;
    }
    lower_scope_active = true;
    if (!wasi_p2_store_owned_host_resource_result(
            exec_env, offset_addr, func_type->results->result, result, NULL, 0)
        || !canonical_resource_transfer_scope_can_commit(&lower_scope)) {
        goto rollback_resources;
    }

    if (!wasi_p2_native_fd_quota_reserve(exec_env, 2, &fd_lease)
        || !wasi_p2_native_fd_quota_lease_take(&fd_lease, 1, &input_fd_lease)
        || !wasi_p2_native_fd_quota_lease_take(&fd_lease, 1,
                                               &output_fd_lease)) {
        err = EMFILE;
        goto native_error;
    }

    err = wasi_sockets_duplicate_socket_streams(socket_fd, &input_stream_fd,
                                                &output_stream_fd);
    if (err != 0) {
        goto native_error;
    }
    ((StreamResourceType *)input_hr->data)->fd = (uint32_t)input_stream_fd;
    ((StreamResourceType *)output_hr->data)->fd = (uint32_t)output_stream_fd;
    wasi_p2_native_fd_quota_transfer_to_host_resource(input_hr,
                                                      &input_fd_lease);
    wasi_p2_native_fd_quota_transfer_to_host_resource(output_hr,
                                                      &output_fd_lease);

    err = wasi_sockets_tcp_finish_connect_status(socket_fd);
    if (err != 0) {
        goto native_error;
    }

    if (!canonical_resource_transfer_scope_leave(&lower_scope, true)) {
        lower_scope_active = false;
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to publish TCP streams");
        goto rollback_resources;
    }
    lower_scope_active = false;
    owned_rep_count = 2;
    goto cleanup;

native_error:
    if (lower_scope_active) {
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
        lower_scope_active = false;
    }
    (void)host_resource_table_delete(hr_table, owned_reps[1]);
    (void)host_resource_table_delete(hr_table, owned_reps[0]);
    owned_reps[0] = owned_reps[1] = 0;
    free_wit_value(result->value.result_value.result.ok);
    error_payload->value.enum_value.value = errno_to_wasi_network(err);
    result->value.result_value.is_err = true;
    result->value.result_value.result.err = error_payload;
    error_payload = NULL;
    goto end;

rollback_resources:
    if (lower_scope_active) {
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
        lower_scope_active = false;
    }
    if (owned_reps[1] != 0) {
        (void)host_resource_table_delete(hr_table, owned_reps[1]);
    }
    if (owned_reps[0] != 0) {
        (void)host_resource_table_delete(hr_table, owned_reps[0]);
    }
    owned_reps[0] = owned_reps[1] = 0;
    goto cleanup;

end:
    if (!wasi_p2_store_owned_host_resource_result(
            exec_env, offset_addr, func_type->results->result, result,
            owned_reps, owned_rep_count)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to publish TCP connect result");
    }
cleanup:
    wasi_p2_native_fd_quota_release(&fd_lease);
    wasi_p2_native_fd_quota_release(&input_fd_lease);
    wasi_p2_native_fd_quota_release(&output_fd_lease);
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `start-listen` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_start_listen_wrapper(wasm_exec_env_t exec_env,
                                                 uint32_t socket_handle,
                                                 uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    // Get the actual socket fd from the host resource
    wasi_tcp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;
    uint64_t backlog = ((wasi_socket_context_t *)hr->data)->tcp_listen_backlog;
    if (!stage_unit_socket_success(exec_env, offset_addr,
                                   func_type->results->result, &result,
                                   &error_payload, &lower_scope)) {
        goto cleanup;
    }

    int err = wasi_sockets_tcp_start_listen(socket_fd, backlog);
    if (err == 0)
        ((wasi_socket_context_t *)hr->data)->tcp_is_listening = true;
    (void)finish_unit_socket_result(exec_env, offset_addr,
                                    func_type->results->result, result,
                                    &error_payload, &lower_scope, err,
                                    "failed to publish TCP listen result");
    goto cleanup;

end:
    if (result) {
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    }
cleanup:
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `finish-listen` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_finish_listen_wrapper(wasm_exec_env_t exec_env,
                                                  uint32_t socket_handle,
                                                  uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    // Get the actual socket fd from the host resource
    wasi_tcp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;
    int err = wasi_sockets_tcp_finish_listen(socket_fd);
    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
    }
    else {
        result = wit_result_ctor(false, NULL);
    }

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `accept` method of the `wasi:sockets/tcp.tcp-socket`
 * resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the listening TCP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_accept_wrapper(wasm_exec_env_t exec_env,
                                           uint32_t socket_handle,
                                           uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;
    wit_value_t error_result = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };
    bool lower_scope_active = false;
    uint32_t owned_reps[3] = { 0 };
    uint32_t owned_rep_count = 0;
    HostResource *socket_resource = NULL;
    HostResource *input_resource = NULL;
    HostResource *output_resource = NULL;
    wasi_socket_context_t *accepted_socket = NULL;
    StreamResourceType *accepted_input = NULL;
    StreamResourceType *accepted_output = NULL;
    HostResourceTable *hr_table = NULL;
    wasi_tcp_socket_t new_socket = -1;
    wasi_input_stream_t input_stream = -1;
    wasi_output_stream_t output_stream = -1;
    WasiP2NativeFdQuotaLease fd_lease = { 0 };
    WasiP2NativeFdQuotaLease socket_fd_lease = { 0 };
    WasiP2NativeFdQuotaLease input_fd_lease = { 0 };
    WasiP2NativeFdQuotaLease output_fd_lease = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    int err = 0;

    hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    /* Prepare the complete result graph and tentative owned handles before
       accept() can remove a connection from the listener queue. */
    error_payload = wit_enum_ctor(WASI_NETWORK_ERROR_CODE_UNKNOWN);
    if (error_payload)
        error_result = wit_result_ctor(true, error_payload);
    if (!error_result) {
        free_wit_value(error_payload);
        error_payload = NULL;
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "failed to allocate TCP accept error result");
        goto cleanup;
    }

    if (!wasi_p2_native_fd_quota_reserve(exec_env, 3, &fd_lease)
        || !wasi_p2_native_fd_quota_lease_take(&fd_lease, 1, &socket_fd_lease)
        || !wasi_p2_native_fd_quota_lease_take(&fd_lease, 1, &input_fd_lease)
        || !wasi_p2_native_fd_quota_lease_take(&fd_lease, 1,
                                               &output_fd_lease)) {
        error_payload->value.enum_value.value =
            WASI_NETWORK_ERROR_CODE_NEW_SOCKET_LIMIT;
        result = error_result;
        error_result = NULL;
        goto end;
    }

    socket_resource =
        host_resource_create(WASI_P2_TCP_SOCKET, sizeof(wasi_socket_context_t));
    input_resource = create_tcp_stream_resource(WASI_P2_IO_INPUT_STREAM, -1);
    output_resource = create_tcp_stream_resource(WASI_P2_IO_OUTPUT_STREAM, -1);
    if (!socket_resource || !input_resource || !output_resource)
        goto allocation_failed;

    ((wasi_socket_context_t *)socket_resource->data)->family =
        ((wasi_socket_context_t *)hr->data)->family;
    ((wasi_socket_context_t *)socket_resource->data)->fd = -1;
    ((wasi_socket_context_t *)socket_resource->data)->tcp_listen_backlog = 0;
    ((wasi_socket_context_t *)socket_resource->data)->tcp_is_listening = false;
    host_resource_set_dtor(socket_resource, tcp_socket_dtor);
    wasi_p2_native_fd_quota_transfer_to_host_resource(socket_resource,
                                                      &socket_fd_lease);
    wasi_p2_native_fd_quota_transfer_to_host_resource(input_resource,
                                                      &input_fd_lease);
    wasi_p2_native_fd_quota_transfer_to_host_resource(output_resource,
                                                      &output_fd_lease);
    accepted_socket = (wasi_socket_context_t *)socket_resource->data;
    accepted_input = (StreamResourceType *)input_resource->data;
    accepted_output = (StreamResourceType *)output_resource->data;

    owned_reps[0] = host_resource_table_add(hr_table, socket_resource);
    if (owned_reps[0] == 0)
        goto allocation_failed;
    owned_rep_count = 1;
    socket_resource = NULL;
    owned_reps[1] = host_resource_table_add(hr_table, input_resource);
    if (owned_reps[1] == 0)
        goto allocation_failed;
    owned_rep_count = 2;
    input_resource = NULL;
    owned_reps[2] = host_resource_table_add(hr_table, output_resource);
    if (owned_reps[2] == 0)
        goto allocation_failed;
    owned_rep_count = 3;
    output_resource = NULL;

    result = make_resource_tuple_result(owned_reps, 3);
    if (!result
        || !store(exec_env->cx, offset_addr, func_type->results->result,
                  error_result)
        || !canonical_resource_transfer_scope_enter(
            &lower_scope, exec_env->cx, WASM_COMPONENT_TABLE_TRANSACTION_LOWER))
        goto allocation_failed;
    lower_scope_active = true;
    if (!store(exec_env->cx, offset_addr, func_type->results->result, result)
        || !canonical_resource_transfer_scope_can_commit(&lower_scope))
        goto allocation_failed;

    wasi_sockets_tcp_accept(((wasi_socket_context_t *)hr->data)->fd,
                            &new_socket, &input_stream, &output_stream, &err);
    if (err != 0) {
        error_payload->value.enum_value.value = errno_to_wasi_network(err);
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
        lower_scope_active = false;
        for (uint32_t i = 0; i < owned_rep_count; i++) {
            (void)host_resource_table_delete(hr_table, owned_reps[i]);
            owned_reps[i] = 0;
        }
        owned_rep_count = 0;
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    error_result);
        goto cleanup;
    }

    accepted_socket->fd = new_socket;
    accepted_input->fd = input_stream;
    accepted_output->fd = output_stream;
    new_socket = input_stream = output_stream = -1;
    if (!canonical_resource_transfer_scope_leave(&lower_scope, true))
        goto allocation_failed;
    lower_scope_active = false;
    owned_rep_count = 0;
    goto cleanup;

allocation_failed:
    wasm_runtime_set_exception(exec_env->module_inst,
                               "failed to stage TCP accept result");
    if (lower_scope_active) {
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
        lower_scope_active = false;
    }
    for (uint32_t i = 0; i < owned_rep_count; i++) {
        if (owned_reps[i] != 0)
            (void)host_resource_table_delete(hr_table, owned_reps[i]);
        owned_reps[i] = 0;
    }
    owned_rep_count = 0;
end:
    (void)wasi_p2_store_owned_host_resource_result(
        exec_env, offset_addr, func_type->results->result, result, owned_reps,
        owned_rep_count);
cleanup:
    if (lower_scope_active)
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
    if (socket_resource)
        destroy_host_resource(socket_resource);
    if (input_resource)
        destroy_host_resource(input_resource);
    if (output_resource)
        destroy_host_resource(output_resource);
    if (new_socket >= 0)
        close(new_socket);
    if (input_stream >= 0)
        close(input_stream);
    if (output_stream >= 0)
        close(output_stream);
    wasi_p2_native_fd_quota_release(&fd_lease);
    wasi_p2_native_fd_quota_release(&socket_fd_lease);
    wasi_p2_native_fd_quota_release(&input_fd_lease);
    wasi_p2_native_fd_quota_release(&output_fd_lease);
    free_wit_value(error_result);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `local-address` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_local_address_wrapper(wasm_exec_env_t exec_env,
                                                  uint32_t socket_handle,
                                                  uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    wasi_ip_socket_address_t local_address;
    int err = 0;

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    // Get the actual socket fd from the host resource
    wasi_tcp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;
    wasi_sockets_tcp_local_address(socket_fd, &local_address, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
        goto end;
    }
    else {
        wit_value_t ip_sock_addr = get_ip_sock_addr_value(&local_address);
        result = make_owned_ok_result(ip_sock_addr);
        goto end;
    }
end:
    if (result) {
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    }
    else {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "failed to construct TCP local-address result");
    }
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `remote-address` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_remote_address_wrapper(wasm_exec_env_t exec_env,
                                                   uint32_t socket_handle,
                                                   uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    wasi_ip_socket_address_t remote_address;
    int err = 0;

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    // Get the actual socket fd from the host resource
    wasi_tcp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;
    wasi_sockets_tcp_remote_address(socket_fd, &remote_address, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
        goto end;
    }
    else {
        wit_value_t ip_sock_addr = get_ip_sock_addr_value(&remote_address);
        result = make_owned_ok_result(ip_sock_addr);
        goto end;
    }

end:
    if (result) {
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    }
    else {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "failed to construct TCP remote-address result");
    }
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `is-listening` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @return `true` if the socket is listening, `false` otherwise.
 */
uint32_t
wasi_sockets_tcp_tcp_socket_is_listening_wrapper(wasm_exec_env_t exec_env,
                                                 uint32_t socket_handle)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        return 0;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not lift tcp socket resource");
        return 0;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);
    free_wit_value(lifted_handle);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get tcp socket resource");
        return 0;
    }

    return ((wasi_socket_context_t *)hr->data)->tcp_is_listening;
}

/**
 * @brief Wrapper for the `address-family` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @return The IP address family of the socket.
 */
uint32_t
wasi_sockets_tcp_tcp_socket_address_family_wrapper(wasm_exec_env_t exec_env,
                                                   uint32_t socket_handle)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        return 0;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not lift tcp socket resource");
        return 0;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();

    HostResource *socket_hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);
    free_wit_value(lifted_handle);

    if (!socket_hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get tcp socket resource");
        return WASI_IP_ADDRESS_FAMILY_IPV4;
    }

    // Get the actual family from the host resource
    wasi_ip_address_family_t family =
        ((wasi_socket_context_t *)socket_hr->data)->family;
    return family;
}

/**
 * @brief Wrapper for the `set-listen-backlog-size` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param value The listen backlog size.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_set_listen_backlog_size_wrapper(
    wasm_exec_env_t exec_env, uint32_t socket_handle, int64_t value,
    uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    if (value <= 0) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    if (value > SOMAXCONN)
        value = SOMAXCONN;

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    if (((wasi_socket_context_t *)hr->data)->tcp_is_listening) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    (void)publish_socket_unit_mutation(
        exec_env, offset_addr, func_type->results->result,
        (wasi_socket_context_t *)hr->data, (uint64_t)value, set_tcp_backlog,
        "failed to publish TCP listen backlog result");
    goto cleanup;

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
cleanup:
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `keep-alive-enabled` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_keep_alive_enabled_wrapper(wasm_exec_env_t exec_env,
                                                       uint32_t socket_handle,
                                                       uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    bool enabled;
    int err = 0;

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    // Get the actual socket fd from the host resource
    wasi_tcp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;
    wasi_sockets_tcp_keep_alive_enabled(socket_fd, &enabled, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
    }
    else {
        result = make_owned_ok_result(wit_bool_ctor(enabled));
    }

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `set-keep-alive-enabled` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param value A boolean indicating whether to enable keep-alive.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_set_keep_alive_enabled_wrapper(
    wasm_exec_env_t exec_env, uint32_t socket_handle, uint32_t value,
    uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    (void)publish_socket_unit_mutation(
        exec_env, offset_addr, func_type->results->result,
        (wasi_socket_context_t *)hr->data, value, set_tcp_keep_alive_enabled,
        "failed to publish TCP keep-alive-enabled result");
    goto cleanup;

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
cleanup:
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `keep-alive-idle-time` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_keep_alive_idle_time_wrapper(
    wasm_exec_env_t exec_env, uint32_t socket_handle, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    wasi_duration_t time;
    int err = 0;

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    // Get the actual socket fd from the host resource
    wasi_tcp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;
    wasi_sockets_tcp_keep_alive_idle_time(socket_fd, &time, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
    }
    else {
        result = wit_u64_ctor(time);
    }

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `set-keep-alive-idle-time` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param value The keep-alive idle time.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_set_keep_alive_idle_time_wrapper(
    wasm_exec_env_t exec_env, uint32_t socket_handle, int64_t value,
    uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    (void)publish_socket_unit_mutation(
        exec_env, offset_addr, func_type->results->result,
        (wasi_socket_context_t *)hr->data, value, set_tcp_keep_alive_idle_time,
        "failed to publish TCP keep-alive-idle-time result");
    goto cleanup;

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
cleanup:
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `keep-alive-interval` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_keep_alive_interval_wrapper(
    wasm_exec_env_t exec_env, uint32_t socket_handle, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    wasi_duration_t time;
    int err = 0;

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    // Get the actual socket fd from the host resource
    wasi_tcp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;
    wasi_sockets_tcp_keep_alive_interval(socket_fd, &time, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
        goto end;
    }
    else {
        result = wit_u64_ctor(time);
    }

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `set-keep-alive-interval` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param value The keep-alive interval.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_set_keep_alive_interval_wrapper(
    wasm_exec_env_t exec_env, uint32_t socket_handle, int64_t value,
    uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    (void)publish_socket_unit_mutation(
        exec_env, offset_addr, func_type->results->result,
        (wasi_socket_context_t *)hr->data, value, set_tcp_keep_alive_interval,
        "failed to publish TCP keep-alive-interval result");
    goto cleanup;

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
cleanup:
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `keep-alive-count` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_keep_alive_count_wrapper(wasm_exec_env_t exec_env,
                                                     uint32_t socket_handle,
                                                     uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    uint32_t count;
    int err = 0;

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    // Get the actual socket fd from the host resource
    wasi_tcp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;
    wasi_sockets_tcp_keep_alive_count(socket_fd, &count, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
    }
    else {
        result = make_owned_ok_result(wit_u32_ctor(count));
    }

end:
    if (result) {
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    }
    else {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "failed to construct TCP keep-alive-count result");
    }
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `set-keep-alive-count` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param value The keep-alive count.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_set_keep_alive_count_wrapper(
    wasm_exec_env_t exec_env, uint32_t socket_handle, uint32_t value,
    uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    (void)publish_socket_unit_mutation(
        exec_env, offset_addr, func_type->results->result,
        (wasi_socket_context_t *)hr->data, value, set_tcp_keep_alive_count,
        "failed to publish TCP keep-alive-count result");
    goto cleanup;

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
cleanup:
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `hop-limit` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_hop_limit_wrapper(wasm_exec_env_t exec_env,
                                              uint32_t socket_handle,
                                              uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    uint8_t limit;
    int err = 0;

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    // Get the actual socket fd from the host resource
    wasi_tcp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;
    wasi_sockets_tcp_hop_limit(socket_fd, &limit, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
    }
    else {
        result = make_owned_ok_result(wit_u8_ctor(limit));
    }

end:
    if (result) {
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    }
    else {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to construct TCP hop-limit result");
    }
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `set-hop-limit` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param value The hop limit.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_set_hop_limit_wrapper(wasm_exec_env_t exec_env,
                                                  uint32_t socket_handle,
                                                  uint32_t value,
                                                  uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    (void)publish_socket_unit_mutation(
        exec_env, offset_addr, func_type->results->result,
        (wasi_socket_context_t *)hr->data, value, set_tcp_hop_limit,
        "failed to publish TCP hop-limit result");
    goto cleanup;

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
cleanup:
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `receive-buffer-size` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_receive_buffer_size_wrapper(
    wasm_exec_env_t exec_env, uint32_t socket_handle, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    uint64_t size;
    int err = 0;

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    // Get the actual socket fd from the host resource
    wasi_tcp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;
    wasi_sockets_tcp_receive_buffer_size(socket_fd, &size, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
    }
    else {
        result = make_owned_ok_result(wit_u64_ctor(size));
    }

end:
    if (result) {
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    }
    else {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "failed to construct TCP receive-buffer-size result");
    }
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `set-receive-buffer-size` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param value The receive buffer size.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_set_receive_buffer_size_wrapper(
    wasm_exec_env_t exec_env, uint32_t socket_handle, int64_t value,
    uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    (void)publish_socket_unit_mutation(
        exec_env, offset_addr, func_type->results->result,
        (wasi_socket_context_t *)hr->data, value, set_tcp_receive_buffer_size,
        "failed to publish TCP receive-buffer-size result");
    goto cleanup;

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
cleanup:
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `send-buffer-size` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_send_buffer_size_wrapper(wasm_exec_env_t exec_env,
                                                     uint32_t socket_handle,
                                                     uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    uint64_t size;
    int err = 0;

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    // Get the actual socket fd from the host resource
    wasi_tcp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;
    wasi_sockets_tcp_send_buffer_size(socket_fd, &size, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
    }
    else {
        result = make_owned_ok_result(wit_u64_ctor(size));
    }

end:
    if (result) {
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    }
    else {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "failed to construct TCP send-buffer-size result");
    }
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `set-send-buffer-size` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param value The send buffer size.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_set_send_buffer_size_wrapper(
    wasm_exec_env_t exec_env, uint32_t socket_handle, int64_t value,
    uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    (void)publish_socket_unit_mutation(
        exec_env, offset_addr, func_type->results->result,
        (wasi_socket_context_t *)hr->data, value, set_tcp_send_buffer_size,
        "failed to publish TCP send-buffer-size result");
    goto cleanup;

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
cleanup:
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `subscribe` method of the
 * `wasi:sockets/tcp.tcp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The rep of the TCP socket resource.
 * @return A new pollable resource rep.
 */
uint32_t
wasi_sockets_tcp_tcp_socket_subscribe_wrapper(wasm_exec_env_t exec_env,
                                              uint32_t socket_handle)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        return 0;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle))
        return 0;

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);
    free_wit_value(lifted_handle);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get tcp socket resource");
        return 0;
    }

    // Get the actual socket fd from the host resource
    wasi_tcp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;
    HostResource *hr_poll =
        host_resource_create(WASI_P2_POLLABLE, sizeof(wasi_pollable_context_t));

    if (!hr_poll) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not create pollable stream resource");
        return 0;
    }

    if (!init_owned_fd_pollable(exec_env, hr_poll, socket_fd,
                                WASI_POLLABLE_SOCK)) {
        destroy_host_resource(hr_poll);
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not duplicate tcp socket fd");
        return 0;
    }
    host_resource_set_dtor(hr_poll, pollable_dtor);

    uint32_t index_rep = host_resource_table_add(hr_table, hr_poll);
    if (index_rep < 1) {
        destroy_host_resource(hr_poll); // Clean up the HostResource on failure
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not add output stream resource to HR table");
        return 0;
    }

    if (!lower_owned_host_resource(
            exec_env,
            func_type->results->result[0].type_specific.resource_handle,
            hr_table, index_rep, &index_rep)) {
        return 0;
    }
    return index_rep;
}

/**
 * @brief Wrapper for the `shutdown` method of the `wasi:sockets/tcp.tcp-socket`
 * resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the TCP socket.
 * @param shutdown_type The type of shutdown to perform.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_tcp_tcp_socket_shutdown_wrapper(wasm_exec_env_t exec_env,
                                             uint32_t socket_handle,
                                             uint32_t shutdown_type,
                                             uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->tcp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    // Get the actual socket fd from the host resource
    wasi_tcp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;
    if (!stage_unit_socket_success(exec_env, offset_addr,
                                   func_type->results->result, &result,
                                   &error_payload, &lower_scope)) {
        goto cleanup;
    }

    int err = wasi_sockets_tcp_shutdown(socket_fd, shutdown_type);
    (void)finish_unit_socket_result(exec_env, offset_addr,
                                    func_type->results->result, result,
                                    &error_payload, &lower_scope, err,
                                    "failed to publish TCP shutdown result");
    goto cleanup;

end:
    if (result) {
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    }
cleanup:
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/* wasi:sockets/udp-create-socket */

/**
 * @brief Wrapper for the `create-udp-socket` function of the
 * `wasi:sockets/udp-create-socket` interface.
 * @param exec_env The execution environment.
 * @param address_family The IP address family.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_udp_create_socket_create_udp_socket_wrapper(
    wasm_exec_env_t exec_env, uint32_t address_family, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t result = NULL;
    uint32_t owned_rep = 0;
    WasiP2NativeFdQuotaLease fd_lease = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->udp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!wasi_p2_native_fd_quota_reserve(exec_env, 1, &fd_lease)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NEW_SOCKET_LIMIT);
        goto end;
    }

    wasi_udp_socket_t socket;
    int err = 0;

    wasi_sockets_create_udp_socket(address_family, &socket, &err);

    if (err != 0) {
        wasi_p2_native_fd_quota_release(&fd_lease);
        result = get_result_error_val(errno_to_wasi_network(err));
        goto end;
    }
    else {
        HostResourceTable *hr_table = get_global_host_resource_table();
        HostResource *hr = host_resource_create(WASI_P2_UDP_SOCKET,
                                                sizeof(wasi_socket_context_t));

        if (!hr) {
            close(socket);
            wasi_p2_native_fd_quota_release(&fd_lease);
            result =
                get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
            goto end;
        }

        ((wasi_socket_context_t *)hr->data)->family = address_family;
        ((wasi_socket_context_t *)hr->data)->fd = socket;
        ((wasi_socket_context_t *)hr->data)->tcp_listen_backlog = 0;
        ((wasi_socket_context_t *)hr->data)->tcp_is_listening = false;
        host_resource_set_dtor(hr, udp_socket_dtor);
        wasi_p2_native_fd_quota_transfer_to_host_resource(hr, &fd_lease);

        uint32_t index_rep = host_resource_table_add(hr_table, hr);
        if (index_rep < 1) {
            destroy_host_resource(hr); // Clean up the HostResource on failure
            result =
                get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
            goto end;
        }
        result = make_owned_resource_result(index_rep);
        if (!result) {
            (void)host_resource_table_delete(hr_table, index_rep);
            result =
                get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
        }
        else {
            owned_rep = index_rep;
        }
        goto end;
    }
end:
    wasi_p2_native_fd_quota_release(&fd_lease);
    (void)wasi_p2_store_owned_host_resource_result(
        exec_env, offset_addr, func_type->results->result, result, &owned_rep,
        owned_rep != 0 ? 1 : 0);
    free_wit_value(result);
}

/* wasi:sockets/udp */

/**
 * @brief Wrapper for the `start-bind` method of the
 * `wasi:sockets/udp.udp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the UDP socket.
 * @param network_handle The handle of the network resource.
 * @param addr_tag The tag of the IP socket address.
 * @param ... The flattened fields of the IP socket address.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_udp_udp_socket_start_bind_wrapper(
    wasm_exec_env_t exec_env, uint32_t socket_handle, uint32_t network_handle,
    uint32_t addr_tag, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3,
    uint32_t p4, uint32_t p5, uint32_t p6, uint32_t p7, uint32_t p8,
    uint32_t p9, uint32_t p10, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_network_handle = NULL;
    wit_value_t lifted_socket_handle = NULL;
    wit_value_t result = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->udp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, network_handle,
            func_type->params->params[1].type->type_specific.resource_handle,
            &lifted_network_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_socket_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    wasi_ip_socket_address_t local_address;
    int err = 0;

    HostResourceTable *hr_table = get_global_host_resource_table();

    HostResource *socket_hr = host_resource_table_get(
        hr_table, lifted_socket_handle->value.resource_value.value);
    HostResource *network_hr = host_resource_table_get(
        hr_table, lifted_network_handle->value.resource_value.value);

    if (!(socket_hr && network_hr)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    // Get the actual socket fd from the host resource
    wasi_network_t network_fd = *((uint32_t *)network_hr->data);
    wasi_udp_socket_t socket_fd =
        ((wasi_socket_context_t *)socket_hr->data)->fd;

    read_flattened_ip_socket_address(&local_address, addr_tag, p0, p1, p2, p3,
                                     p4, p5, p6, p7, p8, p9, p10);
    if (!stage_unit_socket_success(exec_env, offset_addr,
                                   func_type->results->result, &result,
                                   &error_payload, &lower_scope)) {
        goto cleanup;
    }

    err = wasi_sockets_udp_start_bind(socket_fd, network_fd, &local_address);
    (void)finish_unit_socket_result(
        exec_env, offset_addr, func_type->results->result, result,
        &error_payload, &lower_scope, err, "failed to publish UDP bind result");
    goto cleanup;

end:
    if (result) {
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    }
cleanup:
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_network_handle);
    free_wit_value(lifted_socket_handle);
}

/**
 * @brief Wrapper for the `finish-bind` method of the
 * `wasi:sockets/udp.udp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the UDP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_udp_udp_socket_finish_bind_wrapper(wasm_exec_env_t exec_env,
                                                uint32_t socket_handle,
                                                uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *socket_hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!socket_hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    // Get the actual socket fd from the host resource
    wasi_udp_socket_t socket_fd =
        ((wasi_socket_context_t *)socket_hr->data)->fd;

    int err = wasi_sockets_udp_finish_bind(socket_fd);
    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
    }
    else {
        result = wit_result_ctor(false, NULL);
    }

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `stream` method of the `wasi:sockets/udp.udp-socket`
 * resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the UDP socket.
 * @param remote_addr_tag The tag of the optional remote address.
 * @param ... The flattened fields of the optional remote address.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_udp_udp_socket_stream_wrapper(
    wasm_exec_env_t exec_env, uint32_t socket_handle, uint32_t remote_addr_tag,
    uint32_t addr_tag, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3,
    uint32_t p4, uint32_t p5, uint32_t p6, uint32_t p7, uint32_t p8,
    uint32_t p9, uint32_t p10, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;
    uint32_t owned_reps[2] = { 0 };
    uint32_t owned_rep_count = 0;
    wasi_udp_peer_state_t previous_peer;
    bool peer_must_restore = false;
    bool published = false;
    WasiP2NativeFdQuotaLease fd_lease = { 0 };
    WasiP2NativeFdQuotaLease input_fd_lease = { 0 };
    WasiP2NativeFdQuotaLease output_fd_lease = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->udp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    wasi_ip_socket_address_t remote_address;
    wasi_incoming_datagram_stream_t incoming_stream;
    wasi_outgoing_datagram_stream_t outgoing_stream;
    int err = 0;

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *socket_hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!socket_hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    wasi_udp_socket_t socket_fd =
        ((wasi_socket_context_t *)socket_hr->data)->fd;
    const wasi_ip_socket_address_t *remote_address_ptr = NULL;

    if (remote_addr_tag) {
        read_flattened_ip_socket_address(&remote_address, addr_tag, p0, p1, p2,
                                         p3, p4, p5, p6, p7, p8, p9, p10);
        remote_address_ptr = &remote_address;
        err = wasi_sockets_udp_capture_peer(socket_fd, &previous_peer);
        if (err != 0) {
            result = get_result_error_val(errno_to_wasi_network(err));
            goto end;
        }
    }

    if (!wasi_p2_native_fd_quota_reserve(exec_env, 2, &fd_lease)
        || !wasi_p2_native_fd_quota_lease_take(&fd_lease, 1, &input_fd_lease)
        || !wasi_p2_native_fd_quota_lease_take(&fd_lease, 1,
                                               &output_fd_lease)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NEW_SOCKET_LIMIT);
        goto end;
    }

    wasi_sockets_udp_stream(socket_fd, remote_address_ptr, &incoming_stream,
                            &outgoing_stream, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
        goto end;
    }
    else {
        peer_must_restore = remote_address_ptr != NULL;
        uint32_t incoming_rep = add_udp_datagram_stream_resource(
            hr_table, WASI_P2_UDP_INCOMING_DATAGRAM_STREAM, incoming_stream,
            &input_fd_lease);
        if (incoming_rep == 0) {
            close(outgoing_stream);
            wasi_p2_native_fd_quota_release(&output_fd_lease);
            result =
                get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
            goto end;
        }

        uint32_t outgoing_rep = add_udp_datagram_stream_resource(
            hr_table, WASI_P2_UDP_OUTGOING_DATAGRAM_STREAM, outgoing_stream,
            &output_fd_lease);
        if (outgoing_rep == 0) {
            host_resource_table_delete(hr_table, incoming_rep);
            result =
                get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
            goto end;
        }

        uint32_t reps[] = { incoming_rep, outgoing_rep };
        result = make_resource_tuple_result(reps, 2);
        if (!result) {
            host_resource_table_delete(hr_table, outgoing_rep);
            host_resource_table_delete(hr_table, incoming_rep);
            result =
                get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
        }
        else {
            owned_reps[0] = incoming_rep;
            owned_reps[1] = outgoing_rep;
            owned_rep_count = 2;
        }
        goto end;
    }

end:
    published = wasi_p2_store_owned_host_resource_result(
        exec_env, offset_addr, func_type->results->result, result, owned_reps,
        owned_rep_count);
    published = published && owned_rep_count == 2;
    if (peer_must_restore && !published
        && wasi_sockets_udp_restore_peer(socket_fd, &previous_peer) != 0) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not restore udp socket peer");
    }
    wasi_p2_native_fd_quota_release(&fd_lease);
    wasi_p2_native_fd_quota_release(&input_fd_lease);
    wasi_p2_native_fd_quota_release(&output_fd_lease);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `local-address` method of the
 * `wasi:sockets/udp.udp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the UDP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_udp_udp_socket_local_address_wrapper(wasm_exec_env_t exec_env,
                                                  uint32_t socket_handle,
                                                  uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->udp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    wasi_ip_socket_address_t local_address;
    int err = 0;

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    wasi_udp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;
    wasi_sockets_udp_local_address(socket_fd, &local_address, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
    }
    else {
        wit_value_t ip_sock_addr = get_ip_sock_addr_value(&local_address);
        result = make_owned_ok_result(ip_sock_addr);
        goto end;
    }

end:
    if (result) {
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    }
    else {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "failed to construct UDP local-address result");
    }
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `remote-address` method of the
 * `wasi:sockets/udp.udp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the UDP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_udp_udp_socket_remote_address_wrapper(wasm_exec_env_t exec_env,
                                                   uint32_t socket_handle,
                                                   uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->udp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    wasi_ip_socket_address_t remote_address;
    int err = 0;

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    wasi_udp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;
    wasi_sockets_udp_remote_address(socket_fd, &remote_address, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
        goto end;
    }
    else {
        wit_value_t ip_sock_addr = get_ip_sock_addr_value(&remote_address);
        result = make_owned_ok_result(ip_sock_addr);

        goto end;
    }

end:
    if (result) {
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    }
    else {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "failed to construct UDP remote-address result");
    }
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `address-family` method of the
 * `wasi:sockets/udp.udp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the UDP socket.
 * @return The IP address family of the socket.
 */
uint32_t
wasi_sockets_udp_udp_socket_address_family_wrapper(wasm_exec_env_t exec_env,
                                                   uint32_t socket_handle)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->udp) {
        return 0;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();

    HostResource *socket_hr = host_resource_table_get(hr_table, socket_handle);

    if (socket_hr) {
        // Get the actual family from the host resource
        wasi_ip_address_family_t family =
            ((wasi_socket_context_t *)socket_hr->data)->family;
        return family;
    }

    // According to the wit, this should not fail
    return WASI_IP_ADDRESS_FAMILY_IPV4;
}

/**
 * @brief Wrapper for the `unicast-hop-limit` method of the
 * `wasi:sockets/udp.udp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the UDP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_udp_udp_socket_unicast_hop_limit_wrapper(wasm_exec_env_t exec_env,
                                                      uint32_t socket_handle,
                                                      uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->udp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    uint8_t limit;
    int err = 0;

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    wasi_udp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;
    wasi_sockets_udp_unicast_hop_limit(socket_fd, &limit, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
    }
    else {
        result = make_owned_ok_result(wit_u8_ctor(limit));
    }

end:
    if (result) {
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    }
    else {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "failed to construct UDP unicast-hop-limit result");
    }
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `set-unicast-hop-limit` method of the
 * `wasi:sockets/udp.udp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the UDP socket.
 * @param value The unicast hop limit.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_udp_udp_socket_set_unicast_hop_limit_wrapper(
    wasm_exec_env_t exec_env, uint32_t socket_handle, uint32_t value,
    uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->udp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    (void)publish_socket_unit_mutation(
        exec_env, offset_addr, func_type->results->result,
        (wasi_socket_context_t *)hr->data, value, set_udp_unicast_hop_limit,
        "failed to publish UDP unicast-hop-limit result");
    goto cleanup;

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
cleanup:
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `receive-buffer-size` method of the
 * `wasi:sockets/udp.udp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the UDP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_udp_udp_socket_receive_buffer_size_wrapper(
    wasm_exec_env_t exec_env, uint32_t socket_handle, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->udp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    uint64_t size;
    int err = 0;

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    wasi_udp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;
    wasi_sockets_udp_receive_buffer_size(socket_fd, &size, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
    }
    else {
        result = make_owned_ok_result(wit_u64_ctor(size));
    }

end:
    if (result) {
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    }
    else {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "failed to construct UDP receive-buffer-size result");
    }
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `set-receive-buffer-size` method of the
 * `wasi:sockets/udp.udp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the UDP socket.
 * @param value The receive buffer size.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_udp_udp_socket_set_receive_buffer_size_wrapper(
    wasm_exec_env_t exec_env, uint32_t socket_handle, int64_t value,
    uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->udp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    (void)publish_socket_unit_mutation(
        exec_env, offset_addr, func_type->results->result,
        (wasi_socket_context_t *)hr->data, (uint64_t)value,
        set_udp_receive_buffer_size,
        "failed to publish UDP receive-buffer-size result");
    goto cleanup;

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
cleanup:
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `send-buffer-size` method of the
 * `wasi:sockets/udp.udp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the UDP socket.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_udp_udp_socket_send_buffer_size_wrapper(wasm_exec_env_t exec_env,
                                                     uint32_t socket_handle,
                                                     uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->udp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    uint64_t size;
    int err = 0;

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    wasi_udp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;
    wasi_sockets_udp_send_buffer_size(socket_fd, &size, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
    }
    else {
        result = make_owned_ok_result(wit_u64_ctor(size));
    }

end:
    if (result) {
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    }
    else {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "failed to construct UDP send-buffer-size result");
    }
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `set-send-buffer-size` method of the
 * `wasi:sockets/udp.udp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the UDP socket.
 * @param value The send buffer size.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_udp_udp_socket_set_send_buffer_size_wrapper(
    wasm_exec_env_t exec_env, uint32_t socket_handle, int64_t value,
    uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->udp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    (void)publish_socket_unit_mutation(
        exec_env, offset_addr, func_type->results->result,
        (wasi_socket_context_t *)hr->data, (uint64_t)value,
        set_udp_send_buffer_size,
        "failed to publish UDP send-buffer-size result");
    goto cleanup;

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
cleanup:
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `subscribe` method of the
 * `wasi:sockets/udp.udp-socket` resource.
 * @param exec_env The execution environment.
 * @param socket_handle The handle of the UDP socket.
 * @return A new pollable handle.
 */
uint32_t
wasi_sockets_udp_udp_socket_subscribe_wrapper(wasm_exec_env_t exec_env,
                                              uint32_t socket_handle)
{
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;

    if (!lift_borrow(
            exec_env->cx, socket_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not lift udp socket resource");
        return 0;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);
    free_wit_value(lifted_handle);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get udp socket resource");
        return 0;
    }

    wasi_udp_socket_t socket_fd = ((wasi_socket_context_t *)hr->data)->fd;

    HostResource *hr_poll =
        host_resource_create(WASI_P2_POLLABLE, sizeof(wasi_pollable_context_t));

    if (!hr_poll) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not create pollable stream resource");
        return 0;
    }

    if (!init_owned_fd_pollable(exec_env, hr_poll, socket_fd,
                                WASI_POLLABLE_SOCK)) {
        destroy_host_resource(hr_poll);
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not duplicate udp socket fd");
        return 0;
    }
    host_resource_set_dtor(hr_poll, pollable_dtor);

    uint32_t index_rep = host_resource_table_add(hr_table, hr_poll);
    if (index_rep < 1) {
        destroy_host_resource(hr_poll); // Clean up the HostResource on failure
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not add output stream resource to HR table");
        return 0;
    }

    if (!lower_owned_host_resource(
            exec_env,
            func_type->results->result[0].type_specific.resource_handle,
            hr_table, index_rep, &index_rep)) {
        return 0;
    }
    return index_rep;
}

/**
 * @brief Wrapper for the `receive` method of the
 * `wasi:sockets/udp.incoming-datagram-stream` resource.
 * @param exec_env The execution environment.
 * @param stream_handle The handle of the incoming datagram stream.
 * @param max_results The maximum number of datagrams to receive.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_udp_incoming_datagram_stream_receive_wrapper(
    wasm_exec_env_t exec_env, uint32_t stream_handle, int64_t max_results,
    uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;
    wasi_incoming_datagram_t datagram = { 0 };
    wit_value_t *elems = NULL;
    bool datagram_present = false;
    bool success_result = false;
    wasi_udp_socket_t stream_fd = -1;
    int err = 0;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->udp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    stream_fd = ((wasi_socket_context_t *)hr->data)->fd;

    /* A WIT list may contain fewer elements than max-results. Peek one packet
       so every quota-sensitive constructor and guest-memory store completes
       while the datagram is still queued. The component operation gate
       serializes aliases of this stream until the final allocation-free
       consume. */
    if (max_results != 0) {
        wasi_sockets_udp_peek_receive(stream_fd, &datagram, &datagram_present,
                                      &err);
        if (err != 0) {
            result = get_result_error_val(errno_to_wasi_network(err));
            goto end;
        }
    }
    if (datagram_present) {
        elems = wasm_runtime_calloc(1, sizeof(wit_value_t));
        if (!elems) {
            result =
                get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
            goto end;
        }
        elems[0] = get_incoming_datagram_value(&datagram);
        if (!elems[0]) {
            result =
                get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
            goto end;
        }
    }
    {
        wit_value_t data = wit_list_ctor(elems, datagram_present ? 1 : 0);
        if (!data) {
            result =
                get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
            goto end;
        }
        elems = NULL;
        result = wit_result_ctor(false, data);
        if (!result)
            free_wit_value(data);
        else
            success_result = true;
    }

end:
    if (result) {
        if (!store(exec_env->cx, offset_addr, func_type->results->result,
                   result)) {
            wasm_runtime_set_exception(exec_env->module_inst,
                                       "failed to publish UDP receive result");
        }
        else if (success_result && datagram_present
                 && (err = wasi_sockets_udp_consume_peeked(stream_fd)) != 0) {
            wasm_runtime_set_exception(exec_env->module_inst,
                                       "failed to consume UDP datagram");
        }
    }
    else {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to allocate UDP receive result");
    }
    if (elems) {
        free_wit_value(elems[0]);
        wasm_runtime_free(elems);
    }
    if (datagram.data)
        wasm_runtime_free(datagram.data);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `subscribe` method of the
 * `wasi:sockets/udp.incoming-datagram-stream` resource.
 * @param exec_env The execution environment.
 * @param stream_handle The handle of the incoming datagram stream.
 * @return A new pollable handle.
 */
uint32_t
wasi_sockets_udp_incoming_datagram_stream_subscribe_wrapper(
    wasm_exec_env_t exec_env, uint32_t stream_handle)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->udp) {
        return 0;
    }

    wit_value_t lifted_handle = NULL;

    if (!lift_borrow(
            exec_env->cx, stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        return 0;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);
    free_wit_value(lifted_handle);

    if (!hr) {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not get udp stream incoming resource");
        return 0;
    }

    wasi_udp_socket_t stream_fd = ((wasi_socket_context_t *)hr->data)->fd;

    HostResource *hr_poll =
        host_resource_create(WASI_P2_POLLABLE, sizeof(wasi_pollable_context_t));

    if (!hr_poll) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not create new pollable resource");
        return 0;
    }

    if (!init_owned_fd_pollable(exec_env, hr_poll, stream_fd,
                                WASI_POLLABLE_IN)) {
        destroy_host_resource(hr_poll);
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not duplicate udp incoming stream fd");
        return 0;
    }
    host_resource_set_dtor(hr_poll, pollable_dtor);

    uint32_t index_rep = host_resource_table_add(hr_table, hr_poll);
    if (index_rep < 1) {
        destroy_host_resource(hr_poll); // Clean up the HostResource on failure
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not add pollable resource to HR table");
        return 0;
    }

    if (!lower_owned_host_resource(
            exec_env,
            func_type->results->result[0].type_specific.resource_handle,
            hr_table, index_rep, &index_rep)) {
        return 0;
    }
    return index_rep;
}

/**
 * @brief Wrapper for the `check-send` method of the
 * `wasi:sockets/udp.outgoing-datagram-stream` resource.
 * @param exec_env The execution environment.
 * @param stream_handle The handle of the outgoing datagram stream.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_udp_outgoing_datagram_stream_check_send_wrapper(
    wasm_exec_env_t exec_env, uint32_t stream_handle, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->udp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    uint64_t count;
    int err = 0;

    if (!lift_borrow(
            exec_env->cx, stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    wasi_udp_socket_t stream_fd = ((wasi_socket_context_t *)hr->data)->fd;

    wasi_sockets_udp_check_send(stream_fd, &count, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_network(err));
    }
    else {
        result = make_owned_ok_result(wit_u64_ctor(count));
    }

end:
    if (result) {
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    }
    else {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to construct UDP check-send result");
    }
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `send` method of the
 * `wasi:sockets/udp.outgoing-datagram-stream` resource.
 * @param exec_env The execution environment.
 * @param stream_handle The handle of the outgoing datagram stream.
 * @param datagrams_ptr A pointer to a list of outgoing datagrams in the guest's
 * memory.
 * @param datagrams_len The number of datagrams in the list.
 * @param[out] offset_addr Memory offset where the result will be stored.
 */
void
wasi_sockets_udp_outgoing_datagram_stream_send_wrapper(wasm_exec_env_t exec_env,
                                                       uint32_t stream_handle,
                                                       uint32_t datagrams_ptr,
                                                       uint32_t datagrams_len,
                                                       uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t result = NULL;
    wit_value_t lifted_handle = NULL;
    wit_value_t sent_value = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };
    bool lower_scope_active = false;
    bool store_result_at_end = true;

    wasi_outgoing_datagram_t *datagrams_native = NULL;
    uint64_t i = 0;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->udp) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED);
        goto end;
    }

    uint64_t sent_count = 0;
    int err = 0;
    wit_value_t datagrams_val = NULL;

    if (datagrams_len > UIO_MAXIOV
        || datagrams_len > UINT32_MAX / sizeof(wasi_outgoing_datagram_t)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }
    if (!load_list_from_range(
            exec_env->cx, datagrams_ptr, datagrams_len,
            func_type->params->params[1].type->type_specific.list->element_type,
            &datagrams_val)
        || !datagrams_val || datagrams_val->type != COMPONENT_VAL_TYPE_LIST
        || datagrams_len != datagrams_val->value.list_value.size) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    if (datagrams_len == 0) {
        wit_value_t sent = wit_u64_ctor(0);
        if (sent) {
            result = wit_result_ctor(false, sent);
            if (!result)
                free_wit_value(sent);
        }
        goto end;
    }

    datagrams_native =
        wasm_runtime_calloc(datagrams_len, sizeof(wasi_outgoing_datagram_t));
    if (!datagrams_native) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
        goto end;
    }

    for (i = 0; i < datagrams_val->value.list_value.size; i++) {
        wit_value_t datagram_value = datagrams_val->value.list_value.elems[i];
        ComponentWITRecord outgoing_datagram;
        wit_value_t data_value;
        wit_value_t remote_option;
        uint32_t data_size;
        uint32_t data_idx;

        if (!datagram_value || datagram_value->type != COMPONENT_VAL_TYPE_RECORD
            || datagram_value->value.record_value.size < 2) {
            result =
                get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
            goto end;
        }
        outgoing_datagram = datagram_value->value.record_value;
        data_value = outgoing_datagram.fields[0].value;
        remote_option = outgoing_datagram.fields[1].value;
        if (!data_value || data_value->type != COMPONENT_VAL_TYPE_LIST
            || !remote_option
            || remote_option->type != COMPONENT_VAL_TYPE_OPTION) {
            result =
                get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
            goto end;
        }
        data_size = data_value->value.list_value.size;
        if (data_size > UINT16_MAX) {
            result =
                get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
            goto end;
        }
        if (data_size > 0) {
            datagrams_native[i].data = wasm_runtime_malloc(data_size);
            if (!datagrams_native[i].data) {
                result =
                    get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
                goto end;
            }
        }
        for (data_idx = 0; data_idx < data_size; data_idx++) {
            wit_value_t byte = data_value->value.list_value.elems[data_idx];

            if (!byte || byte->type != COMPONENT_VAL_TYPE_PRIMVAL
                || byte->prim_type != WASM_COMP_PRIMVAL_U8) {
                result = get_result_error_val(
                    WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
                goto end;
            }
            datagrams_native[i].data[data_idx] = byte->value.u8_value;
        }
        datagrams_native[i].data_len = data_size;

        if (remote_option->value.option_value.optional_elem) {
            wit_value_t remote_value =
                remote_option->value.option_value.optional_elem;
            ComponentWITVariant remote_address;
            ComponentWITRecord address_record;
            ComponentWITTuple address_tuple;

            if (remote_value->type != COMPONENT_VAL_TYPE_VARIANT) {
                result = get_result_error_val(
                    WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
                goto end;
            }
            remote_address = remote_value->value.variant_value;
            if (!remote_address.value
                || remote_address.value->type != COMPONENT_VAL_TYPE_RECORD) {
                result = get_result_error_val(
                    WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
                goto end;
            }
            address_record = remote_address.value->value.record_value;
            datagrams_native[i].remote_address =
                wasm_runtime_calloc(1, sizeof(wasi_ip_socket_address_t));
            if (!datagrams_native[i].remote_address) {
                result =
                    get_result_error_val(WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
                goto end;
            }
            if (remote_address.discriminator_size == 4
                && !memcmp(remote_address.discriminator, "ipv4", 4)) {
                if (address_record.size < 2 || !address_record.fields[0].value
                    || !address_record.fields[1].value
                    || address_record.fields[1].value->type
                           != COMPONENT_VAL_TYPE_TUPLE
                    || address_record.fields[1].value->value.tuple_value.size
                           != 4) {
                    result = get_result_error_val(
                        WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
                    goto end;
                }
                address_tuple =
                    address_record.fields[1].value->value.tuple_value;
                datagrams_native[i].remote_address->tag =
                    WASI_IP_SOCKET_ADDRESS_TAG_IPV4;
                datagrams_native[i].remote_address->val.ipv4.port =
                    address_record.fields[0].value->value.u16_value;
                datagrams_native[i].remote_address->val.ipv4.address.f0 =
                    address_tuple.elems[0]->value.u8_value;
                datagrams_native[i].remote_address->val.ipv4.address.f1 =
                    address_tuple.elems[1]->value.u8_value;
                datagrams_native[i].remote_address->val.ipv4.address.f2 =
                    address_tuple.elems[2]->value.u8_value;
                datagrams_native[i].remote_address->val.ipv4.address.f3 =
                    address_tuple.elems[3]->value.u8_value;
            }
            else if (remote_address.discriminator_size == 4
                     && !memcmp(remote_address.discriminator, "ipv6", 4)) {
                if (address_record.size < 4 || !address_record.fields[0].value
                    || !address_record.fields[1].value
                    || !address_record.fields[2].value
                    || !address_record.fields[3].value
                    || address_record.fields[2].value->type
                           != COMPONENT_VAL_TYPE_TUPLE
                    || address_record.fields[2].value->value.tuple_value.size
                           != 8) {
                    result = get_result_error_val(
                        WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
                    goto end;
                }
                address_tuple =
                    address_record.fields[2].value->value.tuple_value;
                datagrams_native[i].remote_address->tag =
                    WASI_IP_SOCKET_ADDRESS_TAG_IPV6;
                datagrams_native[i].remote_address->val.ipv6.port =
                    address_record.fields[0].value->value.u16_value;
                datagrams_native[i].remote_address->val.ipv6.flow_info =
                    address_record.fields[1].value->value.u32_value;
                datagrams_native[i].remote_address->val.ipv6.address.f0 =
                    address_tuple.elems[0]->value.u16_value;
                datagrams_native[i].remote_address->val.ipv6.address.f1 =
                    address_tuple.elems[1]->value.u16_value;
                datagrams_native[i].remote_address->val.ipv6.address.f2 =
                    address_tuple.elems[2]->value.u16_value;
                datagrams_native[i].remote_address->val.ipv6.address.f3 =
                    address_tuple.elems[3]->value.u16_value;
                datagrams_native[i].remote_address->val.ipv6.address.f4 =
                    address_tuple.elems[4]->value.u16_value;
                datagrams_native[i].remote_address->val.ipv6.address.f5 =
                    address_tuple.elems[5]->value.u16_value;
                datagrams_native[i].remote_address->val.ipv6.address.f6 =
                    address_tuple.elems[6]->value.u16_value;
                datagrams_native[i].remote_address->val.ipv6.address.f7 =
                    address_tuple.elems[7]->value.u16_value;
                datagrams_native[i].remote_address->val.ipv6.scope_id =
                    address_record.fields[3].value->value.u32_value;
            }
            else {
                result = get_result_error_val(
                    WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
                goto end;
            }
        }
    }

    if (!lift_borrow(
            exec_env->cx, stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    wasi_udp_socket_t stream_fd = ((wasi_socket_context_t *)hr->data)->fd;

    /* Build both possible result payloads and lower the complete Ok(u64)
       result before the first packet can leave the process. The second store
       below only replaces validated inline scalar data and cannot allocate. */
    error_payload = wit_enum_ctor(WASI_NETWORK_ERROR_CODE_UNKNOWN);
    if (!error_payload) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to allocate UDP send error result");
        store_result_at_end = false;
        goto end;
    }
    sent_value = wit_u64_ctor(0);
    if (sent_value) {
        result = wit_result_ctor(false, sent_value);
        if (!result) {
            free_wit_value(sent_value);
            sent_value = NULL;
        }
    }
    if (!result) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to allocate UDP send result");
        store_result_at_end = false;
        goto end;
    }
    if (!canonical_resource_transfer_scope_enter(
            &lower_scope, exec_env->cx,
            WASM_COMPONENT_TABLE_TRANSACTION_LOWER)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to start UDP send result transfer");
        store_result_at_end = false;
        goto end;
    }
    lower_scope_active = true;
    if (!store(exec_env->cx, offset_addr, func_type->results->result, result)
        || !canonical_resource_transfer_scope_can_commit(&lower_scope)) {
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
        lower_scope_active = false;
        store_result_at_end = false;
        goto end;
    }

    wasi_sockets_udp_send(stream_fd, datagrams_native, datagrams_len,
                          &sent_count, &err);

    if (err >= 0) {
        free_wit_value(sent_value);
        sent_value = NULL;
        error_payload->value.enum_value.value = errno_to_wasi_network(err);
        result->value.result_value.is_err = true;
        result->value.result_value.result.err = error_payload;
        error_payload = NULL;
    }
    else {
        sent_value->value.u64_value = sent_count;
    }
    if (!store(exec_env->cx, offset_addr, func_type->results->result, result)
        || !canonical_resource_transfer_scope_can_commit(&lower_scope)) {
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
        lower_scope_active = false;
        store_result_at_end = false;
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to publish UDP send result");
        goto end;
    }
    if (!canonical_resource_transfer_scope_leave(&lower_scope, true)) {
        lower_scope_active = false;
        store_result_at_end = false;
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to commit UDP send result");
        goto end;
    }
    lower_scope_active = false;
    store_result_at_end = false;

end:
    if (lower_scope_active) {
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
    }
    if (datagrams_native) {
        for (uint64_t j = 0; j < datagrams_len; j++) {
            wasm_runtime_free(datagrams_native[j].data);
            if (datagrams_native[j].remote_address) {
                wasm_runtime_free((void *)datagrams_native[j].remote_address);
            }
        }
        wasm_runtime_free(datagrams_native);
    }

    if (store_result_at_end) {
        if (result) {
            (void)store(exec_env->cx, offset_addr, func_type->results->result,
                        result);
        }
        else {
            wasm_runtime_set_exception(exec_env->module_inst,
                                       "failed to construct UDP send result");
        }
    }
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_handle);
    free_wit_value(datagrams_val);
}

/**
 * @brief Wrapper for the `subscribe` method of the
 * `wasi:sockets/udp.outgoing-datagram-stream` resource.
 * @param exec_env The execution environment.
 * @param stream_handle The handle of the outgoing datagram stream.
 * @return A new pollable handle.
 */
uint32_t
wasi_sockets_udp_outgoing_datagram_stream_subscribe_wrapper(
    wasm_exec_env_t exec_env, uint32_t stream_handle)
{

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common
        || !wasi_ctx->wasi_options->inherit_network
        || !wasi_ctx->wasi_options->udp) {
        return 0;
    }

    wit_value_t lifted_handle = NULL;

    if (!lift_borrow(
            exec_env->cx, stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        return 0;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);
    free_wit_value(lifted_handle);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get resource outgoing stream");
        return 0;
    }

    wasi_udp_socket_t stream_fd = ((wasi_socket_context_t *)hr->data)->fd;

    HostResource *hr_poll =
        host_resource_create(WASI_P2_POLLABLE, sizeof(wasi_pollable_context_t));

    if (!hr_poll) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not create pollable resource");
        return 0;
    }

    if (!init_owned_fd_pollable(exec_env, hr_poll, stream_fd,
                                WASI_POLLABLE_OUT)) {
        destroy_host_resource(hr_poll);
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not duplicate udp outgoing stream fd");
        return 0;
    }
    host_resource_set_dtor(hr_poll, pollable_dtor);

    uint32_t index_rep = host_resource_table_add(hr_table, hr_poll);
    if (index_rep < 1) {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not add pollable resource to HR table");
        destroy_host_resource(hr_poll); // Clean up the HostResource on failure
        return 0;
    }

    if (!lower_owned_host_resource(
            exec_env,
            func_type->results->result[0].type_specific.resource_handle,
            hr_table, index_rep, &index_rep)) {
        return 0;
    }

    return index_rep;
}
