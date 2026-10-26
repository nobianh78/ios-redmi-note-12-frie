/**
 * Mock Apple Secure Enclave Processor (SEP) Service
 * Intercepts /dev/sep requests from securityd, coreauthd, and keybagd
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t magic;         /* 'SEPM' 0x5345504d */
    uint32_t command_id;
    uint32_t payload_size;
    uint8_t  payload[256];
} sep_request_packet_t;

typedef struct {
    uint32_t status;        /* 0 = SEP_OK */
    uint32_t response_size;
    uint8_t  response_data[256];
} sep_response_packet_t;

#define SEP_CMD_GET_UID             0x01
#define SEP_CMD_DERIVE_KEYBAG       0x02
#define SEP_CMD_ATTEST_BOOT_STATE   0x03

void sep_mock_handle_request(const sep_request_packet_t *req, sep_response_packet_t *res) {
    printf("[sep_mock] Received SEP Command: 0x%02x\n", req->command_id);

    memset(res, 0, sizeof(sep_response_packet_t));
    res->status = 0; // Success

    switch (req->command_id) {
        case SEP_CMD_GET_UID:
            printf("[sep_mock] Handling SEP_CMD_GET_UID: Providing static simulated hardware UID.\n");
            res->response_size = 32;
            memset(res->response_data, 0x42, 32); // Static test key
            break;

        case SEP_CMD_DERIVE_KEYBAG:
            printf("[sep_mock] Handling SEP_CMD_DERIVE_KEYBAG: Unlocking simulated APFS keybag.\n");
            res->response_size = 64;
            memset(res->response_data, 0xAA, 64);
            break;

        case SEP_CMD_ATTEST_BOOT_STATE:
            printf("[sep_mock] Handling SEP_CMD_ATTEST_BOOT_STATE: Returning valid hardware attestation.\n");
            res->response_size = 16;
            res->response_data[0] = 0x01; // Secure boot OK flag
            break;

        default:
            printf("[sep_mock] Unknown SEP command 0x%02x -> Bypassing check.\n", req->command_id);
            res->status = 0;
            break;
    }
}
