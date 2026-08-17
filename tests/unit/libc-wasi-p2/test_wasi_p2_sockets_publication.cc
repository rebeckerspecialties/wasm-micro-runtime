/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

extern "C" {
#include "component-model/wasm_component_canon.h"
#include "component-model/wasm_component_canonical.h"
#include "component-model/wasm_component_host_resource.h"
#include "component-model/wasm_component_runtime.h"
#include "wasm_allocation_quota.h"
#include "wasm_export.h"
#include "wasi_p2_common.h"
#include "wasi_p2_io.h"
#include "wasi_p2_sockets.h"
#include "wasi_p2_sockets_wrapper.h"
#include "wasi_p2_types.h"
#include "../../../product-mini/platforms/common/libc_wasi.h"
}

namespace {

constexpr uint32_t kUdpSendLowerIndex = 67;
constexpr uint32_t kTcpFinishConnectLowerIndex = 72;
constexpr uint32_t kResultOffset = 0;
constexpr uint32_t kDatagramOffset = 100;
constexpr uint32_t kPayloadOffset = 256;
constexpr uint32_t kQuotaRequestCapacity = 32768;
constexpr uint32_t kNoDeniedRequest = std::numeric_limits<uint32_t>::max();

struct NativeFdQuotaState {
    std::atomic<uint32_t> limit{ UINT32_MAX };
    std::atomic<uint32_t> active{ 0 };
    std::atomic<uint32_t> peak{ 0 };
    std::atomic<uint32_t> denials{ 0 };
};

bool
reserve_native_fds(void *attachment, uint32_t count)
{
    auto *state = static_cast<NativeFdQuotaState *>(attachment);
    uint32_t active = state->active.load();

    while (active <= state->limit.load()
           && count <= state->limit.load() - active) {
        if (state->active.compare_exchange_weak(active, active + count)) {
            uint32_t peak = state->peak.load();
            while (
                peak < active + count
                && !state->peak.compare_exchange_weak(peak, active + count)) {
            }
            return true;
        }
    }
    state->denials++;
    return false;
}

void
release_native_fds(void *attachment, uint32_t count)
{
    auto *state = static_cast<NativeFdQuotaState *>(attachment);
    const uint32_t previous = state->active.fetch_sub(count);
    ASSERT_GE(previous, count);
}

std::vector<uint8_t>
read_socket_component()
{
    const std::string path = std::string(WAMR_UNIT_TEST_SOURCE_DIR)
                             + "/wasm-apps/test_sockets_comp.wasm";
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return {};
    }
    const std::streamsize size = input.tellg();
    if (size <= 0 || static_cast<uint64_t>(size) > UINT32_MAX) {
        return {};
    }
    input.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!input.read(reinterpret_cast<char *>(bytes.data()), size)) {
        return {};
    }
    return bytes;
}

WASMFunctionInstance *
get_lower(WASMComponentInstance *instance, uint32_t canonical_index)
{
    if (!instance || canonical_index >= instance->core_functions_count) {
        return nullptr;
    }
    WASMFunctionInstance *canonical = instance->core_functions[canonical_index];
    if (!canonical || !canonical->component_function
        || !canonical->canon_options) {
        return nullptr;
    }
    for (uint32_t i = 0; i < instance->core_module_instances_count; i++) {
        WASMModuleInstance *module = instance->core_module_instances[i];
        if (!module || !module->module || !module->e) {
            continue;
        }
        for (uint32_t j = 0; j < module->module->import_function_count; j++) {
            WASMFunctionInstance *lower = &module->e->functions[j];
            if (lower->component_function == canonical->component_function
                && lower->canon_options == canonical->canon_options
                && lower->module_instance && lower->u.func_import
                && lower->u.func_import->func_ptr_linked) {
                return lower;
            }
        }
    }
    return nullptr;
}

WASMFunctionInstance *
get_lower_by_native(WASMComponentInstance *instance, const void *native)
{
    if (!instance || !native)
        return nullptr;
    for (uint32_t i = 0; i < instance->core_module_instances_count; i++) {
        WASMModuleInstance *module = instance->core_module_instances[i];
        if (!module || !module->module || !module->e)
            continue;
        for (uint32_t j = 0; j < module->module->import_function_count; j++) {
            WASMFunctionInstance *lower = &module->e->functions[j];
            if (lower->u.func_import
                && lower->u.func_import->func_ptr_linked == native)
                return lower;
        }
    }
    return nullptr;
}

WASMComponentInstance *
lower_owner(WASMFunctionInstance *lower)
{
    return lower && lower->module_instance
               ? lower->module_instance->comp_instance
               : nullptr;
}

WASMMemoryInstance *
lower_memory(WASMFunctionInstance *lower)
{
    if (!lower || !lower->canon_options
        || !lower->canon_options->lift_lower_opts
        || !lower->canon_options->lift_lower_opts->lift_opts) {
        return nullptr;
    }
    return lower->canon_options->lift_lower_opts->lift_opts->memory;
}

bool
invoke_lower(WASMFunctionInstance *lower, uint32_t *argv, uint32_t argc)
{
    if (!lower || !lower->module_instance || argc != lower->param_cell_num) {
        return false;
    }
    auto *exec_env =
        reinterpret_cast<WASMExecEnv *>(wasm_runtime_get_exec_env_singleton(
            reinterpret_cast<WASMModuleInstanceCommon *>(
                lower->module_instance)));
    if (!exec_env) {
        return false;
    }
    const bool saved_callback_active = exec_env->component_callback_active;
    uint32_t argv_ret[2] = {};
    exec_env->component_callback_active = true;
    const bool result =
        wasm_runtime_invoke_native_p2(exec_env, lower, argv, argc, argv_ret);
    exec_env->component_callback_active = saved_callback_active;
    return result;
}

bool
create_guest_resource_handle(WASMFunctionInstance *lower, uint32_t rep,
                             uint32_t *handle)
{
    if (!lower || !lower->component_function
        || !lower->component_function->func_type
        || !lower->component_function->func_type->params
        || lower->component_function->func_type->params->count == 0
        || !handle) {
        return false;
    }
    WASMComponentTypeInstance *param_type =
        lower->component_function->func_type->params->params[0].type;
    return param_type && param_type->type_specific.resource_handle
           && canon_resource_new(
               param_type->type_specific.resource_handle->resource,
               lower_owner(lower), rep, handle);
}

uint32_t
add_udp_stream_resource(int fd)
{
    HostResource *resource = host_resource_create(
        WASI_P2_UDP_OUTGOING_DATAGRAM_STREAM, sizeof(uint32_t));
    if (!resource) {
        return 0;
    }
    *static_cast<uint32_t *>(resource->data) = static_cast<uint32_t>(fd);
    host_resource_set_dtor(resource, udp_datagram_stream_dtor);
    const uint32_t rep =
        host_resource_table_add(get_global_host_resource_table(), resource);
    if (rep == 0) {
        destroy_host_resource(resource);
    }
    return rep;
}

uint32_t
add_udp_incoming_stream_resource(int fd)
{
    HostResource *resource = host_resource_create(
        WASI_P2_UDP_INCOMING_DATAGRAM_STREAM, sizeof(uint32_t));
    if (!resource) {
        return 0;
    }
    *static_cast<uint32_t *>(resource->data) = static_cast<uint32_t>(fd);
    host_resource_set_dtor(resource, udp_datagram_stream_dtor);
    const uint32_t rep =
        host_resource_table_add(get_global_host_resource_table(), resource);
    if (rep == 0) {
        destroy_host_resource(resource);
    }
    return rep;
}

uint32_t
add_tcp_socket_resource(int fd)
{
    HostResource *resource =
        host_resource_create(WASI_P2_TCP_SOCKET, sizeof(wasi_socket_context_t));
    if (!resource) {
        return 0;
    }
    auto *context = static_cast<wasi_socket_context_t *>(resource->data);
    context->fd = static_cast<uint32_t>(fd);
    context->family = WASI_IP_ADDRESS_FAMILY_IPV4;
    context->tcp_listen_backlog = 0;
    context->tcp_is_listening = false;
    host_resource_set_dtor(resource, tcp_socket_dtor);
    const uint32_t rep =
        host_resource_table_add(get_global_host_resource_table(), resource);
    if (rep == 0) {
        destroy_host_resource(resource);
    }
    return rep;
}

bool
ensure_guest_memory(WASMFunctionInstance *lower, uint64_t required_size)
{
    WASMMemoryInstance *memory = lower_memory(lower);
    if (!memory) {
        return false;
    }
    while (memory->memory_data_size < required_size) {
        if (!wasm_runtime_enlarge_memory(
                reinterpret_cast<WASMModuleInstanceCommon *>(
                    lower->module_instance),
                1)) {
            return false;
        }
    }
    return true;
}

void
write_udp_datagram(WASMFunctionInstance *lower,
                   const wasi_ip_socket_address_t &remote, uint32_t length,
                   uint8_t fill)
{
    uint8_t *memory = lower_memory(lower)->memory_data;
    memset(memory + kDatagramOffset, 0, 24);
    memcpy(memory + kDatagramOffset, &kPayloadOffset, sizeof(kPayloadOffset));
    memcpy(memory + kDatagramOffset + 4, &length, sizeof(length));
    const uint32_t some = 1;
    memcpy(memory + kDatagramOffset + 8, &some, sizeof(some));
    const uint32_t ipv4 = 0;
    memcpy(memory + kDatagramOffset + 12, &ipv4, sizeof(ipv4));
    memcpy(memory + kDatagramOffset + 16, &remote.val.ipv4.port,
           sizeof(remote.val.ipv4.port));
    memory[kDatagramOffset + 18] = remote.val.ipv4.address.f0;
    memory[kDatagramOffset + 19] = remote.val.ipv4.address.f1;
    memory[kDatagramOffset + 20] = remote.val.ipv4.address.f2;
    memory[kDatagramOffset + 21] = remote.val.ipv4.address.f3;
    memset(memory + kPayloadOffset, fill, length);
}

bool
load_lower_result(WASMFunctionInstance *lower, wit_value_t *result)
{
    if (!lower || !result || !lower->component_function
        || !lower->component_function->func_type
        || !lower->component_function->func_type->results) {
        return false;
    }
    LiftLowerContext context = {};
    context.canonical_opts = lower->canon_options;
    context.inst = lower_owner(lower);
    context.borrow_scope_type = BORROW_SCOPE_NONE;
    return load(&context, kResultOffset,
                lower->component_function->func_type->results->result, result);
}

class SocketPublicationWrapperTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        RuntimeInitArgs init_args = {};
        init_args.mem_alloc_type = Alloc_With_System_Allocator;
        ASSERT_TRUE(wasm_runtime_full_init(&init_args));
        runtime_initialized_ = true;
        ASSERT_TRUE(instantiate_host_resource_table());
        host_table_initialized_ = true;

        component_bytes_ = read_socket_component();
        ASSERT_FALSE(component_bytes_.empty());
        LoadArgs2 args;
        wasm_runtime_load_args2_init(&args);
        args.name = const_cast<char *>("socket-publication-test");
        args.no_resolve = 1;
        args.is_component = 1;
        component_ = wasm_component_load_ex2(
            component_bytes_.data(),
            static_cast<uint32_t>(component_bytes_.size()), &args, error_buf_,
            sizeof(error_buf_));
        ASSERT_NE(component_, nullptr) << error_buf_;

        libc_wasi_set_default_options(&parse_context_);
        libc_component_wasi_init(component_, 0, nullptr, &parse_context_);
        const char *test_name =
            testing::UnitTest::GetInstance()->current_test_info()->name();
        const bool exercises_native_fd_creation =
            strstr(test_name, "NativeFdQuota") != nullptr;
        if (exercises_native_fd_creation) {
            struct InstantiationArgs2 *instantiation_args = nullptr;
            ASSERT_TRUE(
                wasm_runtime_instantiation_args_create(&instantiation_args));
            wasm_runtime_instantiation_args_set_default_stack_size(
                instantiation_args, COMPONENT_DEFAULT_STACK_SIZE);
            wasm_runtime_instantiation_args_set_host_managed_heap_size(
                instantiation_args, COMPONENT_DEFAULT_HEAP_SIZE);
            wasm_runtime_instantiation_args_set_native_fd_quota(
                instantiation_args, &fd_quota_, reserve_native_fds,
                release_native_fds);
            instance_ = wasm_component_instantiate_ex2(
                component_, instantiation_args, error_buf_, sizeof(error_buf_));
            wasm_runtime_instantiation_args_destroy(instantiation_args);
        }
        else {
            instance_ = wasm_component_instantiate_internal(
                component_, nullptr, error_buf_, sizeof(error_buf_));
        }
        ASSERT_NE(instance_, nullptr) << error_buf_;
    }

    void TearDown() override
    {
        if (instance_) {
            wasm_component_deinstantiate(instance_);
            instance_ = nullptr;
        }
        EXPECT_EQ(fd_quota_.active.load(), 0u);
        wasi_p2_sockets_cleanup();
        destroy_resolve_streams_map();
        if (host_table_initialized_) {
            destroy_host_resource_table();
            host_table_initialized_ = false;
        }
        if (component_) {
            wasm_component_unload(component_);
            component_ = nullptr;
        }
        if (runtime_initialized_) {
            wasm_runtime_destroy();
            runtime_initialized_ = false;
        }
    }

    std::vector<uint8_t> component_bytes_;
    WASMComponent *component_ = nullptr;
    WASMComponentInstance *instance_ = nullptr;
    libc_wasi_parse_context_t parse_context_ = {};
    char error_buf_[256] = {};
    bool runtime_initialized_ = false;
    bool host_table_initialized_ = false;
    NativeFdQuotaState fd_quota_;
};

TEST_F(SocketPublicationWrapperTest,
       NativeFdQuotaDeniesNPlusOneThenDropAllowsReopen)
{
    WASMFunctionInstance *lower = get_lower_by_native(
        instance_,
        reinterpret_cast<const void *>(
            wasi_sockets_tcp_create_socket_create_tcp_socket_wrapper));
    ASSERT_NE(lower, nullptr);
    fd_quota_.limit = 1;

    auto create_socket = [&]() -> wit_value_t {
        uint32_t argv[] = { WASI_IP_ADDRESS_FAMILY_IPV4, kResultOffset };
        EXPECT_TRUE(invoke_lower(lower, argv, 2));
        wit_value_t result = nullptr;
        EXPECT_TRUE(load_lower_result(lower, &result));
        return result;
    };

    wit_value_t first = create_socket();
    ASSERT_NE(first, nullptr);
    ASSERT_FALSE(first->value.result_value.is_err);
    const uint32_t first_rep =
        first->value.result_value.result.ok->value.resource_value.value;
    ASSERT_NE(first_rep, 0u);
    EXPECT_EQ(fd_quota_.active.load(), 1u);
    EXPECT_EQ(fd_quota_.peak.load(), 1u);

    WASMFunctionInstance *subscribe_lower = get_lower_by_native(
        instance_, reinterpret_cast<const void *>(
                       wasi_sockets_tcp_tcp_socket_subscribe_wrapper));
    ASSERT_NE(subscribe_lower, nullptr);
    uint32_t subscribe_handle = 0;
    ASSERT_TRUE(create_guest_resource_handle(subscribe_lower, first_rep,
                                             &subscribe_handle));
    uint32_t subscribe_argv[] = { subscribe_handle };
    EXPECT_FALSE(invoke_lower(subscribe_lower, subscribe_argv, 1));
    EXPECT_EQ(fd_quota_.denials.load(), 1u);
    EXPECT_EQ(fd_quota_.active.load(), 1u);

    wit_value_t denied = create_socket();
    ASSERT_NE(denied, nullptr);
    ASSERT_TRUE(denied->value.result_value.is_err);
    EXPECT_EQ(denied->value.result_value.result.err->value.enum_value.value,
              WASI_NETWORK_ERROR_CODE_NEW_SOCKET_LIMIT);
    EXPECT_EQ(fd_quota_.denials.load(), 2u);
    EXPECT_EQ(fd_quota_.active.load(), 1u);

    WASMComponentResourceHandleInstance *subscribe_resource_handle =
        subscribe_lower->component_function->func_type->params->params[0]
            .type->type_specific.resource_handle;
    ASSERT_NE(subscribe_resource_handle, nullptr);
    ASSERT_TRUE(canon_resource_drop(subscribe_resource_handle->resource,
                                    lower_owner(subscribe_lower),
                                    subscribe_handle));
    EXPECT_EQ(fd_quota_.active.load(), 0u);

    wit_value_t reopened = create_socket();
    ASSERT_NE(reopened, nullptr);
    ASSERT_FALSE(reopened->value.result_value.is_err);
    const uint32_t reopened_rep =
        reopened->value.result_value.result.ok->value.resource_value.value;
    ASSERT_NE(reopened_rep, 0u);
    EXPECT_EQ(fd_quota_.active.load(), 1u);
    ASSERT_EQ(host_resource_table_delete(get_global_host_resource_table(),
                                         reopened_rep),
              1u);
    EXPECT_EQ(fd_quota_.active.load(), 0u);

    free_wit_value(reopened);
    free_wit_value(denied);
    free_wit_value(first);
}

TEST_F(SocketPublicationWrapperTest,
       UdpSendMapsNativeMessageTooLargeWithoutPeerTraffic)
{
    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(receiver, 0);
    sockaddr_in receiver_address = {};
    receiver_address.sin_family = AF_INET;
    receiver_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    receiver_address.sin_port = 0;
    ASSERT_EQ(bind(receiver, reinterpret_cast<sockaddr *>(&receiver_address),
                   sizeof(receiver_address)),
              0);
    socklen_t receiver_address_length = sizeof(receiver_address);
    ASSERT_EQ(getsockname(receiver,
                          reinterpret_cast<sockaddr *>(&receiver_address),
                          &receiver_address_length),
              0);

    wasi_ip_socket_address_t remote = {};
    remote.tag = WASI_IP_SOCKET_ADDRESS_TAG_IPV4;
    remote.val.ipv4.port = ntohs(receiver_address.sin_port);
    remote.val.ipv4.address = { 127, 0, 0, 1 };

    WASMFunctionInstance *lower = get_lower(instance_, kUdpSendLowerIndex);
    ASSERT_NE(lower, nullptr);
    constexpr uint32_t oversized_length = UINT16_MAX;
    ASSERT_TRUE(ensure_guest_memory(lower, kPayloadOffset + oversized_length));

    wasi_udp_socket_t sender = -1;
    int error = 0;
    wasi_sockets_create_udp_socket(WASI_IP_ADDRESS_FAMILY_IPV4, &sender,
                                   &error);
    ASSERT_EQ(error, 0);
    const uint32_t rep = add_udp_stream_resource(sender);
    ASSERT_NE(rep, 0u);
    uint32_t handle = 0;
    ASSERT_TRUE(create_guest_resource_handle(lower, rep, &handle));
    write_udp_datagram(lower, remote, oversized_length, 0x5a);

    uint32_t argv[] = { handle, kDatagramOffset, 1, kResultOffset };
    ASSERT_TRUE(invoke_lower(lower, argv, 4));

    wit_value_t result = nullptr;
    ASSERT_TRUE(load_lower_result(lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_TRUE(result->value.result_value.is_err);
    ASSERT_NE(result->value.result_value.result.err, nullptr);
    EXPECT_EQ(result->value.result_value.result.err->value.enum_value.value,
              WASI_NETWORK_ERROR_CODE_DATAGRAM_TOO_LARGE);
    free_wit_value(result);

    pollfd ready = { .fd = receiver, .events = POLLIN, .revents = 0 };
    EXPECT_EQ(poll(&ready, 1, 100), 0);
    close(receiver);
}

TEST_F(SocketPublicationWrapperTest,
       TcpFinishConnectPublishesIndependentInitializedStreams)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);
    sockaddr_in listener_address = {};
    listener_address.sin_family = AF_INET;
    listener_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listener_address.sin_port = 0;
    ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr *>(&listener_address),
                   sizeof(listener_address)),
              0);
    ASSERT_EQ(listen(listener, 1), 0);
    socklen_t listener_address_length = sizeof(listener_address);
    ASSERT_EQ(getsockname(listener,
                          reinterpret_cast<sockaddr *>(&listener_address),
                          &listener_address_length),
              0);

    wasi_ip_socket_address_t remote = {};
    remote.tag = WASI_IP_SOCKET_ADDRESS_TAG_IPV4;
    remote.val.ipv4.port = ntohs(listener_address.sin_port);
    remote.val.ipv4.address = { 127, 0, 0, 1 };

    wasi_tcp_socket_t client = -1;
    int error = 0;
    wasi_sockets_create_tcp_socket(WASI_IP_ADDRESS_FAMILY_IPV4, &client,
                                   &error);
    ASSERT_EQ(error, 0);
    error = wasi_sockets_tcp_start_connect(
        client, wasi_sockets_instance_network(), &remote);
    ASSERT_TRUE(error == 0 || error == EINPROGRESS);
    pollfd ready = { .fd = client, .events = POLLOUT, .revents = 0 };
    ASSERT_EQ(poll(&ready, 1, 1000), 1);

    WASMFunctionInstance *lower =
        get_lower(instance_, kTcpFinishConnectLowerIndex);
    ASSERT_NE(lower, nullptr);
    const uint32_t socket_rep = add_tcp_socket_resource(client);
    ASSERT_NE(socket_rep, 0u);
    uint32_t socket_handle = 0;
    ASSERT_TRUE(
        create_guest_resource_handle(lower, socket_rep, &socket_handle));

    uint32_t argv[] = { socket_handle, kResultOffset };
    ASSERT_TRUE(invoke_lower(lower, argv, 2));

    wit_value_t result = nullptr;
    ASSERT_TRUE(load_lower_result(lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    wit_value_t tuple = result->value.result_value.result.ok;
    ASSERT_NE(tuple, nullptr);
    ASSERT_EQ(tuple->value.tuple_value.size, 2u);
    const uint32_t input_rep =
        tuple->value.tuple_value.elems[0]->value.resource_value.value;
    const uint32_t output_rep =
        tuple->value.tuple_value.elems[1]->value.resource_value.value;
    ASSERT_NE(input_rep, 0u);
    ASSERT_NE(output_rep, 0u);
    ASSERT_NE(input_rep, output_rep);

    HostResourceTable *table = get_global_host_resource_table();
    HostResource *input_resource = host_resource_table_get(table, input_rep);
    HostResource *output_resource = host_resource_table_get(table, output_rep);
    ASSERT_NE(input_resource, nullptr);
    ASSERT_NE(output_resource, nullptr);
    EXPECT_EQ(input_resource->type, WASI_P2_IO_INPUT_STREAM);
    EXPECT_EQ(output_resource->type, WASI_P2_IO_OUTPUT_STREAM);
    auto *input = static_cast<StreamResourceType *>(input_resource->data);
    auto *output = static_cast<StreamResourceType *>(output_resource->data);
    EXPECT_EQ(input->type, STREAM_TYPE_SOCKET);
    EXPECT_EQ(output->type, STREAM_TYPE_SOCKET);
    EXPECT_EQ(input->position, 0u);
    EXPECT_EQ(output->position, 0u);
    EXPECT_FALSE(input->position_valid);
    EXPECT_FALSE(output->position_valid);
    EXPECT_FALSE(input->append);
    EXPECT_FALSE(output->append);
    ASSERT_NE(input->fd, output->fd);
    const int output_fd = static_cast<int>(output->fd);
    free_wit_value(result);

    int accepted = accept(listener, nullptr, nullptr);
    ASSERT_GE(accepted, 0);
    ASSERT_EQ(host_resource_table_delete(table, input_rep), 1u);
    const uint8_t payload[] = { 'o', 'k' };
    ASSERT_EQ(write(output_fd, payload, sizeof(payload)),
              static_cast<ssize_t>(sizeof(payload)));
    uint8_t received[sizeof(payload)] = {};
    ASSERT_EQ(read(accepted, received, sizeof(received)),
              static_cast<ssize_t>(sizeof(received)));
    EXPECT_EQ(memcmp(payload, received, sizeof(payload)), 0);
    EXPECT_EQ(host_resource_table_delete(table, output_rep), 1u);

    close(accepted);
    close(listener);
}

TEST_F(SocketPublicationWrapperTest,
       TcpFinishConnectPublishesErrorAndClosesStagedStreams)
{
    int blocker = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(blocker, 0);
    sockaddr_in blocked_address = {};
    blocked_address.sin_family = AF_INET;
    blocked_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    blocked_address.sin_port = 0;
    ASSERT_EQ(bind(blocker, reinterpret_cast<sockaddr *>(&blocked_address),
                   sizeof(blocked_address)),
              0);
    socklen_t blocked_address_length = sizeof(blocked_address);
    ASSERT_EQ(getsockname(blocker,
                          reinterpret_cast<sockaddr *>(&blocked_address),
                          &blocked_address_length),
              0);
    close(blocker);

    wasi_ip_socket_address_t remote = {};
    remote.tag = WASI_IP_SOCKET_ADDRESS_TAG_IPV4;
    remote.val.ipv4.port = ntohs(blocked_address.sin_port);
    remote.val.ipv4.address = { 127, 0, 0, 1 };

    wasi_tcp_socket_t client = -1;
    int error = 0;
    wasi_sockets_create_tcp_socket(WASI_IP_ADDRESS_FAMILY_IPV4, &client,
                                   &error);
    ASSERT_EQ(error, 0);
    error = wasi_sockets_tcp_start_connect(
        client, wasi_sockets_instance_network(), &remote);
    ASSERT_TRUE(error == EINPROGRESS || error == ECONNREFUSED);
    pollfd ready = { .fd = client, .events = POLLIN | POLLOUT, .revents = 0 };
    ASSERT_EQ(poll(&ready, 1, 1000), 1);

    WASMFunctionInstance *lower =
        get_lower(instance_, kTcpFinishConnectLowerIndex);
    ASSERT_NE(lower, nullptr);
    const uint32_t socket_rep = add_tcp_socket_resource(client);
    ASSERT_NE(socket_rep, 0u);
    uint32_t socket_handle = 0;
    ASSERT_TRUE(
        create_guest_resource_handle(lower, socket_rep, &socket_handle));

    const int first_free = fcntl(client, F_DUPFD_CLOEXEC, 0);
    ASSERT_GE(first_free, 0);
    close(first_free);
    memset(lower_memory(lower)->memory_data + kResultOffset, 0xa5, 16);
    uint32_t argv[] = { socket_handle, kResultOffset };
    ASSERT_TRUE(invoke_lower(lower, argv, 2));

    wit_value_t result = nullptr;
    ASSERT_TRUE(load_lower_result(lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_TRUE(result->value.result_value.is_err);
    ASSERT_NE(result->value.result_value.result.err, nullptr);
    EXPECT_EQ(result->value.result_value.result.err->value.enum_value.value,
              WASI_NETWORK_ERROR_CODE_CONNECTION_REFUSED);
    free_wit_value(result);

    const int next_free = fcntl(client, F_DUPFD_CLOEXEC, 0);
    ASSERT_GE(next_free, 0);
    EXPECT_EQ(next_free, first_free);
    close(next_free);
}

TEST_F(SocketPublicationWrapperTest,
       TcpAcceptPublishesPreallocatedIndependentResources)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    ASSERT_EQ(
        bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)),
        0);
    ASSERT_EQ(listen(listener, 1), 0);
    socklen_t address_length = sizeof(address);
    ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                          &address_length),
              0);

    int client = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client, 0);
    ASSERT_NE(client, listener);
    ASSERT_EQ(connect(client, reinterpret_cast<sockaddr *>(&address),
                      sizeof(address)),
              0);

    WASMFunctionInstance *lower = get_lower_by_native(
        instance_, reinterpret_cast<const void *>(
                       wasi_sockets_tcp_tcp_socket_accept_wrapper));
    ASSERT_NE(lower, nullptr);
    const uint32_t listener_rep = add_tcp_socket_resource(listener);
    ASSERT_NE(listener_rep, 0u);
    ASSERT_NE(fcntl(listener, F_GETFD), -1);
    uint32_t listener_handle = 0;
    ASSERT_TRUE(
        create_guest_resource_handle(lower, listener_rep, &listener_handle));
    HostResource *listener_resource =
        host_resource_table_get(get_global_host_resource_table(), listener_rep);
    ASSERT_NE(listener_resource, nullptr);
    EXPECT_EQ(static_cast<wasi_socket_context_t *>(listener_resource->data)->fd,
              listener);
    ASSERT_NE(fcntl(listener, F_GETFD), -1);

    uint32_t argv[] = { listener_handle, kResultOffset };
    ASSERT_TRUE(invoke_lower(lower, argv, 2));

    wit_value_t result = nullptr;
    ASSERT_TRUE(load_lower_result(lower, &result));
    ASSERT_NE(result, nullptr);
    if (result->value.result_value.is_err
        && result->value.result_value.result.err) {
        ADD_FAILURE()
            << "accept returned network error "
            << result->value.result_value.result.err->value.enum_value.value;
    }
    ASSERT_FALSE(result->value.result_value.is_err);
    wit_value_t tuple = result->value.result_value.result.ok;
    ASSERT_NE(tuple, nullptr);
    ASSERT_EQ(tuple->value.tuple_value.size, 3u);
    const uint32_t socket_rep =
        tuple->value.tuple_value.elems[0]->value.resource_value.value;
    const uint32_t input_rep =
        tuple->value.tuple_value.elems[1]->value.resource_value.value;
    const uint32_t output_rep =
        tuple->value.tuple_value.elems[2]->value.resource_value.value;
    ASSERT_NE(socket_rep, 0u);
    ASSERT_NE(input_rep, 0u);
    ASSERT_NE(output_rep, 0u);
    EXPECT_NE(socket_rep, input_rep);
    EXPECT_NE(socket_rep, output_rep);
    EXPECT_NE(input_rep, output_rep);

    HostResourceTable *table = get_global_host_resource_table();
    HostResource *input_resource = host_resource_table_get(table, input_rep);
    HostResource *output_resource = host_resource_table_get(table, output_rep);
    ASSERT_NE(input_resource, nullptr);
    ASSERT_NE(output_resource, nullptr);
    const int input_fd = static_cast<int>(
        static_cast<StreamResourceType *>(input_resource->data)->fd);
    const int output_fd = static_cast<int>(
        static_cast<StreamResourceType *>(output_resource->data)->fd);
    EXPECT_NE(input_fd, output_fd);
    free_wit_value(result);

    const uint8_t request[] = { 'i', 'n' };
    ASSERT_EQ(write(client, request, sizeof(request)),
              static_cast<ssize_t>(sizeof(request)));
    pollfd input_ready = { .fd = input_fd, .events = POLLIN, .revents = 0 };
    ASSERT_EQ(poll(&input_ready, 1, 1000), 1);
    ASSERT_EQ(input_ready.revents & (POLLERR | POLLHUP | POLLNVAL), 0);
    uint8_t readback[sizeof(request)] = {};
    ASSERT_EQ(read(input_fd, readback, sizeof(readback)),
              static_cast<ssize_t>(sizeof(readback)));
    EXPECT_EQ(memcmp(request, readback, sizeof(request)), 0);

    const uint8_t response[] = { 'o', 'k' };
    ASSERT_EQ(write(output_fd, response, sizeof(response)),
              static_cast<ssize_t>(sizeof(response)));
    memset(readback, 0, sizeof(readback));
    ASSERT_EQ(read(client, readback, sizeof(readback)),
              static_cast<ssize_t>(sizeof(readback)));
    EXPECT_EQ(memcmp(response, readback, sizeof(response)), 0);

    EXPECT_EQ(host_resource_table_delete(table, input_rep), 1u);
    EXPECT_EQ(host_resource_table_delete(table, output_rep), 1u);
    EXPECT_EQ(host_resource_table_delete(table, socket_rep), 1u);
    close(client);
}

TEST_F(SocketPublicationWrapperTest,
       UdpReceivePublishesBeforeConsumingQueuedDatagram)
{
    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(receiver, 0);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    ASSERT_EQ(
        bind(receiver, reinterpret_cast<sockaddr *>(&address), sizeof(address)),
        0);
    socklen_t address_length = sizeof(address);
    ASSERT_EQ(getsockname(receiver, reinterpret_cast<sockaddr *>(&address),
                          &address_length),
              0);
    int flags = fcntl(receiver, F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(fcntl(receiver, F_SETFL, flags | O_NONBLOCK), 0);

    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(sender, 0);
    const uint8_t payload[] = { 0x31, 0x32, 0x33 };
    ASSERT_EQ(sendto(sender, payload, sizeof(payload), 0,
                     reinterpret_cast<sockaddr *>(&address), sizeof(address)),
              static_cast<ssize_t>(sizeof(payload)));
    pollfd packet_ready = { .fd = receiver, .events = POLLIN, .revents = 0 };
    ASSERT_EQ(poll(&packet_ready, 1, 1000), 1);
    ASSERT_NE(packet_ready.revents & POLLIN, 0);

    WASMFunctionInstance *lower = get_lower_by_native(
        instance_,
        reinterpret_cast<const void *>(
            wasi_sockets_udp_incoming_datagram_stream_receive_wrapper));
    ASSERT_NE(lower, nullptr);
    const uint32_t rep = add_udp_incoming_stream_resource(receiver);
    ASSERT_NE(rep, 0u);
    uint32_t handle = 0;
    ASSERT_TRUE(create_guest_resource_handle(lower, rep, &handle));

    uint32_t argv[] = { handle, 1, 0, kResultOffset };
    ASSERT_TRUE(invoke_lower(lower, argv, 4)) << wasm_runtime_get_exception(
        reinterpret_cast<WASMModuleInstanceCommon *>(lower->module_instance));
    wit_value_t result = nullptr;
    ASSERT_TRUE(load_lower_result(lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    wit_value_t list = result->value.result_value.result.ok;
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->value.list_value.size, 1u);
    free_wit_value(result);

    uint8_t byte = 0;
    EXPECT_EQ(recv(receiver, &byte, sizeof(byte), 0), -1);
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    close(sender);
}

TEST_F(SocketPublicationWrapperTest, UdpReceivePublishesEmptyList)
{
    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(receiver, 0);
    int flags = fcntl(receiver, F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(fcntl(receiver, F_SETFL, flags | O_NONBLOCK), 0);

    WASMFunctionInstance *lower = get_lower_by_native(
        instance_,
        reinterpret_cast<const void *>(
            wasi_sockets_udp_incoming_datagram_stream_receive_wrapper));
    ASSERT_NE(lower, nullptr);
    const uint32_t rep = add_udp_incoming_stream_resource(receiver);
    ASSERT_NE(rep, 0u);
    uint32_t handle = 0;
    ASSERT_TRUE(create_guest_resource_handle(lower, rep, &handle));

    uint32_t argv[] = { handle, 1, 0, kResultOffset };
    ASSERT_TRUE(invoke_lower(lower, argv, 4)) << wasm_runtime_get_exception(
        reinterpret_cast<WASMModuleInstanceCommon *>(lower->module_instance));
    wit_value_t result = nullptr;
    ASSERT_TRUE(load_lower_result(lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    ASSERT_NE(result->value.result_value.result.ok, nullptr);
    EXPECT_EQ(result->value.result_value.result.ok->value.list_value.size, 0u);
    free_wit_value(result);
}

struct QuotaRequest {
    uint64_t bytes = 0;
    uint32_t allocations = 0;
};

struct SocketQuotaState {
    std::array<QuotaRequest, kQuotaRequestCapacity> requests = {};
    std::atomic<uint32_t> reserve_calls = 0;
    std::atomic<uint32_t> denied_calls = 0;
    std::atomic<uint64_t> live_bytes = 0;
    std::atomic<uint32_t> live_allocations = 0;
    std::atomic<uint32_t> retain_calls = 0;
    std::atomic<uint32_t> attachment_release_calls = 0;
    std::atomic<uint32_t> deny_at = kNoDeniedRequest;
    std::atomic<bool> overflow = false;
};

bool
socket_quota_reserve(void *attachment, uint64_t bytes, uint32_t allocations)
{
    auto *state = static_cast<SocketQuotaState *>(attachment);
    const uint32_t index = state->reserve_calls.fetch_add(1);
    if (index < state->requests.size()) {
        state->requests[index] = { bytes, allocations };
    }
    else {
        state->overflow.store(true);
        return false;
    }
    if (index == state->deny_at.load()) {
        state->denied_calls.fetch_add(1);
        return false;
    }
    state->live_bytes.fetch_add(bytes);
    state->live_allocations.fetch_add(allocations);
    return true;
}

void
socket_quota_release(void *attachment, uint64_t bytes, uint32_t allocations)
{
    auto *state = static_cast<SocketQuotaState *>(attachment);
    state->live_bytes.fetch_sub(bytes);
    state->live_allocations.fetch_sub(allocations);
}

bool
socket_quota_retain(void *attachment)
{
    auto *state = static_cast<SocketQuotaState *>(attachment);
    state->retain_calls.fetch_add(1);
    return true;
}

void
socket_quota_attachment_release(void *attachment)
{
    auto *state = static_cast<SocketQuotaState *>(attachment);
    state->attachment_release_calls.fetch_add(1);
}

struct QuotaSendResult {
    std::vector<QuotaRequest> execution_requests;
    bool invoked = false;
    bool result_loaded = false;
    bool result_is_error = false;
    uint32_t result_error = UINT32_MAX;
    uint64_t sent_count = UINT64_MAX;
    uint8_t result_discriminant = 0xff;
    uint32_t denied_calls = 0;
    uint64_t live_bytes = UINT64_MAX;
    uint32_t live_allocations = UINT32_MAX;
    uint32_t retain_calls = 0;
    uint32_t attachment_release_calls = 0;
    bool request_overflow = false;
    uint32_t stage = 0;
};

QuotaSendResult
run_quota_udp_send(const std::vector<uint8_t> &component_bytes,
                   const wasi_ip_socket_address_t &remote,
                   uint32_t denied_execution_request)
{
    QuotaSendResult result;
    SocketQuotaState quota;
    std::vector<uint8_t> component_copy = component_bytes;
    RuntimeInitArgs init_args = {};
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init_args)) {
        return result;
    }
    result.stage = 1;

    bool host_table_initialized = instantiate_host_resource_table();
    if (host_table_initialized) {
        result.stage = 2;
    }
    WASMComponent *component = nullptr;
    WASMComponentInstance *instance = nullptr;
    libc_wasi_parse_context_t parse_context = {};
    char error_buf[256] = {};
    WASMAllocationQuotaScope owner_scope = {};
    bool owner_scope_active = false;

    LoadArgs2 args;
    wasm_runtime_load_args2_init(&args);
    args.name = const_cast<char *>("socket-quota-publication-test");
    args.no_resolve = 1;
    args.is_component = 1;
    wasm_runtime_load_args2_set_allocation_quota(
        &args, &quota, socket_quota_reserve, socket_quota_release,
        socket_quota_retain, socket_quota_attachment_release);

    if (host_table_initialized) {
        component = wasm_component_load_ex2(
            component_copy.data(), static_cast<uint32_t>(component_copy.size()),
            &args, error_buf, sizeof(error_buf));
    }
    if (component) {
        result.stage = 3;
        libc_wasi_set_default_options(&parse_context);
        libc_component_wasi_init(component, 0, nullptr, &parse_context);
        instance = wasm_component_instantiate_internal(
            component, nullptr, error_buf, sizeof(error_buf));
    }

    WASMFunctionInstance *lower = get_lower(instance, kUdpSendLowerIndex);
    if (lower
        && wasm_allocation_quota_scope_enter_for_allocation(&owner_scope,
                                                            component)) {
        result.stage = 4;
        owner_scope_active = true;
        wasi_udp_socket_t sender = -1;
        int error = 0;
        wasi_sockets_create_udp_socket(WASI_IP_ADDRESS_FAMILY_IPV4, &sender,
                                       &error);
        const uint32_t rep = error == 0 ? add_udp_stream_resource(sender) : 0;
        uint32_t handle = 0;
        if (rep != 0 && create_guest_resource_handle(lower, rep, &handle)
            && ensure_guest_memory(lower, kPayloadOffset + 1)) {
            result.stage = 5;
            uint32_t argv[] = { handle, kDatagramOffset, 1, kResultOffset };
            int warmup_receiver = socket(AF_INET, SOCK_DGRAM, 0);
            sockaddr_in warmup_native = {};
            warmup_native.sin_family = AF_INET;
            warmup_native.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            warmup_native.sin_port = 0;
            socklen_t warmup_length = sizeof(warmup_native);
            bool warmup_ready =
                warmup_receiver >= 0
                && bind(warmup_receiver,
                        reinterpret_cast<sockaddr *>(&warmup_native),
                        sizeof(warmup_native))
                       == 0
                && getsockname(warmup_receiver,
                               reinterpret_cast<sockaddr *>(&warmup_native),
                               &warmup_length)
                       == 0;
            if (warmup_ready) {
                wasi_ip_socket_address_t warmup_remote = {};
                warmup_remote.tag = WASI_IP_SOCKET_ADDRESS_TAG_IPV4;
                warmup_remote.val.ipv4.port = ntohs(warmup_native.sin_port);
                warmup_remote.val.ipv4.address = { 127, 0, 0, 1 };
                write_udp_datagram(lower, warmup_remote, 1, 0x71);
                warmup_ready = invoke_lower(lower, argv, 4);
                if (warmup_ready) {
                    result.stage = 6;
                }
                wit_value_t warmup_result = nullptr;
                warmup_ready = warmup_ready
                               && load_lower_result(lower, &warmup_result)
                               && warmup_result
                               && !warmup_result->value.result_value.is_err;
                if (warmup_ready) {
                    result.stage = 7;
                }
                free_wit_value(warmup_result);
                pollfd warmup_poll = { .fd = warmup_receiver,
                                       .events = POLLIN,
                                       .revents = 0 };
                uint8_t warmup_byte = 0;
                warmup_ready = warmup_ready && poll(&warmup_poll, 1, 1000) == 1
                               && recv(warmup_receiver, &warmup_byte,
                                       sizeof(warmup_byte), 0)
                                      == 1;
                if (warmup_ready) {
                    result.stage = 8;
                }
            }
            if (warmup_receiver >= 0) {
                close(warmup_receiver);
            }

            if (warmup_ready) {
                result.stage = 9;
                write_udp_datagram(lower, remote, 1, 0x71);
                memset(lower_memory(lower)->memory_data + kResultOffset, 0xa5,
                       16);
                const uint32_t execution_start = quota.reserve_calls.load();
                if (denied_execution_request != kNoDeniedRequest) {
                    quota.deny_at.store(execution_start
                                        + denied_execution_request);
                }
                result.invoked = invoke_lower(lower, argv, 4);
                const uint32_t execution_end = quota.reserve_calls.load();
                quota.deny_at.store(kNoDeniedRequest);
                if (execution_end >= execution_start
                    && execution_end - execution_start
                           <= quota.requests.size()) {
                    result.execution_requests.assign(
                        quota.requests.begin() + execution_start,
                        quota.requests.begin() + execution_end);
                }
                result.result_discriminant =
                    lower_memory(lower)->memory_data[kResultOffset];
                if (!result.invoked) {
                    wasm_runtime_clear_exception(
                        reinterpret_cast<WASMModuleInstanceCommon *>(
                            lower->module_instance));
                }
                else {
                    wit_value_t loaded = nullptr;
                    result.result_loaded = load_lower_result(lower, &loaded);
                    if (result.result_loaded && loaded) {
                        result.result_is_error =
                            loaded->value.result_value.is_err;
                        if (result.result_is_error
                            && loaded->value.result_value.result.err) {
                            result.result_error =
                                loaded->value.result_value.result.err->value
                                    .enum_value.value;
                        }
                        else if (loaded->value.result_value.result.ok) {
                            result.sent_count = loaded->value.result_value
                                                    .result.ok->value.u64_value;
                        }
                        free_wit_value(loaded);
                    }
                }
            }
        }
    }

    quota.deny_at.store(kNoDeniedRequest);
    if (owner_scope_active) {
        wasm_allocation_quota_scope_leave(&owner_scope);
    }
    if (instance) {
        wasm_component_deinstantiate(instance);
    }
    wasi_p2_sockets_cleanup();
    destroy_resolve_streams_map();
    if (host_table_initialized) {
        destroy_host_resource_table();
    }
    if (component) {
        wasm_component_unload(component);
    }
    wasm_runtime_destroy();

    result.denied_calls = quota.denied_calls.load();
    result.live_bytes = quota.live_bytes.load();
    result.live_allocations = quota.live_allocations.load();
    result.retain_calls = quota.retain_calls.load();
    result.attachment_release_calls = quota.attachment_release_calls.load();
    result.request_overflow = quota.overflow.load();
    return result;
}

bool
receiver_has_packet(int receiver, int timeout_ms)
{
    pollfd ready = { .fd = receiver, .events = POLLIN, .revents = 0 };
    return poll(&ready, 1, timeout_ms) == 1 && (ready.revents & POLLIN) != 0;
}

void
consume_packet(int receiver)
{
    uint8_t byte = 0;
    ASSERT_EQ(recv(receiver, &byte, sizeof(byte), 0), 1);
    EXPECT_EQ(byte, 0x71);
}

TEST(SocketPublicationQuotaTest,
     UdpSendAllocationDenialsCannotPublishOkOrReachPeer)
{
    const std::vector<uint8_t> component_bytes = read_socket_component();
    ASSERT_FALSE(component_bytes.empty());

    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(receiver, 0);
    sockaddr_in receiver_address = {};
    receiver_address.sin_family = AF_INET;
    receiver_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    receiver_address.sin_port = 0;
    ASSERT_EQ(bind(receiver, reinterpret_cast<sockaddr *>(&receiver_address),
                   sizeof(receiver_address)),
              0);
    socklen_t receiver_address_length = sizeof(receiver_address);
    ASSERT_EQ(getsockname(receiver,
                          reinterpret_cast<sockaddr *>(&receiver_address),
                          &receiver_address_length),
              0);
    wasi_ip_socket_address_t remote = {};
    remote.tag = WASI_IP_SOCKET_ADDRESS_TAG_IPV4;
    remote.val.ipv4.port = ntohs(receiver_address.sin_port);
    remote.val.ipv4.address = { 127, 0, 0, 1 };

    const QuotaSendResult baseline =
        run_quota_udp_send(component_bytes, remote, kNoDeniedRequest);
    ASSERT_TRUE(baseline.invoked);
    ASSERT_TRUE(baseline.result_loaded);
    ASSERT_FALSE(baseline.result_is_error);
    EXPECT_EQ(baseline.sent_count, 1u);
    ASSERT_TRUE(receiver_has_packet(receiver, 1000));
    consume_packet(receiver);
    ASSERT_FALSE(baseline.request_overflow);
    ASSERT_EQ(baseline.live_bytes, 0u);
    ASSERT_EQ(baseline.live_allocations, 0u);
    ASSERT_EQ(baseline.retain_calls, baseline.attachment_release_calls);

    ASSERT_GE(baseline.execution_requests.size(), 6u);
    const uint32_t native_start =
        static_cast<uint32_t>(baseline.execution_requests.size() - 3);
    uint32_t raw_wit_value = 0;
    ASSERT_TRUE(wasm_allocation_header_calculate_size(
        sizeof(ComponentWITValue), WASM_ALLOCATION_MAX_ALIGNMENT,
        &raw_wit_value));
    uint32_t raw_iovec = 0;
    uint32_t raw_address = 0;
    ASSERT_TRUE(wasm_allocation_header_calculate_size(
        sizeof(iovec), WASM_ALLOCATION_MAX_ALIGNMENT, &raw_iovec));
    ASSERT_TRUE(wasm_allocation_header_calculate_size(
        sizeof(sockaddr_storage), WASM_ALLOCATION_MAX_ALIGNMENT, &raw_address));
    EXPECT_EQ(baseline.execution_requests[native_start + 1].bytes, raw_iovec);
    EXPECT_EQ(baseline.execution_requests[native_start + 2].bytes, raw_address);

    uint32_t output_start = kNoDeniedRequest;
    for (uint32_t i = 0; i + 2 < native_start; i++) {
        if (baseline.execution_requests[i].bytes == raw_wit_value
            && baseline.execution_requests[i + 1].bytes == raw_wit_value
            && baseline.execution_requests[i + 2].bytes == raw_wit_value) {
            output_start = i;
        }
    }
    ASSERT_NE(output_start, kNoDeniedRequest);

    std::vector<uint32_t> denied_requests = {
        output_start, output_start + 1, output_start + 2, native_start - 1,
        native_start, native_start + 1, native_start + 2,
    };
    std::sort(denied_requests.begin(), denied_requests.end());
    denied_requests.erase(
        std::unique(denied_requests.begin(), denied_requests.end()),
        denied_requests.end());

    for (uint32_t denied_request : denied_requests) {
        SCOPED_TRACE(denied_request);
        const QuotaSendResult denied =
            run_quota_udp_send(component_bytes, remote, denied_request);
        ASSERT_FALSE(denied.request_overflow);
        ASSERT_GE(denied.denied_calls, 1u)
            << "stage=" << denied.stage
            << " requests=" << denied.execution_requests.size();
        EXPECT_FALSE(receiver_has_packet(receiver, 100));
        EXPECT_EQ(denied.live_bytes, 0u);
        EXPECT_EQ(denied.live_allocations, 0u);
        EXPECT_EQ(denied.retain_calls, denied.attachment_release_calls);
        if (denied.invoked) {
            ASSERT_TRUE(denied.result_loaded);
            EXPECT_TRUE(denied.result_is_error);
            EXPECT_NE(denied.result_discriminant, 0u);
        }
        else {
            EXPECT_NE(denied.result_discriminant, 0u);
        }
        if (denied_request >= native_start) {
            ASSERT_TRUE(denied.invoked);
            EXPECT_EQ(denied.result_error,
                      WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY);
        }
    }

    close(receiver);
}

struct QuotaReceiveResult {
    std::vector<QuotaRequest> execution_requests;
    bool invoked = false;
    bool packet_after_first = false;
    bool retry_invoked = false;
    bool retry_success = false;
    bool packet_after_retry = false;
    uint32_t denied_calls = 0;
    uint64_t live_bytes = UINT64_MAX;
    uint32_t live_allocations = UINT32_MAX;
    uint32_t retain_calls = 0;
    uint32_t attachment_release_calls = 0;
    bool request_overflow = false;
};

struct QuotaSetterResult {
    std::vector<QuotaRequest> execution_requests;
    bool invoked = false;
    bool state_unchanged = false;
    bool retry_invoked = false;
    bool retry_changed_state = false;
    uint32_t denied_calls = 0;
    uint64_t live_bytes = UINT64_MAX;
    uint32_t live_allocations = UINT32_MAX;
    uint32_t retain_calls = 0;
    uint32_t attachment_release_calls = 0;
    bool request_overflow = false;
};

QuotaSetterResult
run_quota_tcp_send_buffer_setter(const std::vector<uint8_t> &component_bytes,
                                 uint32_t denied_execution_request)
{
    QuotaSetterResult result;
    SocketQuotaState quota;
    std::vector<uint8_t> component_copy = component_bytes;
    RuntimeInitArgs init_args = {};
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init_args))
        return result;

    const bool host_table_initialized = instantiate_host_resource_table();
    WASMComponent *component = nullptr;
    WASMComponentInstance *instance = nullptr;
    libc_wasi_parse_context_t parse_context = {};
    char error_buf[256] = {};
    WASMAllocationQuotaScope owner_scope = {};
    bool owner_scope_active = false;
    int socket_fd = -1;
    bool socket_transferred = false;

    LoadArgs2 args;
    wasm_runtime_load_args2_init(&args);
    args.name = const_cast<char *>("socket-quota-setter-test");
    args.no_resolve = 1;
    args.is_component = 1;
    wasm_runtime_load_args2_set_allocation_quota(
        &args, &quota, socket_quota_reserve, socket_quota_release,
        socket_quota_retain, socket_quota_attachment_release);
    if (host_table_initialized) {
        component = wasm_component_load_ex2(
            component_copy.data(), static_cast<uint32_t>(component_copy.size()),
            &args, error_buf, sizeof(error_buf));
    }
    if (component) {
        libc_wasi_set_default_options(&parse_context);
        libc_component_wasi_init(component, 0, nullptr, &parse_context);
        instance = wasm_component_instantiate_internal(
            component, nullptr, error_buf, sizeof(error_buf));
    }

    WASMFunctionInstance *lower = get_lower_by_native(
        instance,
        reinterpret_cast<const void *>(
            wasi_sockets_tcp_tcp_socket_set_send_buffer_size_wrapper));
    if (lower
        && wasm_allocation_quota_scope_enter_for_allocation(&owner_scope,
                                                            component)) {
        owner_scope_active = true;
        socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        int initial_size = 0;
        socklen_t initial_size_length = sizeof(initial_size);
        const bool initial_ready =
            socket_fd >= 0
            && getsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &initial_size,
                          &initial_size_length)
                   == 0;
        const uint64_t requested_size =
            initial_size > 32768 ? static_cast<uint64_t>(initial_size / 4)
                                 : static_cast<uint64_t>(initial_size) * 4;
        const uint32_t rep =
            initial_ready ? add_tcp_socket_resource(socket_fd) : 0;
        if (rep != 0)
            socket_transferred = true;
        uint32_t handle = 0;
        if (rep != 0 && create_guest_resource_handle(lower, rep, &handle)
            && ensure_guest_memory(lower, kPayloadOffset + 64)) {
            uint32_t argv[] = { handle, static_cast<uint32_t>(requested_size),
                                static_cast<uint32_t>(requested_size >> 32),
                                kResultOffset };
            const uint32_t execution_start = quota.reserve_calls.load();
            if (denied_execution_request != kNoDeniedRequest) {
                quota.deny_at.store(execution_start + denied_execution_request);
            }
            result.invoked = invoke_lower(lower, argv, 4);
            const uint32_t execution_end = quota.reserve_calls.load();
            quota.deny_at.store(kNoDeniedRequest);
            if (execution_end >= execution_start
                && execution_end - execution_start <= quota.requests.size()) {
                result.execution_requests.assign(
                    quota.requests.begin() + execution_start,
                    quota.requests.begin() + execution_end);
            }
            if (!result.invoked) {
                wasm_runtime_clear_exception(
                    reinterpret_cast<WASMModuleInstanceCommon *>(
                        lower->module_instance));
            }
            int after_size = 0;
            socklen_t after_size_length = sizeof(after_size);
            if (getsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &after_size,
                           &after_size_length)
                == 0) {
                result.state_unchanged = after_size == initial_size;
            }

            if (denied_execution_request != kNoDeniedRequest
                && quota.denied_calls.load() != 0) {
                result.retry_invoked = invoke_lower(lower, argv, 4);
                int retry_size = 0;
                socklen_t retry_size_length = sizeof(retry_size);
                if (result.retry_invoked
                    && getsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &retry_size,
                                  &retry_size_length)
                           == 0) {
                    result.retry_changed_state = retry_size != initial_size;
                }
            }
        }
    }

    quota.deny_at.store(kNoDeniedRequest);
    if (!socket_transferred && socket_fd >= 0)
        close(socket_fd);
    if (owner_scope_active)
        wasm_allocation_quota_scope_leave(&owner_scope);
    if (instance)
        wasm_component_deinstantiate(instance);
    wasi_p2_sockets_cleanup();
    destroy_resolve_streams_map();
    if (host_table_initialized)
        destroy_host_resource_table();
    if (component)
        wasm_component_unload(component);
    wasm_runtime_destroy();

    result.denied_calls = quota.denied_calls.load();
    result.live_bytes = quota.live_bytes.load();
    result.live_allocations = quota.live_allocations.load();
    result.retain_calls = quota.retain_calls.load();
    result.attachment_release_calls = quota.attachment_release_calls.load();
    result.request_overflow = quota.overflow.load();
    return result;
}

QuotaReceiveResult
run_quota_udp_receive(const std::vector<uint8_t> &component_bytes,
                      uint32_t denied_execution_request)
{
    QuotaReceiveResult result;
    SocketQuotaState quota;
    std::vector<uint8_t> component_copy = component_bytes;
    RuntimeInitArgs init_args = {};
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init_args))
        return result;

    bool host_table_initialized = instantiate_host_resource_table();
    WASMComponent *component = nullptr;
    WASMComponentInstance *instance = nullptr;
    libc_wasi_parse_context_t parse_context = {};
    char error_buf[256] = {};
    WASMAllocationQuotaScope owner_scope = {};
    bool owner_scope_active = false;
    int sender = -1;

    LoadArgs2 args;
    wasm_runtime_load_args2_init(&args);
    args.name = const_cast<char *>("socket-quota-receive-test");
    args.no_resolve = 1;
    args.is_component = 1;
    wasm_runtime_load_args2_set_allocation_quota(
        &args, &quota, socket_quota_reserve, socket_quota_release,
        socket_quota_retain, socket_quota_attachment_release);
    if (host_table_initialized) {
        component = wasm_component_load_ex2(
            component_copy.data(), static_cast<uint32_t>(component_copy.size()),
            &args, error_buf, sizeof(error_buf));
    }
    if (component) {
        libc_wasi_set_default_options(&parse_context);
        libc_component_wasi_init(component, 0, nullptr, &parse_context);
        instance = wasm_component_instantiate_internal(
            component, nullptr, error_buf, sizeof(error_buf));
    }

    WASMFunctionInstance *lower = get_lower_by_native(
        instance,
        reinterpret_cast<const void *>(
            wasi_sockets_udp_incoming_datagram_stream_receive_wrapper));
    if (lower
        && wasm_allocation_quota_scope_enter_for_allocation(&owner_scope,
                                                            component)) {
        owner_scope_active = true;
        int receiver = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        socklen_t address_length = sizeof(address);
        bool receiver_ready =
            receiver >= 0
            && bind(receiver, reinterpret_cast<sockaddr *>(&address),
                    sizeof(address))
                   == 0
            && getsockname(receiver, reinterpret_cast<sockaddr *>(&address),
                           &address_length)
                   == 0;
        int flags = receiver_ready ? fcntl(receiver, F_GETFL, 0) : -1;
        receiver_ready = receiver_ready && flags >= 0
                         && fcntl(receiver, F_SETFL, flags | O_NONBLOCK) == 0;
        sender = socket(AF_INET, SOCK_DGRAM, 0);
        const uint32_t rep =
            receiver_ready ? add_udp_incoming_stream_resource(receiver) : 0;
        uint32_t handle = 0;
        if (rep != 0 && sender >= 0
            && create_guest_resource_handle(lower, rep, &handle)) {
            const uint8_t payload = 0x6d;
            if (sendto(sender, &payload, sizeof(payload), 0,
                       reinterpret_cast<sockaddr *>(&address), sizeof(address))
                == 1) {
                pollfd ready = { .fd = receiver,
                                 .events = POLLIN,
                                 .revents = 0 };
                if (poll(&ready, 1, 1000) != 1 || (ready.revents & POLLIN) == 0)
                    goto receive_cleanup;
                uint32_t argv[] = { handle, 1, 0, kResultOffset };
                const uint32_t execution_start = quota.reserve_calls.load();
                if (denied_execution_request != kNoDeniedRequest) {
                    quota.deny_at.store(execution_start
                                        + denied_execution_request);
                }
                result.invoked = invoke_lower(lower, argv, 4);
                const uint32_t execution_end = quota.reserve_calls.load();
                quota.deny_at.store(kNoDeniedRequest);
                if (execution_end >= execution_start
                    && execution_end - execution_start
                           <= quota.requests.size()) {
                    result.execution_requests.assign(
                        quota.requests.begin() + execution_start,
                        quota.requests.begin() + execution_end);
                }
                if (!result.invoked) {
                    wasm_runtime_clear_exception(
                        reinterpret_cast<WASMModuleInstanceCommon *>(
                            lower->module_instance));
                }
                result.packet_after_first = receiver_has_packet(receiver, 100);

                if (denied_execution_request != kNoDeniedRequest
                    && quota.denied_calls.load() != 0) {
                    wasm_runtime_clear_exception(
                        reinterpret_cast<WASMModuleInstanceCommon *>(
                            lower->module_instance));
                    memset(lower_memory(lower)->memory_data + kResultOffset,
                           0xa5, 16);
                    result.retry_invoked = invoke_lower(lower, argv, 4);
                    wit_value_t retry_result = nullptr;
                    if (result.retry_invoked
                        && load_lower_result(lower, &retry_result)
                        && retry_result
                        && !retry_result->value.result_value.is_err
                        && retry_result->value.result_value.result.ok
                        && retry_result->value.result_value.result.ok->value
                                   .list_value.size
                               == 1) {
                        result.retry_success = true;
                    }
                    free_wit_value(retry_result);
                    result.packet_after_retry =
                        receiver_has_packet(receiver, 100);
                }
            }
        }
        else if (receiver >= 0 && rep == 0) {
            close(receiver);
        }
    }

receive_cleanup:

    quota.deny_at.store(kNoDeniedRequest);
    if (sender >= 0)
        close(sender);
    if (owner_scope_active)
        wasm_allocation_quota_scope_leave(&owner_scope);
    if (instance)
        wasm_component_deinstantiate(instance);
    wasi_p2_sockets_cleanup();
    destroy_resolve_streams_map();
    if (host_table_initialized)
        destroy_host_resource_table();
    if (component)
        wasm_component_unload(component);
    wasm_runtime_destroy();

    result.denied_calls = quota.denied_calls.load();
    result.live_bytes = quota.live_bytes.load();
    result.live_allocations = quota.live_allocations.load();
    result.retain_calls = quota.retain_calls.load();
    result.attachment_release_calls = quota.attachment_release_calls.load();
    result.request_overflow = quota.overflow.load();
    return result;
}

TEST(SocketPublicationQuotaTest,
     UdpReceiveAllocationDenialsLeavePacketQueuedForRetry)
{
    const std::vector<uint8_t> component_bytes = read_socket_component();
    ASSERT_FALSE(component_bytes.empty());

    const QuotaReceiveResult baseline =
        run_quota_udp_receive(component_bytes, kNoDeniedRequest);
    ASSERT_TRUE(baseline.invoked);
    EXPECT_FALSE(baseline.packet_after_first);
    ASSERT_FALSE(baseline.execution_requests.empty());
    ASSERT_FALSE(baseline.request_overflow);
    ASSERT_EQ(baseline.live_bytes, 0u);
    ASSERT_EQ(baseline.live_allocations, 0u);
    ASSERT_EQ(baseline.retain_calls, baseline.attachment_release_calls);

    for (uint32_t denied_request = 0;
         denied_request < baseline.execution_requests.size();
         denied_request++) {
        SCOPED_TRACE(denied_request);
        const QuotaReceiveResult denied =
            run_quota_udp_receive(component_bytes, denied_request);
        ASSERT_FALSE(denied.request_overflow);
        ASSERT_GE(denied.denied_calls, 1u);
        EXPECT_TRUE(denied.packet_after_first);
        EXPECT_TRUE(denied.retry_invoked);
        EXPECT_TRUE(denied.retry_success);
        EXPECT_FALSE(denied.packet_after_retry);
        EXPECT_EQ(denied.live_bytes, 0u);
        EXPECT_EQ(denied.live_allocations, 0u);
        EXPECT_EQ(denied.retain_calls, denied.attachment_release_calls);
    }
}

TEST(SocketPublicationQuotaTest,
     SocketSetterAllocationDenialsPrecedeNativeMutation)
{
    const std::vector<uint8_t> component_bytes = read_socket_component();
    ASSERT_FALSE(component_bytes.empty());

    const QuotaSetterResult baseline =
        run_quota_tcp_send_buffer_setter(component_bytes, kNoDeniedRequest);
    ASSERT_TRUE(baseline.invoked);
    ASSERT_FALSE(baseline.execution_requests.empty());
    EXPECT_FALSE(baseline.state_unchanged);
    EXPECT_FALSE(baseline.request_overflow);
    EXPECT_EQ(baseline.live_bytes, 0u);
    EXPECT_EQ(baseline.live_allocations, 0u);
    EXPECT_EQ(baseline.retain_calls, baseline.attachment_release_calls);

    for (uint32_t denied_request = 0;
         denied_request < baseline.execution_requests.size();
         denied_request++) {
        SCOPED_TRACE(denied_request);
        const QuotaSetterResult denied =
            run_quota_tcp_send_buffer_setter(component_bytes, denied_request);
        ASSERT_FALSE(denied.request_overflow);
        ASSERT_GE(denied.denied_calls, 1u);
        EXPECT_TRUE(denied.state_unchanged);
        EXPECT_TRUE(denied.retry_invoked);
        EXPECT_TRUE(denied.retry_changed_state);
        EXPECT_EQ(denied.live_bytes, 0u);
        EXPECT_EQ(denied.live_allocations, 0u);
        EXPECT_EQ(denied.retain_calls, denied.attachment_release_calls);
    }
}

} // namespace
