/*
 * email_notifier.c — see email_notifier.h
 */

#define _POSIX_C_SOURCE 200809L /* for mkstemp, fdopen */

#include "email_notifier.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * Copies frame_path into a private temp file in one fast local read+
 * write, and returns the temp path (caller must unlink() it when done)
 * via out_path, or returns -1 on failure.
 *
 * Why this exists: curl_mime_filedata() does NOT read the file upfront —
 * it streams it lazily from disk while curl_easy_perform() runs, which
 * for SMTP can take several seconds (TLS handshake, auth, DATA
 * transfer). If person_detector.py rewrites frame.jpg (truncate +
 * rewrite, not atomic) at any point during that window, curl can end up
 * reading a mix of leftover bytes from the old file and new bytes from
 * the file that replaced it — which looks exactly like "two images
 * overlapping" once decoded, since it genuinely is fragments of two
 * different JPEGs concatenated. A single fread() here takes a few
 * milliseconds at most, shrinking that race window by orders of
 * magnitude compared to the full SMTP send — not a hard guarantee (the
 * ideal fix is Step 2 writing frame.jpg atomically via write-to-temp +
 * rename, which this project doesn't control), but enough to make the
 * corruption effectively unreproducible in practice.
 */
static int snapshot_frame_to_tempfile(const char *frame_path, char *out_path, size_t out_path_size) {
    FILE *src = fopen(frame_path, "rb");
    if (!src) return -1;

    fseek(src, 0, SEEK_END);
    long size = ftell(src);
    fseek(src, 0, SEEK_SET);
    if (size <= 0) {
        fclose(src);
        return -1;
    }

    unsigned char *buf = malloc((size_t)size);
    if (!buf) {
        fclose(src);
        return -1;
    }

    size_t read_bytes = fread(buf, 1, (size_t)size, src);
    fclose(src);
    if (read_bytes != (size_t)size) {
        free(buf);
        return -1;
    }

    snprintf(out_path, out_path_size, "/tmp/surveillance_email_XXXXXX");
    int fd = mkstemp(out_path);
    if (fd < 0) {
        free(buf);
        return -1;
    }

    FILE *dst = fdopen(fd, "wb");
    if (!dst) {
        close(fd);
        free(buf);
        unlink(out_path);
        return -1;
    }

    size_t written = fwrite(buf, 1, (size_t)size, dst);
    fclose(dst);
    free(buf);

    if (written != (size_t)size) {
        unlink(out_path);
        return -1;
    }

    return 0;
}

int email_notifier_send(const notifier_config_t *cfg, int count, const char *timestamp,
                         double cpu_temp_c) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "[email] curl_easy_init failed\n");
        return -1;
    }

    int result = -1;
    struct curl_slist *recipients = NULL;
    struct curl_slist *headers = NULL;
    curl_mime *mime = NULL;

    curl_easy_setopt(curl, CURLOPT_URL, cfg->smtp_url);
    curl_easy_setopt(curl, CURLOPT_USE_SSL, cfg->smtp_use_starttls ? CURLUSESSL_ALL : CURLUSESSL_TRY);
    curl_easy_setopt(curl, CURLOPT_USERNAME, cfg->smtp_username);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg->smtp_password);
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, cfg->smtp_from);

    recipients = curl_slist_append(recipients, cfg->smtp_to);
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);

    /* --- RFC822 message headers ---
     * libcurl's SMTP support (confusingly) reuses CURLOPT_HTTPHEADER for
     * the actual mail headers (From/To/Subject/Date), separate from the
     * MIME body built below — these are NOT HTTP headers despite the
     * option name. This is libcurl's documented pattern for SMTP+MIME
     * (see curl's own smtp-mime.c example), not a HTTP/SMTP mixup on our
     * part. */
    char subject_header[256];
    snprintf(subject_header, sizeof(subject_header),
             "Subject: Surveillance alert - %d person(s) detected", count);
    char from_header[CFG_MAXLEN + 8];
    snprintf(from_header, sizeof(from_header), "From: %s", cfg->smtp_from);
    char to_header[CFG_MAXLEN + 8];
    snprintf(to_header, sizeof(to_header), "To: %s", cfg->smtp_to);

    headers = curl_slist_append(headers, from_header);
    headers = curl_slist_append(headers, to_header);
    headers = curl_slist_append(headers, subject_header);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    /* --- Body: plain text summary + JPEG attachment --- */
    mime = curl_mime_init(curl);

    curl_mimepart *body_part = curl_mime_addpart(mime);
    char body_text[512];
    snprintf(body_text, sizeof(body_text),
        "Detection alert\r\n"
        "----------------\r\n"
        "Person count: %d\r\n"
        "Timestamp:    %s\r\n"
        "CPU temp:     %.1f C\r\n",
        count, timestamp, cpu_temp_c);
    curl_mime_data(body_part, body_text, CURL_ZERO_TERMINATED);
    curl_mime_type(body_part, "text/plain");

    curl_mimepart *attachment_part = curl_mime_addpart(mime);

    char snapshot_path[64] = "";
    int have_snapshot = (snapshot_frame_to_tempfile(cfg->frame_path, snapshot_path, sizeof(snapshot_path)) == 0);

    CURLcode file_rc = CURLE_READ_ERROR;
    if (have_snapshot) {
        file_rc = curl_mime_filedata(attachment_part, snapshot_path);
    }

    if (!have_snapshot || file_rc != CURLE_OK) {
        fprintf(stderr, "[email] warning: could not attach %s — sending without attachment\n",
                cfg->frame_path);
    } else {
        curl_mime_type(attachment_part, "image/jpeg");
        curl_mime_filename(attachment_part, "frame.jpg");
        /* CRITICAL: without this, libcurl sends the raw JPEG bytes with
         * no Content-Transfer-Encoding at all — SMTP is a line-based
         * 7/8-bit text protocol, so any lone 0x0A byte inside the binary
         * scan data (which occurs naturally in JPEG data) can get
         * mangled by mail relays along the way (CRLF normalization,
         * dot-stuffing, etc). base64 is the standard fix: it re-encodes
         * the binary as safe ASCII before transport. Omitting this call
         * is exactly what produced a structurally-valid-but-visually-
         * corrupted JPEG in testing — the file size/markers looked fine,
         * but scan data bytes had been altered in transit. */
        curl_mime_encoder(attachment_part, "base64");
    }

    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    /* Uncomment while debugging SMTP auth/TLS issues:
     * curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
     */

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        fprintf(stderr, "[email] send failed: %s\n", curl_easy_strerror(rc));
    } else {
        fprintf(stdout, "[email] sent alert for count=%d timestamp=%s\n", count, timestamp);
        result = 0;
    }

    curl_mime_free(mime);
    curl_slist_free_all(headers);
    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);

    if (have_snapshot) {
        unlink(snapshot_path);
    }

    return result;
}
