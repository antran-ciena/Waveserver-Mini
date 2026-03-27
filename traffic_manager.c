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
    // TODO: F1 — Traffic Generation (/8 pts)
    //
    // Generate an OTN frame, query the connection manager for a valid route,
    // and forward or drop accordingly. Update both local traffic stats and
    // the port manager's per-port counters.
    //
    // Refer to common.h for the relevant structs and message types.
    // A port value of 0 in stats means "randomize within its valid range."
    //Note: Generate a frame using stats.client_port and stats.line_port. 
    //A value of 0 means "randomize within its valid range" (client: 3–6, line: 1–2)

    static const char *payloads[] = {
        "Data payload alpha",
        "Waveserver mini frame",
        "OTN client traffic",
        "Forwarding test pattern"
    };
    otn_frame_t frame = {0};
    udp_message_t req = {0};
    udp_message_t resp = {0};
    udp_message_t counter_msg = {0};
    udp_route_lookup_request_t *lookup = (udp_route_lookup_request_t *)req.payload;
    udp_route_lookup_reply_t *route = (udp_route_lookup_reply_t *)resp.payload;
    udp_counter_update_t *counter = (udp_counter_update_t *)counter_msg.payload;

    frame.header.client_port = stats.client_port == 0 ? (uint8_t)(3 + (rand() % MAX_CLIENT_PORTS)) : stats.client_port;
    frame.header.line_port = stats.line_port == 0 ? (uint8_t)(1 + (rand() % MAX_LINE_PORTS)) : stats.line_port;
    frame.header.frame_id = stats.next_frame_id++;
    strncpy(frame.data, payloads[rand() % (sizeof(payloads) / sizeof(payloads[0]))], sizeof(frame.data) - 1);

    LOG(LOG_DEBUG, "Frame #%u generated: client-%u -> line-%u msg=\"%s\"",
        frame.header.frame_id, frame.header.client_port, frame.header.line_port, frame.data);

    req.msg_type = MSG_LOOKUP_CONNECTION;
    req.status = STATUS_REQUEST;
    lookup->client_port = frame.header.client_port;
    lookup->line_port = frame.header.line_port;

    counter_msg.msg_type = MSG_UPDATE_COUNTERS;
    counter_msg.status = STATUS_REQUEST;

    if (!send_udp_message_and_receive(client_socket, &req, &resp, CONN_MANAGER_UDP) ||
        resp.status != STATUS_SUCCESS ||
        route->operational_state != CONN_UP)
    {
        stats.total_dropped++;
        counter->port_id = frame.header.client_port;
        counter->pkts_rx = 0;
        counter->pkts_dropped = 1;
        send_udp_message_one_way(client_socket, &counter_msg, PORT_MANAGER_UDP);
        LOG(LOG_WARN, "Frame #%u DROPPED: no connection for client-%u line-%u",
            frame.header.frame_id, frame.header.client_port, frame.header.line_port);
        return;
    }
    stats.total_forwarded++;
    counter->port_id = frame.header.line_port;
    counter->pkts_rx = 1;
    counter->pkts_dropped = 0;
    send_udp_message_one_way(client_socket, &counter_msg, PORT_MANAGER_UDP);
    LOG(LOG_DEBUG, "Frame #%u forwarded: Client-%u -> Line-%u via %s",
        frame.header.frame_id, frame.header.client_port, frame.header.line_port, route->conn_name);

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