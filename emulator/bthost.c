/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2011-2012  Intel Corporation
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <endian.h>
#include <stdbool.h>

#include "monitor/bt.h"
#include "bthost.h"

#define le16_to_cpu(val) (val)
#define le32_to_cpu(val) (val)
#define cpu_to_le16(val) (val)
#define cpu_to_le32(val) (val)

struct cmd {
	struct cmd *next;
	struct cmd *prev;
	uint8_t data[256 + sizeof(struct bt_hci_cmd_hdr)];
	uint16_t len;
};

struct cmd_queue {
	struct cmd *head;
	struct cmd *tail;
};

struct bthost {
	uint8_t bdaddr[6];
	bthost_send_func send_handler;
	void *send_data;
	struct cmd_queue cmd_q;
	uint8_t ncmd;
	bthost_cmd_complete_cb cmd_complete_cb;
	void *cmd_complete_data;
};

struct bthost *bthost_create(void)
{
	struct bthost *bthost;

	bthost = malloc(sizeof(*bthost));
	if (!bthost)
		return NULL;

	memset(bthost, 0, sizeof(*bthost));

	return bthost;
}

void bthost_destroy(struct bthost *bthost)
{
	struct cmd *cmd;

	if (!bthost)
		return;

	for (cmd = bthost->cmd_q.tail; cmd != NULL; cmd = cmd->next)
		free(cmd);

	free(bthost);
}

void bthost_set_send_handler(struct bthost *bthost, bthost_send_func handler,
							void *user_data)
{
	if (!bthost)
		return;

	bthost->send_handler = handler;
	bthost->send_data = user_data;
}

static void queue_command(struct bthost *bthost, const void *data,
								uint16_t len)
{
	struct cmd_queue *cmd_q = &bthost->cmd_q;
	struct cmd *cmd;

	cmd = malloc(sizeof(*cmd));
	if (!cmd)
		return;

	memset(cmd, 0, sizeof(*cmd));

	memcpy(cmd->data, data, len);
	cmd->len = len;

	if (cmd_q->tail)
		cmd_q->tail->next = cmd;

	cmd->prev = cmd_q->tail;
	cmd_q->tail = cmd;
}

static void send_packet(struct bthost *bthost, const void *data, uint16_t len)
{
	if (!bthost->send_handler)
		return;

	bthost->send_handler(data, len, bthost->send_data);
}

static void send_acl(struct bthost *bthost, uint16_t handle, uint16_t cid,
						const void *data, uint16_t len)
{
	struct bt_hci_acl_hdr *acl_hdr;
	struct bt_l2cap_hdr *l2_hdr;
	uint16_t pkt_len;
	void *pkt_data;

	pkt_len = 1 + sizeof(*acl_hdr) + sizeof(*l2_hdr) + len;

	pkt_data = malloc(pkt_len);
	if (!pkt_data)
		return;

	((uint8_t *) pkt_data)[0] = BT_H4_ACL_PKT;

	acl_hdr = pkt_data + 1;
	acl_hdr->handle = cpu_to_le16(handle);
	acl_hdr->dlen = cpu_to_le16(len + sizeof(*l2_hdr));

	l2_hdr = pkt_data + 1 + sizeof(*acl_hdr);
	l2_hdr->cid = cpu_to_le16(cid);
	l2_hdr->len = cpu_to_le16(len);

	if (len > 0)
		memcpy(pkt_data + 1 + sizeof(*acl_hdr) + sizeof(*l2_hdr),
								data, len);

	send_packet(bthost, pkt_data, pkt_len);

	free(pkt_data);
}

static void send_l2cap_sig(struct bthost *bthost, uint16_t handle, uint8_t code,
				uint8_t ident, const void *data, uint16_t len)
{
	static uint8_t next_ident = 1;
	struct bt_l2cap_hdr_sig *hdr;
	uint16_t pkt_len;
	void *pkt_data;

	pkt_len = sizeof(*hdr) + len;

	pkt_data = malloc(pkt_len);
	if (!pkt_data)
		return;

	if (!ident) {
		ident = next_ident++;
		if (!ident)
			ident = next_ident++;
	}

	hdr = pkt_data;
	hdr->code  = code;
	hdr->ident = ident;
	hdr->len   = cpu_to_le16(len);

	if (len > 0)
		memcpy(pkt_data + sizeof(*hdr), data, len);

	send_acl(bthost, handle, 0x0001, pkt_data, pkt_len);

	free(pkt_data);
}

static void send_command(struct bthost *bthost, uint16_t opcode,
						const void *data, uint8_t len)
{
	struct bt_hci_cmd_hdr *hdr;
	uint16_t pkt_len;
	void *pkt_data;

	pkt_len = 1 + sizeof(*hdr) + len;

	pkt_data = malloc(pkt_len);
	if (!pkt_data)
		return;

	((uint8_t *) pkt_data)[0] = BT_H4_CMD_PKT;

	hdr = pkt_data + 1;
	hdr->opcode = cpu_to_le16(opcode);
	hdr->plen = len;

	if (len > 0)
		memcpy(pkt_data + 1 + sizeof(*hdr), data, len);

	if (bthost->ncmd) {
		send_packet(bthost, pkt_data, pkt_len);
		bthost->ncmd--;
	} else {
		queue_command(bthost, pkt_data, pkt_len);
	}

	free(pkt_data);
}

static void next_cmd(struct bthost *bthost)
{
	struct cmd_queue *cmd_q = &bthost->cmd_q;
	struct cmd *cmd = cmd_q->tail;
	struct cmd *next;

	if (!cmd)
		return;

	next = cmd->next;

	if (!bthost->ncmd)
		return;

	send_packet(bthost, cmd->data, cmd->len);
	bthost->ncmd--;

	if (next)
		next->prev = NULL;

	cmd_q->tail = next;

	free(cmd);
}

static void read_bd_addr_complete(struct bthost *bthost, const void *data,
								uint8_t len)
{
	const struct bt_hci_rsp_read_bd_addr *ev = data;

	if (len < sizeof(*ev))
		return;

	if (ev->status)
		return;

	memcpy(bthost->bdaddr, ev->bdaddr, 6);
}

static void evt_cmd_complete(struct bthost *bthost, const void *data,
								uint8_t len)
{
	const struct bt_hci_evt_cmd_complete *ev = data;
	const void *param;
	uint16_t opcode;

	if (len < sizeof(*ev))
		return;

	param = data + sizeof(*ev);

	bthost->ncmd = ev->ncmd;

	opcode = le16toh(ev->opcode);

	switch (opcode) {
	case BT_HCI_CMD_RESET:
		break;
	case BT_HCI_CMD_READ_BD_ADDR:
		read_bd_addr_complete(bthost, param, len - sizeof(*ev));
		break;
	case BT_HCI_CMD_WRITE_SCAN_ENABLE:
		break;
	default:
		printf("Unhandled cmd_complete opcode 0x%04x\n", opcode);
		break;
	}

	if (bthost->cmd_complete_cb)
		bthost->cmd_complete_cb(opcode, 0, param, len - sizeof(*ev),
						bthost->cmd_complete_data);

	next_cmd(bthost);
}

static void evt_cmd_status(struct bthost *bthost, const void *data,
								uint8_t len)
{
	const struct bt_hci_evt_cmd_status *ev = data;
	uint16_t opcode;

	if (len < sizeof(*ev))
		return;

	bthost->ncmd = ev->ncmd;

	opcode = le16toh(ev->opcode);

	if (ev->status && bthost->cmd_complete_cb)
		bthost->cmd_complete_cb(opcode, ev->status, NULL, 0,
						bthost->cmd_complete_data);

	next_cmd(bthost);
}

static void evt_conn_request(struct bthost *bthost, const void *data,
								uint8_t len)
{
	const struct bt_hci_evt_conn_request *ev = data;
	struct bt_hci_cmd_accept_conn_request cmd;

	if (len < sizeof(*ev))
		return;

	memset(&cmd, 0, sizeof(cmd));
	memcpy(cmd.bdaddr, ev->bdaddr, sizeof(ev->bdaddr));

	send_command(bthost, BT_HCI_CMD_ACCEPT_CONN_REQUEST, &cmd,
								sizeof(cmd));
}

static void evt_conn_complete(struct bthost *bthost, const void *data,
								uint8_t len)
{
	const struct bt_hci_evt_conn_complete *ev = data;

	if (len < sizeof(*ev))
		return;
}

static void process_evt(struct bthost *bthost, const void *data, uint16_t len)
{
	const struct bt_hci_evt_hdr *hdr = data;
	const void *param;

	if (len < sizeof(*hdr))
		return;

	if (sizeof(*hdr) + hdr->plen != len)
		return;

	param = data + sizeof(*hdr);

	switch (hdr->evt) {
	case BT_HCI_EVT_CMD_COMPLETE:
		evt_cmd_complete(bthost, param, hdr->plen);
		break;

	case BT_HCI_EVT_CMD_STATUS:
		evt_cmd_status(bthost, param, hdr->plen);
		break;

	case BT_HCI_EVT_CONN_REQUEST:
		evt_conn_request(bthost, param, hdr->plen);
		break;

	case BT_HCI_EVT_CONN_COMPLETE:
		evt_conn_complete(bthost, param, hdr->plen);
		break;

	default:
		printf("Unsupported event 0x%2.2x\n", hdr->evt);
		break;
	}
}

static bool l2cap_info_req(struct bthost *bthost, uint16_t handle,
				uint8_t ident, const void *data, uint16_t len)
{
	const struct bt_l2cap_pdu_info_req *req = data;
	struct bt_l2cap_pdu_info_rsp rsp;

	if (len < sizeof(*req))
		return false;

	rsp.type = req->type;
	rsp.result = cpu_to_le16(0x0001); /* Not Supported */

	send_l2cap_sig(bthost, handle, BT_L2CAP_PDU_INFO_RSP, ident, &rsp,
								sizeof(rsp));

	return true;
}

static void l2cap_sig(struct bthost *bthost, uint16_t handle, const void *data,
								uint16_t len)
{
	const struct bt_l2cap_hdr_sig *hdr = data;
	struct bt_l2cap_pdu_cmd_reject rej;
	uint16_t hdr_len;

	if (len < sizeof(*hdr))
		goto reject;

	hdr_len = le16_to_cpu(hdr->len);

	if (sizeof(*hdr) + hdr_len != len)
		goto reject;

	switch (hdr->code) {
	case BT_L2CAP_PDU_INFO_REQ:
		if (!l2cap_info_req(bthost, handle, hdr->ident,
						data + sizeof(*hdr), hdr_len))
			goto reject;
		break;
	default:
		goto reject;
	}

	return;

reject:
	memset(&rej, 0, sizeof(rej));
	send_l2cap_sig(bthost, handle, BT_L2CAP_PDU_CMD_REJECT, 0,
							&rej, sizeof(rej));
}

static void process_acl(struct bthost *bthost, const void *data, uint16_t len)
{
	const struct bt_hci_acl_hdr *acl_hdr = data;
	const struct bt_l2cap_hdr *l2_hdr = data + sizeof(*acl_hdr);
	uint16_t handle, cid, acl_len, l2_len;
	const void *l2_data;

	if (len < sizeof(*acl_hdr) + sizeof(*l2_hdr))
		return;

	acl_len = le16_to_cpu(acl_hdr->dlen);
	if (len != sizeof(*acl_hdr) + acl_len)
		return;

	handle = le16_to_cpu(acl_hdr->handle);

	l2_len = le16_to_cpu(l2_hdr->len);
	if (len - sizeof(*acl_hdr) != sizeof(*l2_hdr) + l2_len)
		return;

	l2_data = data + sizeof(*acl_hdr) + sizeof(*l2_hdr);

	cid = le16_to_cpu(l2_hdr->cid);

	switch (cid) {
	case 0x0001:
		l2cap_sig(bthost, handle, l2_data, l2_len);
		break;
	default:
		printf("Packet for unknown CID 0x%04x (%u)\n", cid, cid);
		break;
	}
}

void bthost_receive_h4(struct bthost *bthost, const void *data, uint16_t len)
{
	uint8_t pkt_type;

	if (!bthost)
		return;

	if (len < 1)
		return;

	pkt_type = ((const uint8_t *) data)[0];

	switch (pkt_type) {
	case BT_H4_EVT_PKT:
		process_evt(bthost, data + 1, len - 1);
		break;
	case BT_H4_ACL_PKT:
		process_acl(bthost, data + 1, len - 1);
		break;
	default:
		printf("Unsupported packet 0x%2.2x\n", pkt_type);
		break;
	}
}

void bthost_set_cmd_complete_cb(struct bthost *bthost,
				bthost_cmd_complete_cb cb, void *user_data)
{
	bthost->cmd_complete_cb = cb;
	bthost->cmd_complete_data = user_data;
}

void bthost_write_scan_enable(struct bthost *bthost, uint8_t scan)
{
	send_command(bthost, BT_HCI_CMD_WRITE_SCAN_ENABLE, &scan, 1);
}

void bthost_start(struct bthost *bthost)
{
	if (!bthost)
		return;

	bthost->ncmd = 1;

	send_command(bthost, BT_HCI_CMD_RESET, NULL, 0);

	send_command(bthost, BT_HCI_CMD_READ_BD_ADDR, NULL, 0);
}

void bthost_stop(struct bthost *bthost)
{
}
