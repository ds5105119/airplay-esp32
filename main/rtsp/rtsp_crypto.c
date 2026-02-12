#include "rtsp_crypto.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_log.h"
#include "sodium.h"

static const char *TAG = "rtsp_crypto";

// Send all data, handling partial sends
static int send_all(int socket, const uint8_t *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    int r = send(socket, data + sent, len - sent, 0);
    if (r <= 0) {
      return -1;
    }
    sent += (size_t)r;
  }
  return 0;
}

int rtsp_crypto_read_block(int socket, rtsp_conn_t *conn, uint8_t *buffer,
                           size_t buffer_size) {
  if (!conn || !conn->hap_session || !conn->encrypted_mode) {
    // Expected during session teardown - not an error
    return -1;
  }

  // Read 2-byte length header (little-endian). Keep partial read state in conn.
  while (conn->crypto_rx.len_received < sizeof(conn->crypto_rx.len_buf)) {
    int r = recv(socket,
                 conn->crypto_rx.len_buf + conn->crypto_rx.len_received,
                 sizeof(conn->crypto_rx.len_buf) - conn->crypto_rx.len_received,
                 0);
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
      }
      return -1;
    }
    if (r == 0) {
      return -1;
    }
    conn->crypto_rx.len_received += (uint8_t)r;
  }

  // Allocate encrypted buffer once we have a full length header
  if (!conn->crypto_rx.encrypted) {
    uint16_t block_len = (uint16_t)conn->crypto_rx.len_buf[0] |
                         ((uint16_t)conn->crypto_rx.len_buf[1] << 8);

    if (block_len == 0 || block_len > RTSP_ENCRYPTED_BLOCK_MAX ||
        block_len > buffer_size) {
      ESP_LOGE(TAG, "Invalid encrypted block length: %d", block_len);
      conn->crypto_rx.len_received = 0;
      conn->crypto_rx.block_len = 0;
      errno = EBADMSG;
      return -1;
    }

    conn->crypto_rx.block_len = block_len;
    conn->crypto_rx.encrypted_len = (size_t)block_len + 16; // +16 tag
    conn->crypto_rx.encrypted_received = 0;
    conn->crypto_rx.encrypted = malloc(conn->crypto_rx.encrypted_len);
    if (!conn->crypto_rx.encrypted) {
      ESP_LOGE(TAG, "Failed to allocate encrypted buffer");
      conn->crypto_rx.len_received = 0;
      conn->crypto_rx.block_len = 0;
      conn->crypto_rx.encrypted_len = 0;
      errno = ENOMEM;
      return -1;
    }
  }

  // Read encrypted payload (+ tag), keeping partial state across timeouts.
  while (conn->crypto_rx.encrypted_received < conn->crypto_rx.encrypted_len) {
    int r = recv(socket,
                 conn->crypto_rx.encrypted + conn->crypto_rx.encrypted_received,
                 conn->crypto_rx.encrypted_len - conn->crypto_rx.encrypted_received,
                 0);
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
      }
      free(conn->crypto_rx.encrypted);
      conn->crypto_rx.encrypted = NULL;
      conn->crypto_rx.len_received = 0;
      conn->crypto_rx.block_len = 0;
      conn->crypto_rx.encrypted_len = 0;
      conn->crypto_rx.encrypted_received = 0;
      return -1;
    }
    if (r == 0) {
      free(conn->crypto_rx.encrypted);
      conn->crypto_rx.encrypted = NULL;
      conn->crypto_rx.len_received = 0;
      conn->crypto_rx.block_len = 0;
      conn->crypto_rx.encrypted_len = 0;
      conn->crypto_rx.encrypted_received = 0;
      errno = ECONNRESET;
      return -1;
    }
    conn->crypto_rx.encrypted_received += (size_t)r;
  }

  // Decrypt using session keys
  uint8_t nonce[12] = {0};
  memcpy(nonce + 4, &conn->hap_session->decrypt_nonce, 8);

  unsigned long long plaintext_len;
  if (crypto_aead_chacha20poly1305_ietf_decrypt(
          buffer, &plaintext_len, NULL, conn->crypto_rx.encrypted,
          conn->crypto_rx.encrypted_len, conn->crypto_rx.len_buf,
          sizeof(conn->crypto_rx.len_buf), nonce,
          conn->hap_session->decrypt_key) != 0) {
    free(conn->crypto_rx.encrypted);
    conn->crypto_rx.encrypted = NULL;
    conn->crypto_rx.len_received = 0;
    conn->crypto_rx.block_len = 0;
    conn->crypto_rx.encrypted_len = 0;
    conn->crypto_rx.encrypted_received = 0;
    ESP_LOGE(TAG, "Failed to decrypt frame");
    errno = EBADMSG;
    return -1;
  }
  free(conn->crypto_rx.encrypted);
  conn->crypto_rx.encrypted = NULL;
  conn->crypto_rx.len_received = 0;
  conn->crypto_rx.block_len = 0;
  conn->crypto_rx.encrypted_len = 0;
  conn->crypto_rx.encrypted_received = 0;

  conn->hap_session->decrypt_nonce++;

  if (plaintext_len > buffer_size) {
    ESP_LOGE(TAG, "Decrypted length too large: %llu", plaintext_len);
    errno = EBADMSG;
    return -1;
  }

  return (int)plaintext_len;
}

int rtsp_crypto_write_frame(int socket, rtsp_conn_t *conn, const uint8_t *data,
                            size_t data_len) {
  if (!conn || !conn->hap_session || !conn->encrypted_mode) {
    // Expected during session teardown - not an error
    return -1;
  }

  size_t offset = 0;
  while (offset < data_len) {
    uint16_t block_len = (data_len - offset) > RTSP_ENCRYPTED_BLOCK_MAX
                             ? RTSP_ENCRYPTED_BLOCK_MAX
                             : (uint16_t)(data_len - offset);

    uint8_t len_buf[2];
    len_buf[0] = block_len & 0xFF;
    len_buf[1] = (block_len >> 8) & 0xFF;

    uint8_t nonce[12] = {0};
    memcpy(nonce + 4, &conn->hap_session->encrypt_nonce, 8);

    size_t encrypted_len = block_len + 16; // +16 for Poly1305 tag
    uint8_t *encrypted = malloc(encrypted_len);
    if (!encrypted) {
      ESP_LOGE(TAG, "Failed to allocate encrypted buffer");
      return -1;
    }

    unsigned long long ct_len;
    crypto_aead_chacha20poly1305_ietf_encrypt(
        encrypted, &ct_len, data + offset, block_len, len_buf, sizeof(len_buf),
        NULL, nonce, conn->hap_session->encrypt_key);

    if (ct_len != encrypted_len) {
      ESP_LOGE(TAG, "Unexpected encrypted length: %llu", ct_len);
      free(encrypted);
      return -1;
    }

    if (send_all(socket, len_buf, sizeof(len_buf)) != 0 ||
        send_all(socket, encrypted, encrypted_len) != 0) {
      ESP_LOGE(TAG, "Failed to send encrypted block");
      free(encrypted);
      return -1;
    }

    free(encrypted);
    conn->hap_session->encrypt_nonce++;
    offset += block_len;
  }

  return 0;
}
