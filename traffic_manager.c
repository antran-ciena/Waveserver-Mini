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

void send_counter_update(uint8_t port_id, uint32_t pkts_rx, uint32_t pkts_dropped)
{
    udp_message_t msg = {0};
    msg.msg_type = MSG_UPDATE_COUNTERS;
    msg.status = STATUS_REQUEST;

    udp_counter_update_t *payload = (udp_counter_update_t *)msg.payload;
    payload->port_id = port_id;
    payload->pkts_rx = pkts_rx;
    payload->pkts_dropped = pkts_dropped;

    send_udp_message_one_way(client_socket, &msg, PORT_MANAGER_UDP);
}

void generate_traffic()
{
    // 1. Determine client and line ports (randomize if 0)
    uint8_t client = stats.client_port;
    uint8_t line   = stats.line_port;

    if (client == 0)
        client = 3 + (rand() % 4);  // random 3-6
    if (line == 0)
        line = 1 + (rand() % 2);    // random 1-2

    // 2. Build OTN frame
    otn_frame_t frame = {0};
    frame.header.client_port = client;
    frame.header.line_port   = line;
    frame.header.frame_id    = stats.next_frame_id++;

    static const char *payloads[] = {
        "Data payload alpha", "Waveserver mini frame",
        "OTN test data", "Traffic payload bravo",
        "Optical signal data"
    };
    snprintf(frame.data, sizeof(frame.data), "%s",
             payloads[rand() % 5]);

    LOG(LOG_DEBUG, "Frame #%u generated: client-%d → line-%d msg=\"%s\"",
        frame.header.frame_id, client, line, frame.data);

    // 3. Query Connection Manager for a route lookup
    udp_message_t req = {0};
    req.msg_type = MSG_LOOKUP_CONNECTION;
    req.status   = STATUS_REQUEST;

    udp_route_lookup_request_t *lookup = (udp_route_lookup_request_t *)req.payload;
    lookup->client_port = client;
    lookup->line_port   = line;

    udp_message_t resp = {0};
    if (!send_udp_message_and_receive(client_socket, &req, &resp, CONN_MANAGER_UDP))
    {
        LOG(LOG_WARN, "Frame #%u DROPPED: failed to reach connection manager",
            frame.header.frame_id);
        stats.total_dropped++;
        send_counter_update(client, 0, 1);
        return;
    }

    // 4. Forward or drop based on connection lookup result
    if (resp.status != STATUS_SUCCESS)
    {
        // No connection exists for this client/line pair
        LOG(LOG_WARN, "Frame #%u DROPPED: no connection for client-%d line-%d",
            frame.header.frame_id, client, line);
        stats.total_dropped++;
        send_counter_update(client, 0, 1);
        return;
    }

    udp_route_lookup_reply_t *route = (udp_route_lookup_reply_t *)resp.payload;

    if (route->operational_state != CONN_UP)
    {
        // Connection exists but is DOWN
        LOG(LOG_WARN, "Frame #%u DROPPED: connection %s is DOWN",
            frame.header.frame_id, route->conn_name);
        stats.total_dropped++;
        send_counter_update(client, 0, 1);
        return;
    }

    // 5. FORWARD — connection is UP
    LOG(LOG_DEBUG, "Frame #%u FORWARDED: client-%d → line-%d via %s",
        frame.header.frame_id, client, line, route->conn_name);
    stats.total_forwarded++;

    // Update port manager counters: client port gets rx+1, line port gets rx+1
    send_counter_update(client, 1, 0);
    send_counter_update(line, 1, 0);
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
    stats.running = false;
    resp->status = STATUS_SUCCESS;
    LOG(LOG_INFO, "Traffic stopped");
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
