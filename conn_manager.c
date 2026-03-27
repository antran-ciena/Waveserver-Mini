#include "common.h"
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>

#define SERVICE_NAME "conn_mgr"

static conn_t conns[MAX_CONNS];
static int client_socket;

void initialize_connections()
{
    memset(conns, 0, sizeof(conns));
    LOG(LOG_INFO, "Initialize connection table");
}

bool get_port_info(uint8_t port_id, port_t *out)
{
    udp_message_t req = {0};
    req.msg_type = MSG_GET_PORT_INFO;
    req.status = STATUS_REQUEST;

    udp_port_cmd_request_t *payload = (udp_port_cmd_request_t *)req.payload;
    payload->port_id = port_id;

    udp_message_t resp = {0};
    if (!send_udp_message_and_receive(client_socket, &req, &resp, PORT_MANAGER_UDP))
    {
        LOG(LOG_ERROR, "send/receive failed for port-%d", port_id);
        return false;
    }

    if (resp.status != STATUS_SUCCESS)
    {
        LOG(LOG_ERROR, "port-%d not found", port_id);
        return false;
    }

    memcpy(out, resp.payload, sizeof(*out));
    return true;
}

conn_t *find_connection_by_name(const char *name)
{
    for (int i = 0; i < MAX_CONNS; i++)
    {
        if (conns[i].client_port != 0 &&
            strncmp(conns[i].conn_name, name, MAX_CONN_NAME_CHARACTER) == 0)
            return &conns[i];
    }
    return NULL;
}

void handle_port_state_change(const udp_message_t *req)
{
    const udp_port_state_change_t *payload = (udp_port_state_change_t *)req->payload;
    uint8_t port_id = payload->port_id;

    if (port_id < 1 || port_id > MAX_PORT_NUM) {
        LOG(LOG_ERROR, "Port-%d out of range");
        return;
    }

    LOG(LOG_INFO, "Port-%d operational_state changed to %s",
        port_id,
        payload->operational_state == PORT_UP ? "UP" : "DOWN");

    for (int i = 0; i < MAX_CONNS; i++)
    {
        if (conns[i].client_port == 0)
            continue; // empty slot

        if (conns[i].client_port == port_id || conns[i].line_port == port_id)
        {
            if (payload->operational_state == PORT_DOWN)
            {
                conns[i].operational_state = CONN_DOWN;
                LOG(LOG_WARN, "Connection %s DOWN (port-%d went down)", conns[i].conn_name, port_id);
            }
            else
            {
                // check if the other port is also up
                uint8_t other_port = (conns[i].client_port == port_id) ? conns[i].line_port : conns[i].client_port;
                port_t other_port_info = {0};
                if (get_port_info(other_port, &other_port_info) == false)
                {
                    LOG(LOG_ERROR, "handle port up: Could not get info for port-%d, leaving connection %s DOWN",
                        other_port,
                        conns[i].conn_name);
                }
                else if (other_port_info.operational_state == PORT_UP)
                {
                    conns[i].operational_state = CONN_UP;
                    LOG(LOG_INFO, "Connection %s UP (port-%d recovered, port-%d also UP)",
                        conns[i].conn_name,
                        port_id,
                        other_port);
                }
                else
                {
                    LOG(LOG_WARN, "Connection %s DOWN (port-%d recovered, but port-%d still DOWN)",
                        conns[i].conn_name,
                        port_id,
                        other_port);
                }
            }
        }
    }
}

void handle_lookup_connection(const udp_message_t *req, udp_message_t *resp)
{
    const udp_route_lookup_request_t *payload = (const udp_route_lookup_request_t *)req->payload;

    conn_t *conn = NULL;
    for (int i = 0; i < MAX_CONNS; i++)
    {
        if (conns[i].client_port == payload->client_port &&
            conns[i].line_port == payload->line_port)
        {
            conn = &conns[i];
            break;
        }
    }

    if (conn == NULL)
    {
        LOG(LOG_WARN, "MSG_LOOKUP_CONNECTION: no connection for client port-%d and line port-%d",
            payload->client_port,
            payload->line_port);
        resp->status = STATUS_FAILURE;
        return;
    }

    udp_route_lookup_reply_t *reply = (udp_route_lookup_reply_t *)resp->payload;

    strncpy(reply->conn_name, conn->conn_name, sizeof(reply->conn_name) - 1);
    reply->operational_state = (uint8_t)conn->operational_state;
    resp->status = STATUS_SUCCESS;

    LOG(LOG_DEBUG,
        "MSG_LOOKUP_CONNECTION: client-%d → line-%d via %s operational_state=%s",
        payload->client_port, conn->line_port, conn->conn_name,
        conn->operational_state == CONN_UP ? "UP" : "DOWN");
}

void handle_create_connection(const udp_message_t *req, udp_message_t *resp)
{
    const udp_create_conn_request_t *payload = (const udp_create_conn_request_t *)req->payload;

    // Validation 4: name length must be 1–31 characters
    if (payload->name[0] == '\0' || payload->name[MAX_CONN_NAME_CHARACTER - 1] != '\0')
    {
        set_error_msg(resp, "Connection name must be between 1 and 31 characters");
        LOG(LOG_ERROR, "Create connection rejected: invalid name length");
        return;
    }

    // Validation 1: one port must be a line port (1–2), the other a client port (3–6)
    uint8_t client_port = 0;
    uint8_t line_port   = 0;

    if (payload->client_port >= 3 && payload->client_port <= 6 &&
        payload->line_port   >= 1 && payload->line_port   <= 2)
    {
        client_port = payload->client_port;
        line_port   = payload->line_port;
    }
    else
    {
        set_error_msg(resp, "One port must be a line port (1-2) and the other a client port (3-6)");
        LOG(LOG_ERROR, "Create connection rejected: invalid port types (client=%d, line=%d)",
            payload->client_port, payload->line_port);
        return;
    }

    // Validation 4 (continued): name must be unique
    if (find_connection_by_name(payload->name) != NULL)
    {
        set_error_msg(resp, "A connection with that name already exists");
        LOG(LOG_ERROR, "Create connection rejected: duplicate name '%s'", payload->name);
        return;
    }

    // Validation 3: client port must not already be in a connection
    for (int i = 0; i < MAX_CONNS; i++)
    {
        if (conns[i].client_port == client_port)
        {
            set_error_msg(resp, "Client port is already used by an existing connection");
            LOG(LOG_ERROR, "Create connection rejected: client port-%d already in use", client_port);
            return;
        }
    }

    // Validation 2: both ports must be operationally UP
    port_t client_port_info = {0};
    if (!get_port_info(client_port, &client_port_info))
    {
        set_error_msg(resp, "Could not retrieve client port info");
        LOG(LOG_ERROR, "Create connection rejected: failed to query client port-%d", client_port);
        return;
    }
    if (client_port_info.operational_state != PORT_UP)
    {
        set_error_msg(resp, "Client port is not operationally UP");
        LOG(LOG_ERROR, "Create connection rejected: client port-%d is DOWN", client_port);
        return;
    }

    port_t line_port_info = {0};
    if (!get_port_info(line_port, &line_port_info))
    {
        set_error_msg(resp, "Could not retrieve line port info");
        LOG(LOG_ERROR, "Create connection rejected: failed to query line port-%d", line_port);
        return;
    }
    if (line_port_info.operational_state != PORT_UP)
    {
        set_error_msg(resp, "Line port is not operationally UP");
        LOG(LOG_ERROR, "Create connection rejected: line port-%d is DOWN", line_port);
        return;
    }

    // Find an empty slot (client_port == 0 marks an empty entry)
    conn_t *slot = NULL;
    for (int i = 0; i < MAX_CONNS; i++)
    {
        if (conns[i].client_port == 0)
        {
            slot = &conns[i];
            break;
        }
    }
    if (slot == NULL)
    {
        set_error_msg(resp, "Connection table is full (max 4 connections)");
        LOG(LOG_ERROR, "Create connection rejected: table full");
        return;
    }

    // Commit the new connection
    strncpy(slot->conn_name, payload->name, MAX_CONN_NAME_CHARACTER - 1);
    slot->conn_name[MAX_CONN_NAME_CHARACTER - 1] = '\0';
    slot->client_port        = client_port;
    slot->line_port          = line_port;
    slot->operational_state  = CONN_UP;

    resp->status = STATUS_SUCCESS;
    LOG(LOG_INFO, "Connection '%s' created: client port-%d → line port-%d",
        slot->conn_name, client_port, line_port);
}

void handle_get_connections(udp_message_t *resp)
{
    resp->status = STATUS_SUCCESS;

    udp_get_connections_reply_t *resp_payload = (udp_get_connections_reply_t *)resp->payload;
    int count = 0;

    for (int i = 0; i < MAX_CONNS; i++)
    {
        if (conns[i].line_port > 0 && conns[i].client_port > 0) {
            resp_payload->all_connections[count].client_port = conns[i].client_port;
            resp_payload->all_connections[count].line_port = conns[i].line_port;
            resp_payload->all_connections[count].operational_state = conns[i].operational_state;
            strncpy(resp_payload->all_connections[count].conn_name, conns[i].conn_name, MAX_CONN_NAME_CHARACTER - 1);
            count++;
        }
    }

    resp_payload->conn_count = count;
}

void handle_delete_conn(const udp_message_t *req, udp_message_t *resp)
{
    const udp_delete_conn_request_t *udp_payload = (const udp_delete_conn_request_t *)req->payload;
    const char *err = NULL;
    conn_t *found_conn = find_connection_by_name(udp_payload->name);

    if (found_conn == NULL) {
        err = "could not find connection with that name";
        LOG(LOG_ERROR, "%s (name=%s)", err, udp_payload->name);
        set_error_msg(resp, err);
        return;
    }

    found_conn->client_port = 0;
    found_conn->line_port = 0;
    found_conn->operational_state = CONN_DOWN;
    found_conn->conn_name[0] = '\0';
    resp->status = STATUS_SUCCESS;
    LOG(LOG_INFO, "deleted connection '%s'", udp_payload->name);
    
}

bool dispatch(const udp_message_t *req, udp_message_t *resp)
{
    bool send_reply = false;
    resp->msg_type = req->msg_type;

    switch ((msg_type_t)req->msg_type)
    {
    case MSG_PORT_STATE_CHANGE:
        handle_port_state_change(req);
        break;
    case MSG_LOOKUP_CONNECTION:
        handle_lookup_connection(req, resp);
        send_reply = true;
        break;
    case MSG_CREATE_CONN:
        handle_create_connection(req, resp);
        send_reply = true;
        break;
    case MSG_GET_CONNECTIONS:
        handle_get_connections(resp);
        send_reply = true;
        break;
    case MSG_DELETE_CONN:
        handle_delete_conn(req, resp);
        send_reply = true;
        break;
    default:
        LOG(LOG_WARN, "Unknown msg_type: %d", req->msg_type);
        break;
    }

    return send_reply;
}

int main()
{
    log_init(SERVICE_NAME);
    initialize_connections();

    int server_socket = create_udp_server(CONN_MANAGER_UDP);
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

    while (true)
    {
        udp_message_t req = {0};
        struct sockaddr_in sender = {0};
        socklen_t sender_len = sizeof(sender);

        ssize_t n = recvfrom(server_socket, &req, sizeof(req), 0, (struct sockaddr *)&sender, &sender_len);
        if (n < 0)
        {
            LOG(LOG_ERROR, "recvfrom failed");
            continue;
        }

        udp_message_t resp = {0};
        if (dispatch(&req, &resp) &&
            (sendto(server_socket, &resp, sizeof(resp), 0, (struct sockaddr *)&sender, sender_len) < 0))
        {
            LOG(LOG_ERROR, "sendto reply failed");
        }
    }

    return 0;
}
