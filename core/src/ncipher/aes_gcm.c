#include <nfastapp.h>
#include <seelib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "aes_gcm.h"
#include "log.h"
#include "memzero.h"

extern NFastApp_Connection conn;
extern NFast_AppHandle app;
extern M_CertificateList cert_list;

// For some unknown reason, NFastApp_Transact with -O2 requires heap allocated
// buffers. 1000 bytes should be enough.
uint8_t aes_gcm_buffer[1000];

/**
 * AES-GCM encryption. Used to encrypt the wallet. Also used to encrypt the
 * public key. When this function returns, plaintext is zeroed out.
 *
 * Implementation detail:
 * Ciphertext is [IV (12 bytes), ciphertext (same length as input), TAG (16
 * bytes)].
 */
Result aes_gcm_encrypt(M_KeyID keyId, uint8_t *plaintext, size_t plaintext_len,
                       uint8_t *ciphertext, size_t ciphertext_len,
                       size_t *bytes_written) {
  memzero(ciphertext, ciphertext_len);

  size_t expected_ciphertext_len = plaintext_len + 12 + 16;
  if (ciphertext_len < expected_ciphertext_len) {
    ERROR("ciphertext buffer too small.");
    memzero(plaintext, plaintext_len);
    return Result_AES_GCM_ENCRYPT_BUFFER_TOO_SMALL_FAILURE;
  }
  if (expected_ciphertext_len > sizeof(aes_gcm_buffer)) {
    ERROR("plaintext too long.");
    memzero(plaintext, plaintext_len);
    return Result_AES_GCM_ENCRYPT_PLAINTEXT_TOO_LONG_FAILURE;
  }

  M_Status retcode;
  M_Command command = {0};
  M_Reply reply = {0};

  command.cmd = Cmd_Encrypt;
  command.args.encrypt.key = keyId;
  command.args.encrypt.mech = Mech_RijndaelmGCM;
  command.args.encrypt.plain.type = PlainTextType_Bytes;
  command.args.encrypt.given_iv = NULL;

  memcpy(aes_gcm_buffer, plaintext, plaintext_len);
  memzero(plaintext, plaintext_len);
  command.args.encrypt.plain.data.bytes.data.len = plaintext_len;
  command.args.encrypt.plain.data.bytes.data.ptr = aes_gcm_buffer;
  command.certs = &cert_list;
  command.flags |= Command_flags_certs_present;
  retcode = NFastApp_Transact(conn, NULL, &command, &reply, NULL);
  memzero(aes_gcm_buffer, sizeof(aes_gcm_buffer));

  if (retcode != Status_OK) {
    ERROR("NFastApp_Transact failed");
    return Result_NFAST_APP_TRANSACT_FAILURE;
  }
  if ((retcode = reply.status) != Status_OK) {
    char buf[1000];
    NFast_StrError(buf, sizeof(buf), reply.status, NULL);
    ERROR("NFastApp_Transact bad status (%s)", buf);
    NFastApp_Free_Reply(app, NULL, NULL, &reply);
    return Result_NFAST_APP_TRANSACT_STATUS_FAILURE;
  }

  if (reply.reply.encrypt.cipher.data.genericgcm128.cipher.len !=
      plaintext_len) {
    NFastApp_Free_Reply(app, NULL, NULL, &reply);
    ERROR("unexpected cipher len");
    return Result_AES_GCM_ENCRYPT_UNEXPECTED_CIPHERTEXT_LEN_FAILURE;
  }

  if (reply.reply.encrypt.cipher.iv.genericgcm128.iv.len != 12) {
    NFastApp_Free_Reply(app, NULL, NULL, &reply);
    ERROR("unexpected IV len");
    return Result_AES_GCM_ENCRYPT_UNEXPECTED_IV_LEN_FAILURE;
  }

  memcpy(ciphertext, reply.reply.encrypt.cipher.iv.genericgcm128.iv.ptr, 12);
  memcpy(ciphertext + 12,
         reply.reply.encrypt.cipher.data.genericgcm128.cipher.ptr,
         plaintext_len);
  memcpy(ciphertext + plaintext_len + 12,
         reply.reply.encrypt.cipher.data.genericgcm128.tag.bytes, 16);
  *bytes_written = expected_ciphertext_len;

  NFastApp_Free_Reply(app, NULL, NULL, &reply);

  return Result_SUCCESS;
}

/**
 * AES-GCM decryption.
 */
Result aes_gcm_decrypt(M_KeyID keyId, const uint8_t *ciphertext,
                       size_t ciphertext_len, uint8_t *plaintext,
                       size_t plaintext_len, size_t *bytes_written) {
  memzero(plaintext, plaintext_len);

  size_t expected_plaintext_len = ciphertext_len - 12 - 16;
  if (plaintext_len < expected_plaintext_len) {
    ERROR("plaintext buffer too small.");
    return Result_AES_GCM_DECRYPT_BUFFER_TOO_SMALL_FAILURE;
  }
  if (ciphertext_len > sizeof(aes_gcm_buffer)) {
    ERROR("ciphertext too long.");
    return Result_AES_GCM_DECRYPT_CIPHERTEXT_TOO_LONG_FAILURE;
  }

  M_Status retcode;
  M_Command command = {0};
  M_Reply reply = {0};

  command.cmd = Cmd_Decrypt;
  command.args.decrypt.key = keyId;
  command.args.decrypt.mech = Mech_RijndaelmGCM;
  command.args.decrypt.reply_type = PlainTextType_Bytes;

  memcpy(aes_gcm_buffer, ciphertext, ciphertext_len);
  command.args.decrypt.cipher.mech = Mech_RijndaelmGCM;
  command.args.decrypt.cipher.data.genericgcm128.cipher.len =
      expected_plaintext_len;
  command.args.decrypt.cipher.data.genericgcm128.cipher.ptr =
      aes_gcm_buffer + 12;
  memcpy(command.args.decrypt.cipher.data.genericgcm128.tag.bytes,
         aes_gcm_buffer + ciphertext_len - 16, 16);
  command.args.decrypt.cipher.iv.genericgcm128.taglen = 16;
  command.args.decrypt.cipher.iv.genericgcm128.iv.len = 12;
  command.args.decrypt.cipher.iv.genericgcm128.iv.ptr = aes_gcm_buffer;
  command.args.decrypt.cipher.iv.genericgcm128.header.len = 0;
  command.certs = &cert_list;
  command.flags |= Command_flags_certs_present;

  if ((retcode = NFastApp_Transact(conn, NULL, &command, &reply, NULL)) !=
      Status_OK) {
    ERROR("NFastApp_Transact failed");
    return Result_NFAST_APP_TRANSACT_FAILURE;
  }
  if ((retcode = reply.status) != Status_OK) {
    char buf[1000];
    NFast_StrError(buf, sizeof(buf), reply.status, NULL);
    ERROR("NFastApp_Transact bad status (%s)", buf);
    NFastApp_Free_Reply(app, NULL, NULL, &reply);
    return Result_NFAST_APP_TRANSACT_STATUS_FAILURE;
  }

  if (reply.reply.decrypt.plain.data.bytes.data.len != expected_plaintext_len) {
    NFastApp_Free_Reply(app, NULL, NULL, &reply);
    ERROR("invalid plain len");
    return Result_AES_GCM_DECRYPT_UNEXPECTED_PLAINTEXT_LEN_FAILURE;
  }

  // success!
  memcpy(plaintext, reply.reply.decrypt.plain.data.bytes.data.ptr,
         expected_plaintext_len);
  *bytes_written = expected_plaintext_len;

  NFastApp_Free_Reply(app, NULL, NULL, &reply);

  return Result_SUCCESS;
}
