/*

Copyright (c) 2016 SharkRF OÜ. https://www.sharkrf.com/
Author: Norbert "Nonoo" Varga, HA2NON

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

*/

#include "packet.h"
#include "server-sock.h"
#include "server-client.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>

static struct {
	uint32_t client_id;
	uint8_t given_token[8];
	time_t last_auth_fail_at;
} packet_new_connection_data = { .last_auth_fail_at = 0 };

static void packet_process_login(void) {
	srf_ip_conn_packet_t *packet = (srf_ip_conn_packet_t *)server_sock_received_packet.buf;
	srf_ip_conn_packet_t answer_packet;
	uint8_t i;

	if (server_sock_received_packet.received_bytes != sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_login_payload_t)) {
		printf("  packet is %zd bytes, not %lu, ignoring\n", server_sock_received_packet.received_bytes, sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_login_payload_t));
		return;
	}

	srf_ip_conn_packet_init(&answer_packet.header, SRF_IP_CONN_PACKET_TYPE_TOKEN);
	packet_new_connection_data.client_id = ntohl(packet->login.client_id);
	printf("  got login packet from id %u, answering with token ", packet_new_connection_data.client_id);
	for (i = 0; i < sizeof(packet_new_connection_data.given_token); i++) {
		packet_new_connection_data.given_token[i] = answer_packet.token.token[i] = rand();
		printf("%.2x", answer_packet.token.token[i]);
	}
	printf("\n");

	server_sock_send((uint8_t *)&answer_packet, sizeof(srf_ip_conn_packet_header_t) + sizeof(srf_ip_conn_token_payload_t), &server_sock_received_packet.from_addr);
}

static void packet_process_auth(void) {
	srf_ip_conn_packet_t *packet = (srf_ip_conn_packet_t *)server_sock_received_packet.buf;
	srf_ip_conn_packet_t answer_packet;
	uint8_t i;

	// Limiting auth retries for only one per 5 seconds.
	if (time(NULL) - packet_new_connection_data.last_auth_fail_at < 5) {
		printf("  got auth packet, but retry timeout hasn't been expired\n");
		return;
	}

	if (server_sock_received_packet.received_bytes != sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_auth_payload_t)) {
		printf("  packet is %zd bytes, not %lu, ignoring\n", server_sock_received_packet.received_bytes, sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_auth_payload_t));
		return;
	}

	printf("  got auth packet, hmac: ");
	for (i = 0; i < 32; i++)
		printf("%.2x", packet->auth.hmac[i]);
	printf("\n");

	// If hash is not matching the HMAC we received in the auth packet, we send a nak.
	if (!srf_ip_conn_packet_hmac_check(packet_new_connection_data.given_token, CONFIG_PASSWORD, packet, sizeof(srf_ip_conn_auth_payload_t))) {
		time(&packet_new_connection_data.last_auth_fail_at);

		printf("    hmac mismatch, sending nak\n");
		srf_ip_conn_packet_init(&answer_packet.header, SRF_IP_CONN_PACKET_TYPE_NAK);
		for (i = 0; i < sizeof(answer_packet.nak.random_data); i++)
			answer_packet.nak.random_data[i] = rand();
		answer_packet.nak.result = SRF_IP_CONN_NAK_RESULT_AUTH_INVALID_HMAC;
		srf_ip_conn_packet_hmac_add(packet_new_connection_data.given_token, CONFIG_PASSWORD, &answer_packet, sizeof(srf_ip_conn_nak_payload_t));
		server_sock_send((uint8_t *)&answer_packet, sizeof(srf_ip_conn_packet_header_t) + sizeof(srf_ip_conn_nak_payload_t), &server_sock_received_packet.from_addr);
		return;
	}

	// If user tries to log in with an invalid client id, we reject it with a nak.
	if (packet_new_connection_data.client_id == 12345) {
		printf("    invalid client id, sending nak\n");
		srf_ip_conn_packet_init(&answer_packet.header, SRF_IP_CONN_PACKET_TYPE_NAK);
		for (i = 0; i < sizeof(answer_packet.nak.random_data); i++)
			answer_packet.nak.random_data[i] = rand();
		answer_packet.nak.result = SRF_IP_CONN_NAK_RESULT_AUTH_INVALID_CLIENT_ID;
		srf_ip_conn_packet_hmac_add(packet_new_connection_data.given_token, CONFIG_PASSWORD, &answer_packet, sizeof(srf_ip_conn_nak_payload_t));
		server_sock_send((uint8_t *)&answer_packet, sizeof(srf_ip_conn_packet_header_t) + sizeof(srf_ip_conn_nak_payload_t), &server_sock_received_packet.from_addr);
		return;
	}

	// Client is now logged in.
	server_client_login(packet_new_connection_data.client_id, packet_new_connection_data.given_token, &server_sock_received_packet.from_addr);

	srf_ip_conn_packet_init(&answer_packet.header, SRF_IP_CONN_PACKET_TYPE_ACK);
	for (i = 0; i < sizeof(answer_packet.ack.random_data); i++)
		answer_packet.ack.random_data[i] = rand();
	answer_packet.ack.result = SRF_IP_CONN_ACK_RESULT_AUTH;
	srf_ip_conn_packet_hmac_add(packet_new_connection_data.given_token, CONFIG_PASSWORD, &answer_packet, sizeof(srf_ip_conn_ack_payload_t));
	server_sock_send((uint8_t *)&answer_packet, sizeof(srf_ip_conn_packet_header_t) + sizeof(srf_ip_conn_ack_payload_t), &server_sock_received_packet.from_addr);
}

static flag_t packet_process_config(void) {
	srf_ip_conn_packet_t *packet = (srf_ip_conn_packet_t *)server_sock_received_packet.buf;
	srf_ip_conn_packet_t answer_packet;
	uint8_t i;

	if (server_sock_received_packet.received_bytes != sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_config_payload_t)) {
		printf("  packet is %zd bytes, not %lu, ignoring\n", server_sock_received_packet.received_bytes, sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_config_payload_t));
		return 0;
	}
	if (!server_client_is_logged_in(&server_sock_received_packet.from_addr)) {
		printf("  client isn't logged in, ignoring packet\n");
		return 0;
	}
	if (!srf_ip_conn_packet_hmac_check(server_client.token, CONFIG_PASSWORD, packet, sizeof(srf_ip_conn_config_payload_t))) {
		printf("  invalid hmac, ignoring packet\n");
		return 0;
	}

	printf("  got valid config packet\n");
	server_client_config(&packet->config);

	srf_ip_conn_packet_init(&answer_packet.header, SRF_IP_CONN_PACKET_TYPE_ACK);
	answer_packet.ack.result = SRF_IP_CONN_ACK_RESULT_CONFIG;
	for (i = 0; i < sizeof(answer_packet.ack.random_data); i++)
		answer_packet.ack.random_data[i] = rand();
	srf_ip_conn_packet_hmac_add(server_client.token, CONFIG_PASSWORD, &answer_packet, sizeof(srf_ip_conn_ack_payload_t));
	server_sock_send((uint8_t *)&answer_packet, sizeof(srf_ip_conn_packet_header_t) + sizeof(srf_ip_conn_ack_payload_t), &server_sock_received_packet.from_addr);
	return 1;
}

static flag_t packet_process_ping(void) {
	srf_ip_conn_packet_t *packet = (srf_ip_conn_packet_t *)server_sock_received_packet.buf;
	srf_ip_conn_packet_t answer_packet;
	uint8_t i;

	if (server_sock_received_packet.received_bytes != sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_ping_payload_t)) {
		printf("  packet is %zd bytes, not %lu, ignoring\n", server_sock_received_packet.received_bytes, sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_ping_payload_t));
		return 0;
	}
	if (!server_client_is_logged_in(&server_sock_received_packet.from_addr)) {
		printf("  client isn't logged in, ignoring packet\n");
		return 0;
	}
	if (!srf_ip_conn_packet_hmac_check(server_client.token, CONFIG_PASSWORD, packet, sizeof(srf_ip_conn_ping_payload_t))) {
		printf("  invalid hmac, ignoring packet\n");
		return 0;
	}

	printf("  got ping, sending pong\n");
	srf_ip_conn_packet_init(&answer_packet.header, SRF_IP_CONN_PACKET_TYPE_PONG);
	for (i = 0; i < sizeof(answer_packet.pong.random_data); i++)
		answer_packet.pong.random_data[i] = rand();
	srf_ip_conn_packet_hmac_add(server_client.token, CONFIG_PASSWORD, &answer_packet, sizeof(srf_ip_conn_pong_payload_t));
	server_sock_send((uint8_t *)&answer_packet, sizeof(srf_ip_conn_packet_header_t) + sizeof(srf_ip_conn_pong_payload_t), &server_sock_received_packet.from_addr);
	return 1;
}

static void packet_process_close(void) {
	srf_ip_conn_packet_t *packet = (srf_ip_conn_packet_t *)server_sock_received_packet.buf;
	srf_ip_conn_packet_t answer_packet;
	uint8_t i;

	if (server_sock_received_packet.received_bytes != sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_close_payload_t)) {
		printf("  packet is %zd bytes, not %lu, ignoring\n", server_sock_received_packet.received_bytes, sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_close_payload_t));
		return;
	}
	if (!server_client_is_logged_in(&server_sock_received_packet.from_addr)) {
		printf("  client isn't logged in, ignoring packet\n");
		return;
	}
	if (!srf_ip_conn_packet_hmac_check(server_client.token, CONFIG_PASSWORD, packet, sizeof(srf_ip_conn_close_payload_t))) {
		printf("  invalid hmac, ignoring packet\n");
		return;
	}

	printf("  got valid close packet\n");
	server_client_logout();

	srf_ip_conn_packet_init(&answer_packet.header, SRF_IP_CONN_PACKET_TYPE_ACK);
	answer_packet.ack.result = SRF_IP_CONN_ACK_RESULT_CLOSE;
	for (i = 0; i < sizeof(answer_packet.ack.random_data); i++)
		answer_packet.ack.random_data[i] = rand();
	srf_ip_conn_packet_hmac_add(server_client.token, CONFIG_PASSWORD, &answer_packet, sizeof(srf_ip_conn_ack_payload_t));
	server_sock_send((uint8_t *)&answer_packet, sizeof(srf_ip_conn_packet_header_t) + sizeof(srf_ip_conn_ack_payload_t), &server_sock_received_packet.from_addr);
}

static flag_t packet_process_raw(void) {
	srf_ip_conn_packet_t *packet = (srf_ip_conn_packet_t *)server_sock_received_packet.buf;

	if (server_sock_received_packet.received_bytes != sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_data_raw_payload_t)) {
		printf("  packet is %zd bytes, not %lu, ignoring\n", server_sock_received_packet.received_bytes, sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_data_raw_payload_t));
		return 0;
	}
	if (!server_client_is_logged_in(&server_sock_received_packet.from_addr)) {
		printf("  client isn't logged in, ignoring packet\n");
		return 0;
	}
	if (!srf_ip_conn_packet_hmac_check(server_client.token, CONFIG_PASSWORD, packet, sizeof(srf_ip_conn_data_raw_payload_t))) {
		printf("  invalid hmac, ignoring packet\n");
		return 0;
	}

	printf("  got valid raw data\n");
	srf_ip_conn_packet_print_data_raw_payload(&packet->data_raw);

	return 1;
}

static flag_t packet_process_dmr(void) {
	srf_ip_conn_packet_t *packet = (srf_ip_conn_packet_t *)server_sock_received_packet.buf;

	if (server_sock_received_packet.received_bytes != sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_data_dmr_payload_t)) {
		printf("  packet is %zd bytes, not %lu, ignoring\n", server_sock_received_packet.received_bytes, sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_data_dmr_payload_t));
		return 0;
	}
	if (!server_client_is_logged_in(&server_sock_received_packet.from_addr)) {
		printf("  client isn't logged in, ignoring packet\n");
		return 0;
	}
	if (!srf_ip_conn_packet_hmac_check(server_client.token, CONFIG_PASSWORD, packet, sizeof(srf_ip_conn_data_dmr_payload_t))) {
		printf("  invalid hmac, ignoring packet\n");
		return 0;
	}

	printf("  got valid dmr data\n");
	srf_ip_conn_packet_print_data_dmr_payload(&packet->data_dmr);

	return 1;
}

static flag_t packet_process_dstar(void) {
	srf_ip_conn_packet_t *packet = (srf_ip_conn_packet_t *)server_sock_received_packet.buf;

	if (server_sock_received_packet.received_bytes != sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_data_dstar_payload_t)) {
		printf("  packet is %zd bytes, not %lu, ignoring\n", server_sock_received_packet.received_bytes, sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_data_dstar_payload_t));
		return 0;
	}
	if (!server_client_is_logged_in(&server_sock_received_packet.from_addr)) {
		printf("  client isn't logged in, ignoring packet\n");
		return 0;
	}
	if (!srf_ip_conn_packet_hmac_check(server_client.token, CONFIG_PASSWORD, packet, sizeof(srf_ip_conn_data_dstar_payload_t))) {
		printf("  invalid hmac, ignoring packet\n");
		return 0;
	}

	printf("  got valid dstar data\n");
	srf_ip_conn_packet_print_data_dstar_payload(&packet->data_dstar);

	return 1;
}

static flag_t packet_process_c4fm(void) {
	srf_ip_conn_packet_t *packet = (srf_ip_conn_packet_t *)server_sock_received_packet.buf;

	if (server_sock_received_packet.received_bytes != sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_data_c4fm_payload_t)) {
		printf("  packet is %zd bytes, not %lu, ignoring\n", server_sock_received_packet.received_bytes, sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_data_c4fm_payload_t));
		return 0;
	}
	if (!server_client_is_logged_in(&server_sock_received_packet.from_addr)) {
		printf("  client isn't logged in, ignoring packet\n");
		return 0;
	}
	if (!srf_ip_conn_packet_hmac_check(server_client.token, CONFIG_PASSWORD, packet, sizeof(srf_ip_conn_data_c4fm_payload_t))) {
		printf("  invalid hmac, ignoring packet\n");
		return 0;
	}

	printf("  got valid c4fm data\n");
	srf_ip_conn_packet_print_data_c4fm_payload(&packet->data_c4fm);

	return 1;
}

static flag_t packet_process_nxdn(void) {
	srf_ip_conn_packet_t *packet = (srf_ip_conn_packet_t *)server_sock_received_packet.buf;

	if (server_sock_received_packet.received_bytes != sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_data_nxdn_payload_t)) {
		printf("  packet is %zd bytes, not %lu, ignoring\n", server_sock_received_packet.received_bytes, sizeof(srf_ip_conn_packet_header_t)+sizeof(srf_ip_conn_data_nxdn_payload_t));
		return 0;
	}
	if (!server_client_is_logged_in(&server_sock_received_packet.from_addr)) {
		printf("  client isn't logged in, ignoring packet\n");
		return 0;
	}
	if (!srf_ip_conn_packet_hmac_check(server_client.token, CONFIG_PASSWORD, packet, sizeof(srf_ip_conn_data_nxdn_payload_t))) {
		printf("  invalid hmac, ignoring packet\n");
		return 0;
	}

	printf("  got valid nxdn data\n");
	srf_ip_conn_packet_print_data_nxdn_payload(&packet->data_nxdn);

	return 1;
}

void packet_process(void) {
	srf_ip_conn_packet_header_t *header = (srf_ip_conn_packet_header_t *)server_sock_received_packet.buf;

	switch (header->version) {
		case 0:
			switch (header->packet_type) {
				case SRF_IP_CONN_PACKET_TYPE_LOGIN:
					packet_process_login();
					break;
				case SRF_IP_CONN_PACKET_TYPE_AUTH:
					packet_process_auth();
					break;
				case SRF_IP_CONN_PACKET_TYPE_CONFIG:
					if (packet_process_config())
						server_client_got_valid_packet();
					break;
				case SRF_IP_CONN_PACKET_TYPE_PING:
					if (packet_process_ping())
						server_client_got_valid_packet();
					break;
				case SRF_IP_CONN_PACKET_TYPE_CLOSE:
					packet_process_close();
					break;
				case SRF_IP_CONN_PACKET_TYPE_DATA_RAW:
					if (packet_process_raw())
						server_client_got_valid_packet();
					break;
				case SRF_IP_CONN_PACKET_TYPE_DATA_DMR:
					if (packet_process_dmr())
						server_client_got_valid_packet();
					break;
				case SRF_IP_CONN_PACKET_TYPE_DATA_DSTAR:
					if (packet_process_dstar())
						server_client_got_valid_packet();
					break;
				case SRF_IP_CONN_PACKET_TYPE_DATA_C4FM:
					if (packet_process_c4fm())
						server_client_got_valid_packet();
					break;
				case SRF_IP_CONN_PACKET_TYPE_DATA_NXDN:
					if (packet_process_nxdn())
						server_client_got_valid_packet();
					break;
			}
			break;
	}
}
