#include "common.h"
#include <errno.h>

#define SERVICE_NAME "protection_mgr"

typedef struct
{
    char conn_name[MAX_CONN_NAME_CHARACTER];
    uint8_t client_port;
    uint8_t original_line_port;
    uint8_t current_line_port;
    uint8_t operational_state;
    bool is_switched;
} tracked_conn_t;

static bool group_active = false;
static uint32_t switchover_events = 0;
static tracked_conn_t tracked[MAX_CONNS];
static uint8_t tracked_count = 0;

static bool line_fault[3] = {false, false, false}; // indexes 1..2 used
static int client_socket = -1;

static bool get_port_info(uint8_t port_id, port_t *out)
{
    udp_message_t req = {0};
    req.msg_type = MSG_GET_PORT_INFO;
    req.status = STATUS_REQUEST;

    udp_port_cmd_request_t *payload = (udp_port_cmd_request_t *)req.payload;
    payload->port_id = port_id;

    udp_message_t resp = {0};
    if (!send_udp_message_and_receive(client_socket, &req, &resp, PORT_MANAGER_UDP))
        return false;

    if (resp.status != STATUS_SUCCESS)
        return false;

    memcpy(out, resp.payload, sizeof(*out));
    return true;
}

static bool get_connections(udp_get_connections_reply_t *out)
{
    udp_message_t req = {0};
    req.msg_type = MSG_GET_CONNECTIONS;
    req.status = STATUS_REQUEST;

    udp_message_t resp = {0};
    if (!send_udp_message_and_receive(client_socket, &req, &resp, CONN_MANAGER_UDP))
        return false;

    if (resp.status != STATUS_SUCCESS)
        return false;

    memcpy(out, resp.payload, sizeof(*out));
    return true;
}

static int find_tracked_idx(const char *name)
{
    for (int i = 0; i < tracked_count; i++)
    {
        if (strncmp(tracked[i].conn_name, name, MAX_CONN_NAME_CHARACTER) == 0)
            return i;
    }
    return -1;
}

static bool switch_connection_line(const char *name, uint8_t new_line)
{
    udp_message_t req = {0};
    req.msg_type = MSG_SWITCH_CONN_LINE_PORT;
    req.status = STATUS_REQUEST;

    udp_switch_conn_line_request_t *payload = (udp_switch_conn_line_request_t *)req.payload;
    strncpy(payload->name, name, sizeof(payload->name) - 1);
    payload->new_line_port = new_line;

    udp_message_t resp = {0};
    if (!send_udp_message_and_receive(client_socket, &req, &resp, CONN_MANAGER_UDP))
        return false;

    return resp.status == STATUS_SUCCESS;
}

static void remove_tracked_at(uint8_t idx)
{
    for (uint8_t i = idx; i + 1 < tracked_count; i++)
        tracked[i] = tracked[i + 1];
    if (tracked_count > 0)
        tracked_count--;
}

static void sync_tracked_connections(void)
{
    if (!group_active)
        return;

    udp_get_connections_reply_t table = {0};
    if (!get_connections(&table))
        return;

    bool exists[MAX_CONNS] = {false};

    for (int i = 0; i < table.conn_count; i++)
    {
        conn_t *c = &table.all_connections[i];
        if (c->line_port < 1 || c->line_port > 2)
            continue;

        int idx = find_tracked_idx(c->conn_name);
        if (idx < 0)
        {
            if (tracked_count >= MAX_CONNS)
                continue;

            idx = tracked_count++;
            memset(&tracked[idx], 0, sizeof(tracked[idx]));
            strncpy(tracked[idx].conn_name, c->conn_name, MAX_CONN_NAME_CHARACTER - 1);
            tracked[idx].original_line_port = c->line_port;
            tracked[idx].current_line_port = c->line_port;
            tracked[idx].is_switched = false;
        }

        tracked[idx].client_port = c->client_port;
        tracked[idx].current_line_port = c->line_port;
        tracked[idx].operational_state = c->operational_state;
        tracked[idx].is_switched = (tracked[idx].current_line_port != tracked[idx].original_line_port);
        exists[idx] = true;
    }

    for (int i = tracked_count - 1; i >= 0; i--)
    {
        if (!exists[i])
            remove_tracked_at((uint8_t)i);
    }
}

static void perform_switchover(uint8_t faulted_line)
{
    if (!group_active)
        return;

    uint8_t peer = (faulted_line == 1) ? 2 : 1;
    if (line_fault[peer])
        return;

    for (int i = 0; i < tracked_count; i++)
    {
        if (tracked[i].current_line_port != faulted_line)
            continue;

        if (switch_connection_line(tracked[i].conn_name, peer))
        {
            LOG(LOG_INFO, "Protection switchover: %s moved from port-%u -> port-%u",
                tracked[i].conn_name, faulted_line, peer);
            tracked[i].current_line_port = peer;
            tracked[i].is_switched = (tracked[i].current_line_port != tracked[i].original_line_port);
            switchover_events++;
        }
    }

    sync_tracked_connections();
}

static void perform_revertive_switch(uint8_t recovered_line)
{
    if (!group_active)
        return;

    for (int i = 0; i < tracked_count; i++)
    {
        if (!tracked[i].is_switched)
            continue;
        if (tracked[i].original_line_port != recovered_line)
            continue;

        uint8_t from = tracked[i].current_line_port;
        if (switch_connection_line(tracked[i].conn_name, recovered_line))
        {
            LOG(LOG_INFO, "Revertive switch: %s moved from port-%u -> port-%u",
                tracked[i].conn_name, from, recovered_line);
            tracked[i].current_line_port = recovered_line;
            tracked[i].is_switched = false;
            switchover_events++;
        }
    }

    sync_tracked_connections();
}

static void handle_set_protection_group(udp_message_t *resp)
{
    if (group_active)
    {
        set_error_msg(resp, "protection group already active");
        return;
    }

    port_t p1 = {0};
    port_t p2 = {0};
    if (!get_port_info(1, &p1) || !get_port_info(2, &p2))
    {
        set_error_msg(resp, "failed to read line port states");
        return;
    }

    if (p1.type != LINE_PORT || p2.type != LINE_PORT)
    {
        set_error_msg(resp, "ports 1 and 2 must be line ports");
        return;
    }

    if (!p1.admin_enabled || !p2.admin_enabled)
    {
        set_error_msg(resp, "both line ports must be admin-enabled");
        return;
    }

    group_active = true;
    switchover_events = 0;
    tracked_count = 0;
    line_fault[1] = p1.fault_active;
    line_fault[2] = p2.fault_active;

    sync_tracked_connections();
    resp->status = STATUS_SUCCESS;
}

static void handle_delete_protection_group(udp_message_t *resp)
{
    if (!group_active)
    {
        set_error_msg(resp, "no active protection group");
        return;
    }

    for (int i = 0; i < tracked_count; i++)
    {
        if (!tracked[i].is_switched)
            continue;

        if (switch_connection_line(tracked[i].conn_name, tracked[i].original_line_port))
        {
            tracked[i].current_line_port = tracked[i].original_line_port;
            tracked[i].is_switched = false;
            switchover_events++;
        }
    }

    group_active = false;
    tracked_count = 0;
    line_fault[1] = false;
    line_fault[2] = false;
    resp->status = STATUS_SUCCESS;
}

static void handle_get_protection_group(udp_message_t *resp)
{
    sync_tracked_connections();

    udp_protection_group_reply_t *reply = (udp_protection_group_reply_t *)resp->payload;
    memset(reply, 0, sizeof(*reply));

    reply->active = group_active;
    reply->line_port_a = 1;
    reply->line_port_b = 2;
    reply->switchover_events = switchover_events;
    reply->conn_count = tracked_count;

    for (int i = 0; i < tracked_count; i++)
    {
        strncpy(reply->conns[i].conn_name, tracked[i].conn_name, MAX_CONN_NAME_CHARACTER - 1);
        reply->conns[i].client_port = tracked[i].client_port;
        reply->conns[i].original_line_port = tracked[i].original_line_port;
        reply->conns[i].current_line_port = tracked[i].current_line_port;
        reply->conns[i].operational_state = tracked[i].operational_state;
        reply->conns[i].is_switched = tracked[i].is_switched;
    }

    resp->status = STATUS_SUCCESS;
}

static void handle_port_state_change(const udp_message_t *req)
{
    const udp_port_state_change_t *payload = (const udp_port_state_change_t *)req->payload;
    if (!group_active)
        return;

    if (payload->port_id != 1 && payload->port_id != 2)
        return;

    bool prev_fault = line_fault[payload->port_id];
    line_fault[payload->port_id] = payload->fault_active;

    if (!prev_fault && payload->fault_active)
    {
        perform_switchover(payload->port_id);
        return;
    }

    if (prev_fault && !payload->fault_active && payload->operational_state == PORT_UP)
    {
        perform_revertive_switch(payload->port_id);
    }
}

static bool dispatch(const udp_message_t *req, udp_message_t *resp)
{
    bool send_reply = true;

    resp->msg_type = req->msg_type;
    resp->status = STATUS_FAILURE;

    switch ((msg_type_t)req->msg_type)
    {
    case MSG_SET_PROTECTION_GROUP:
        handle_set_protection_group(resp);
        break;
    case MSG_DELETE_PROTECTION_GROUP:
        handle_delete_protection_group(resp);
        break;
    case MSG_GET_PROTECTION_GROUP:
        handle_get_protection_group(resp);
        break;
    case MSG_PORT_STATE_CHANGE:
        handle_port_state_change(req);
        send_reply = false;
        break;
    default:
        LOG(LOG_WARN, "Unknown msg_type: %d", req->msg_type);
        send_reply = false;
        break;
    }

    return send_reply;
}

int main(void)
{
    log_init(SERVICE_NAME);

    int server_socket = create_udp_server(PROTECTION_MGR_UDP);
    if (server_socket < 0)
    {
        LOG(LOG_ERROR, "Failed to create server socket - exiting");
        return 1;
    }

    client_socket = create_udp_client();
    if (client_socket < 0)
    {
        LOG(LOG_ERROR, "Failed to create client socket - exiting");
        return 1;
    }

    struct timeval rx_timeout = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, &rx_timeout, sizeof(rx_timeout));

    while (true)
    {
        udp_message_t req = {0};
        struct sockaddr_in sender;
        socklen_t sender_len = sizeof(sender);

        ssize_t n = recvfrom(server_socket, &req, sizeof(req), 0, (struct sockaddr *)&sender, &sender_len);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG(LOG_ERROR, "recvfrom failed");
        }
        else if (n > 0)
        {
            udp_message_t resp = {0};
            if (dispatch(&req, &resp) &&
                (sendto(server_socket, &resp, sizeof(resp), 0, (struct sockaddr *)&sender, sender_len) < 0))
            {
                LOG(LOG_ERROR, "sendto reply failed");
            }
        }

        sync_tracked_connections();
    }

    return 0;
}
