/**
 * protection_manager.c — Waveserver Mini 1+1 Line Protection Service
 *
 * UDP Port: 5004  (PROTECTION_MGR_UDP, defined in common.h)
 *
 * Implements revertive 1+1 line protection for the port-1 / port-2 pair.
 *
 * Fault detection
 * ───────────────
 * Port Manager broadcasts MSG_PORT_STATE_CHANGE to Connection Manager
 * AND to this service. One extra send_udp_message_one_way() call is
 * added to notify_port_state() in port_manager.c (see patch notes).
 *
 * Switchover mechanism
 * ────────────────────
 * Sends MSG_SWITCH_CONN_LINE to Connection Manager which updates
 * conn_t.line_port in-place. Traffic Manager's next route lookup then
 * sees the new line port automatically — no changes to traffic_manager.c.
 */
 
#include "common.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>
 
#define SERVICE_NAME "prot_mgr"
 
/* ── Protection group state ──────────────────────────────────────────── */
 
typedef struct {
    char    conn_name[MAX_CONN_NAME_CHARACTER];
    uint8_t client_port;
    uint8_t original_line;
    uint8_t current_line;
    bool    switched;
} prot_entry_t;
 
typedef struct {
    bool         active;
    uint32_t     switchover_count;
    prot_entry_t entries[MAX_CONNS];
    uint8_t      entry_count;
} prot_group_t;
 
static prot_group_t pg;
static int          client_sock;
 
/* ── Internal helpers ────────────────────────────────────────────────── */
 
static bool refresh_entries(void)
{
    udp_message_t req = {0};
    req.msg_type = MSG_GET_CONNECTIONS;
    req.status   = STATUS_REQUEST;
 
    udp_message_t resp = {0};
    if (!send_udp_message_and_receive(client_sock, &req, &resp, CONN_MANAGER_UDP))
    {
        LOG(LOG_ERROR, "refresh_entries: failed to query conn_mgr");
        return false;
    }
 
    const udp_get_connections_reply_t *r =
        (const udp_get_connections_reply_t *)resp.payload;
 
    pg.entry_count = 0;
    for (int i = 0; i < r->conn_count && pg.entry_count < MAX_CONNS; i++)
    {
        const conn_t *c = &r->all_connections[i];
        if (c->line_port != 1 && c->line_port != 2)
            continue;
 
        prot_entry_t *e = &pg.entries[pg.entry_count++];
        strncpy(e->conn_name, c->conn_name, MAX_CONN_NAME_CHARACTER - 1);
        e->conn_name[MAX_CONN_NAME_CHARACTER - 1] = '\0';
        e->client_port   = c->client_port;
        e->original_line = c->line_port;
        e->current_line  = c->line_port;
        e->switched      = false;
    }
 
    LOG(LOG_DEBUG, "refresh_entries: tracking %d connection(s)", pg.entry_count);
    return true;
}
 
static bool switch_conn_line(const char *conn_name, uint8_t new_line)
{
    udp_message_t req = {0};
    req.msg_type = MSG_SWITCH_CONN_LINE;
    req.status   = STATUS_REQUEST;
 
    udp_switch_conn_line_request_t *payload =
        (udp_switch_conn_line_request_t *)req.payload;
    strncpy(payload->conn_name, conn_name, MAX_CONN_NAME_CHARACTER - 1);
    payload->new_line_port = new_line;
 
    udp_message_t resp = {0};
    if (!send_udp_message_and_receive(client_sock, &req, &resp, CONN_MANAGER_UDP))
    {
        LOG(LOG_ERROR, "switch_conn_line: comms failure for '%s'", conn_name);
        return false;
    }
 
    if (resp.status != STATUS_SUCCESS)
    {
        LOG(LOG_ERROR, "switch_conn_line: conn_mgr rejected switch for '%s'", conn_name);
        return false;
    }
 
    return true;
}
 
/* ── Message handlers ────────────────────────────────────────────────── */
 
static void handle_port_state_change(const udp_message_t *req)
{
    if (!pg.active)
        return;
 
    const udp_port_state_change_t *p =
        (const udp_port_state_change_t *)req->payload;
 
    if (p->port_id != 1 && p->port_id != 2)
        return;
 
    uint8_t changed_port    = p->port_id;
    uint8_t protection_port = (changed_port == 1) ? 2 : 1;
 
    if (p->operational_state == PORT_DOWN)
    {
        LOG(LOG_INFO, "Port-%d went DOWN -- triggering protection switchover",
            changed_port);
 
        for (int i = 0; i < pg.entry_count; i++)
        {
            prot_entry_t *e = &pg.entries[i];
            if (e->current_line != changed_port)
                continue;
 
            if (switch_conn_line(e->conn_name, protection_port))
            {
                LOG(LOG_INFO,
                    "Protection switchover: %s moved from port-%d -> port-%d",
                    e->conn_name, changed_port, protection_port);
                e->current_line = protection_port;
                e->switched     = (protection_port != e->original_line);
                pg.switchover_count++;
            }
        }
    }
    else /* PORT_UP */
    {
        LOG(LOG_INFO, "Port-%d came back UP -- triggering revertive switch",
            changed_port);
 
        for (int i = 0; i < pg.entry_count; i++)
        {
            prot_entry_t *e = &pg.entries[i];
            if (e->original_line != changed_port || !e->switched)
                continue;
 
            if (switch_conn_line(e->conn_name, changed_port))
            {
                LOG(LOG_INFO,
                    "Revertive switch: %s moved from port-%d -> port-%d",
                    e->conn_name, e->current_line, changed_port);
                e->current_line = changed_port;
                e->switched     = false;
            }
        }
    }
}
 
static void handle_set_protection(udp_message_t *resp)
{
    if (pg.active)
    {
        set_error_msg(resp, "Protection group already exists");
        return;
    }
 
    /* Both line ports must be admin-enabled */
    for (uint8_t port_id = 1; port_id <= 2; port_id++)
    {
        udp_message_t req = {0};
        req.msg_type = MSG_GET_PORT_INFO;
        req.status   = STATUS_REQUEST;
        ((udp_port_cmd_request_t *)req.payload)->port_id = port_id;
 
        udp_message_t presp = {0};
        if (!send_udp_message_and_receive(client_sock, &req, &presp, PORT_MANAGER_UDP) ||
            presp.status != STATUS_SUCCESS)
        {
            set_error_msg(resp, "Could not query Port Manager");
            return;
        }
 
        const port_t *port = (const port_t *)presp.payload;
        if (!port->admin_enabled)
        {
            char msg[64];
            snprintf(msg, sizeof(msg),
                     "Port-%d must be admin-enabled before creating protection group",
                     port_id);
            set_error_msg(resp, msg);
            return;
        }
    }
 
    pg.active           = true;
    pg.switchover_count = 0;
 
    if (!refresh_entries())
    {
        pg.active = false;
        set_error_msg(resp, "Failed to read connection table from conn_mgr");
        return;
    }
 
    LOG(LOG_INFO, "Protection group created: port-1 <-> port-2 (%d connection(s) tracked)",
        pg.entry_count);
    resp->status = STATUS_SUCCESS;
}
 
static void handle_delete_protection(udp_message_t *resp)
{
    if (!pg.active)
    {
        set_error_msg(resp, "No protection group is active");
        return;
    }
 
    /* Revert any switched connections back to their original line port */
    for (int i = 0; i < pg.entry_count; i++)
    {
        prot_entry_t *e = &pg.entries[i];
        if (!e->switched)
            continue;
 
        if (switch_conn_line(e->conn_name, e->original_line))
        {
            LOG(LOG_INFO, "delete protection: reverted %s to port-%d",
                e->conn_name, e->original_line);
            e->current_line = e->original_line;
            e->switched     = false;
        }
    }
 
    pg.active      = false;
    pg.entry_count = 0;
    LOG(LOG_INFO, "Protection group deleted");
    resp->status = STATUS_SUCCESS;
}
 
static void handle_get_protection(udp_message_t *resp)
{
    udp_get_protection_reply_t *r =
        (udp_get_protection_reply_t *)resp->payload;
 
    r->active           = pg.active;
    r->switchover_count = pg.switchover_count;
    r->entry_count      = (uint8_t)pg.entry_count;
 
    for (int i = 0; i < pg.entry_count; i++)
    {
        r->entries[i].client_port   = pg.entries[i].client_port;
        r->entries[i].original_line = pg.entries[i].original_line;
        r->entries[i].current_line  = pg.entries[i].current_line;
        r->entries[i].switched      = pg.entries[i].switched;
        strncpy(r->entries[i].conn_name, pg.entries[i].conn_name,
                MAX_CONN_NAME_CHARACTER - 1);
        r->entries[i].conn_name[MAX_CONN_NAME_CHARACTER - 1] = '\0';
    }
 
    resp->status = STATUS_SUCCESS;
}
 
/* ── Dispatch ────────────────────────────────────────────────────────── */
 
static bool dispatch(const udp_message_t *req, udp_message_t *resp)
{
    bool send_reply = true;
    resp->msg_type  = req->msg_type;
    resp->status    = STATUS_FAILURE;
 
    switch ((msg_type_t)req->msg_type)
    {
    case MSG_PORT_STATE_CHANGE:
        handle_port_state_change(req);
        send_reply = false;
        break;
    case MSG_SET_PROTECTION:
        handle_set_protection(resp);
        break;
    case MSG_DELETE_PROTECTION:
        handle_delete_protection(resp);
        break;
    case MSG_GET_PROTECTION:
        handle_get_protection(resp);
        break;
    default:
        LOG(LOG_WARN, "Unknown msg_type: %d", req->msg_type);
        send_reply = false;
        break;
    }
 
    return send_reply;
}
 
/* ── main ────────────────────────────────────────────────────────────── */
 
int main(void)
{
    log_init(SERVICE_NAME);
    memset(&pg, 0, sizeof(pg));
    LOG(LOG_INFO, "Protection Manager starting on UDP port %d", PROTECTION_MGR_UDP);
 
    int server_sock = create_udp_server(PROTECTION_MGR_UDP);
    if (server_sock < 0)
    {
        LOG(LOG_ERROR, "Failed to create server socket -- exiting");
        return 1;
    }
 
    client_sock = create_udp_client();
    if (client_sock < 0)
    {
        LOG(LOG_ERROR, "Failed to create client socket -- exiting");
        return 1;
    }
 
    while (true)
    {
        udp_message_t req = {0};
        struct sockaddr_in sender;
        socklen_t sender_len = sizeof(sender);
 
        ssize_t n = recvfrom(server_sock, &req, sizeof(req), 0,
                             (struct sockaddr *)&sender, &sender_len);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG(LOG_ERROR, "recvfrom failed");
        }
        else if (n > 0)
        {
            udp_message_t resp = {0};
            if (dispatch(&req, &resp) &&
                sendto(server_sock, &resp, sizeof(resp), 0,
                       (struct sockaddr *)&sender, sender_len) < 0)
            {
                LOG(LOG_ERROR, "sendto reply failed");
            }
        }
    }
 
    return 0;
}
 