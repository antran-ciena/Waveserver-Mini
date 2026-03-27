#include "common.h"
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#define SERVICE_NAME "traffic_mgr"

static traffic_stats_t stats;
static int client_socket = 0;

void initialize_stats()
{
    memset(&stats, 0, sizeof(stats));
    stats.next_frame_id = 1; // Start from 1
    LOG(LOG_INFO, "Traffic stats initialized");
}

void generate_traffic()
{
    // if the user pinned a port use that, otherwise randomize
    // client ports are 3-6 and line ports are 1-2
    uint8_t client_port = stats.client_port;
    uint8_t line_port = stats.line_port;
    if (client_port == 0)
        client_port = 3 + (rand() % MAX_CLIENT_PORTS);
    if (line_port == 0)
        line_port = 1 + (rand() % MAX_LINE_PORTS);

    // build the OTN frame with a sequential ID and the chosen ports
    otn_frame_t frame = {0};
    frame.header.client_port = client_port;
    frame.header.line_port = line_port;
    frame.header.frame_id = stats.next_frame_id++;
    snprintf(frame.data, sizeof(frame.data), "frame-%u", frame.header.frame_id);

    // ask conn manager if there's a valid route for this client->line pair
    udp_message_t req = {0};
    req.msg_type = MSG_LOOKUP_CONNECTION;
    req.status = STATUS_REQUEST;
    udp_route_lookup_request_t *lookup = (udp_route_lookup_request_t *)req.payload;
    lookup->client_port = client_port;
    lookup->line_port = line_port;

    udp_message_t resp = {0};
    bool forwarded = false;

    // if conn manager finds a matching connection and its UP, forward the frame
    if (send_udp_message_and_receive(client_socket, &req, &resp, CONN_MANAGER_UDP) &&
        resp.status == STATUS_SUCCESS)
    {
        udp_route_lookup_reply_t *reply = (udp_route_lookup_reply_t *)resp.payload;
        if (reply->operational_state == CONN_UP)
        {
            forwarded = true;
            stats.total_forwarded++;
            LOG(LOG_INFO, "Frame %u forwarded: client-%d → line-%d via %s",
                frame.header.frame_id, client_port, line_port, reply->conn_name);
        }
    }

    // no connection or connection is DOWN, drop the frame
    if (!forwarded)
    {
        stats.total_dropped++;
        LOG(LOG_WARN, "Frame %u dropped: client-%d → line-%d (no route or conn DOWN)",
            frame.header.frame_id, client_port, line_port);
    }

    // tell port manager about this frame so it can update per-port counters
    // this is fire-and-forget, we dont wait for a reply
    udp_message_t counter_msg = {0};
    counter_msg.msg_type = MSG_UPDATE_COUNTERS;
    counter_msg.status = STATUS_REQUEST;
    udp_counter_update_t *counter = (udp_counter_update_t *)counter_msg.payload;
    counter->port_id = client_port;
    counter->pkts_rx = 1;
    counter->pkts_dropped = forwarded ? 0 : 1;
    send_udp_message_one_way(client_socket, &counter_msg, PORT_MANAGER_UDP);
}

void handle_get_traffic_stats(udp_message_t *resp)
{
    resp->msg_type = MSG_GET_TRAFFIC_STATS;
    resp->status = STATUS_SUCCESS;
    memcpy(resp->payload, &stats, sizeof(stats));
    LOG(LOG_DEBUG, "Returning traffic stats");
}

void handle_start_traffic(const udp_message_t *req, udp_message_t *resp)
{
    const udp_start_traffic_request_t *udp_request = (const udp_start_traffic_request_t *)req->payload;
    stats.client_port = udp_request->client_port;
    stats.line_port = udp_request->line_port;

    resp->status = STATUS_FAILURE;

    if (stats.line_port != 0 && (stats.line_port < 1 || stats.line_port > 2)) 
    {
        LOG(LOG_ERROR, "[ERROR] Line port must be 1 or 2, got %d\n", stats.line_port);
        return;
    }

    if (stats.client_port != 0 && (stats.client_port < 3 || stats.client_port > 6))
    {
        LOG(LOG_ERROR, "[ERROR] Client port must be 3–6, got %d\n", stats.client_port);
        return;
    }

    stats.running = true;
    resp->status = STATUS_SUCCESS;
    LOG(LOG_INFO, "Traffic started (client=%u, line=%u, 0=random)",
        stats.client_port, stats.line_port);
}

void handle_stop_traffic(udp_message_t *resp)
{
    // TODO: F4 — Stop Traffic Handler (/2 pts)
}

bool dispatch(const udp_message_t *req, udp_message_t *resp)
{
    bool send_reply = true;

    resp->msg_type = req->msg_type;
    resp->status = STATUS_FAILURE;

    switch ((msg_type_t)req->msg_type)
    {
    case MSG_GET_TRAFFIC_STATS:
        handle_get_traffic_stats(resp);
        break;
    case MSG_START_TRAFFIC:
    {
        handle_start_traffic(req, resp);
        break;
    }
    case MSG_STOP_TRAFFIC:
        handle_stop_traffic(resp);
        break;
    default:
        LOG(LOG_WARN, "Unknown msg_type: %d", req->msg_type);
        send_reply = false;
        break;
    }

    return send_reply;
}

int main()
{
    log_init(SERVICE_NAME);
    initialize_stats();
    srand(time(NULL)); // Seed random

    int server_socket = create_udp_server(TRAFFIC_MGR_UDP);
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

    time_t last_traffic = time(NULL);

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

        time_t now = time(NULL);
        if (stats.running && now - last_traffic >= 3)
        {
            generate_traffic();
            last_traffic = now;
        }
    }
    return 0;
}