/* Copyright (C) 2009-2019 Greenbone Networks GmbH
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * @file  manage.c
 * @brief The Greenbone Vulnerability Manager management layer.
 *
 * This file defines a management layer, for implementing
 * Managers such as the Greenbone Vulnerability Manager daemon.
 *
 * This layer provides facilities for storing and manipulating user
 * data (credentials, targets, tasks, reports, schedules, roles, etc)
 * and general security data (NVTs, CVEs, etc).
 * Task manipulation includes controlling external facilities such as
 * OSP scanners.
 *
 * Simply put, the daemon's GMP implementation uses this layer to do the work.
 */

/**
 * @brief Enable extra functions.
 *
 * time.h in glibc2 needs this for strptime.
 */
#define _XOPEN_SOURCE

/**
 * @brief Enable extra GNU functions.
 *
 * pthread_sigmask () needs this with glibc < 2.19
 */
#define _GNU_SOURCE

#include "manage.h"
#include "manage_acl.h"
#include "manage_sql.h"
#include "manage_sql_secinfo.h"
#include "manage_sql_nvts.h"
#include "manage_sql_tickets.h"
#include "manage_sql_tls_certificates.h"
#include "utils.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <glib.h>
#include <gnutls/x509.h> /* for gnutls_x509_crt_... */
#include <math.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <gvm/base/hosts.h>
#include <gvm/base/proctitle.h>
#include <gvm/osp/osp.h>
#include <gvm/util/fileutils.h>
#include <gvm/util/serverutils.h>
#include <gvm/util/uuidutils.h>
#include <gvm/gmp/gmp.h>

#undef G_LOG_DOMAIN
/**
 * @brief GLib log domain.
 */
#define G_LOG_DOMAIN "md manage"

/**
 * @brief CPE selection stylesheet location.
 */
#define CPE_GETBYNAME_XSL GVM_SCAP_RES_DIR "/cpe_getbyname.xsl"

/**
 * @brief CVE selection stylesheet location.
 */
#define CVE_GETBYNAME_XSL GVM_SCAP_RES_DIR "/cve_getbyname.xsl"

/**
 * @brief OVALDEF selection stylesheet location.
 */
#define OVALDEF_GETBYNAME_XSL GVM_SCAP_RES_DIR "/ovaldef_getbyname.xsl"

/**
 * @brief CERT_BUND_ADV selection stylesheet location.
 */
#define CERT_BUND_ADV_GETBYNAME_XSL GVM_CERT_RES_DIR "/cert_bund_getbyname.xsl"

/**
 * @brief DFN_CERT_ADV selection stylesheet location.
 */
#define DFN_CERT_ADV_GETBYNAME_XSL GVM_CERT_RES_DIR "/dfn_cert_getbyname.xsl"

/**
 * @brief CPE dictionary location.
 */
#define CPE_DICT_FILENAME GVM_SCAP_DATA_DIR "/official-cpe-dictionary_v2.2.xml"

/**
 * @brief CVE data files location format string.
 *
 * %d should be the year expressed as YYYY.
 */
#define CVE_FILENAME_FMT GVM_SCAP_DATA_DIR "/nvdcve-2.0-%d.xml"

/**
 * @brief CERT-Bund data files location format string.
 *
 * %d should be the year without the century (expressed as YY),
 */
#define CERT_BUND_ADV_FILENAME_FMT GVM_CERT_DATA_DIR "/CB-K%02d.xml"

/**
 * @brief DFN-CERT data files location format string.
 *
 * First %d should be the year expressed as YYYY,
 * second %d should be should be Month expressed as MM.
 */
#define DFN_CERT_ADV_FILENAME_FMT GVM_CERT_DATA_DIR "/dfn-cert-%04d.xml"

/**
 * @brief SCAP timestamp location.
 */
#define SCAP_TIMESTAMP_FILENAME GVM_SCAP_DATA_DIR "/timestamp"

/**
 * @brief CERT timestamp location.
 */
#define CERT_TIMESTAMP_FILENAME GVM_CERT_DATA_DIR "/timestamp"

/**
 * @brief Default for Scanner max_checks preference.
 */
#define MAX_CHECKS_DEFAULT "4"

/**
 * @brief Default for Scanner max_hosts preference.
 */
#define MAX_HOSTS_DEFAULT "20"

extern volatile int termination_signal;

/**
 * @brief Path to the relay mapper executable, NULL to disable relays.
 */
static gchar *relay_mapper_path = NULL;

/**
 * @brief Whether to migrate sensors if relays do not match.
 */
static int relay_migrate_sensors = 0;

/**
 * @brief Number of minutes before overdue tasks timeout.
 */
static int schedule_timeout = SCHEDULE_TIMEOUT_DEFAULT;


/* Certificate and key management. */

/**
 * @brief Truncate a certificate, removing extra data.
 *
 * @param[in]  certificate    The certificate.
 *
 * @return  The truncated certificate as a newly allocated string or NULL.
 */
gchar *
truncate_certificate (const gchar* certificate)
{
  GString *cert_buffer;
  gchar *current_pos, *cert_start, *cert_end;
  gboolean done = FALSE;
  cert_buffer = g_string_new ("");

  current_pos = (gchar *) certificate;
  while (done == FALSE && *current_pos != '\0')
    {
      cert_start = NULL;
      cert_end = NULL;
      if (g_str_has_prefix (current_pos,
                            "-----BEGIN CERTIFICATE-----"))
        {
          cert_start = current_pos;
          cert_end = strstr (cert_start,
                             "-----END CERTIFICATE-----");
          if (cert_end)
            cert_end += strlen ("-----END CERTIFICATE-----");
          else
            done = TRUE;
        }
      else if (g_str_has_prefix (current_pos,
                                 "-----BEGIN TRUSTED CERTIFICATE-----"))
        {
          cert_start = current_pos;
          cert_end = strstr (cert_start,
                             "-----END TRUSTED CERTIFICATE-----");
          if (cert_end)
            cert_end += strlen ("-----END TRUSTED CERTIFICATE-----");
          else
            done = TRUE;
        }
      else if (g_str_has_prefix (current_pos,
                                 "-----BEGIN PKCS7-----"))
        {
          cert_start = current_pos;
          cert_end = strstr (cert_start,
                             "-----END PKCS7-----");
          if (cert_end)
            cert_end += strlen ("-----END PKCS7-----");
          else
            done = TRUE;
        }

      if (cert_start && cert_end)
        {
          g_string_append_len (cert_buffer, cert_start, cert_end - cert_start);
          g_string_append_c (cert_buffer, '\n');
        }
      current_pos++;
    }

  return g_string_free (cert_buffer, cert_buffer->len == 0);
}

/**
 * @brief Truncate a private key, removing extra data.
 *
 * @param[in]  private_key    The private key.
 *
 * @return  The truncated private key as a newly allocated string or NULL.
 */
gchar *
truncate_private_key (const gchar* private_key)
{
  gchar *key_start, *key_end;
  key_end = NULL;
  key_start = strstr (private_key, "-----BEGIN RSA PRIVATE KEY-----");
  if (key_start)
    {
      key_end = strstr (key_start, "-----END RSA PRIVATE KEY-----");

      if (key_end)
        key_end += strlen ("-----END RSA PRIVATE KEY-----");
      else
        return NULL;
    }

  if (key_start == NULL)
    {
      key_start = strstr (private_key, "-----BEGIN DSA PRIVATE KEY-----");
      if (key_start)
        {
          key_end = strstr (key_start, "-----END DSA PRIVATE KEY-----");

          if (key_end)
            key_end += strlen ("-----END DSA PRIVATE KEY-----");
          else
            return NULL;
        }
    }

  if (key_start == NULL)
    {
      key_start = strstr (private_key, "-----BEGIN EC PRIVATE KEY-----");
      if (key_start)
        {
          key_end = strstr (key_start, "-----END EC PRIVATE KEY-----");

          if (key_end)
            key_end += strlen ("-----END EC PRIVATE KEY-----");
          else
            return NULL;
        }
    }

  if (key_end && key_end[0] == '\n')
    key_end++;

  if (key_start == NULL || key_end == NULL)
    return NULL;
  else
    return g_strndup (key_start, key_end - key_start);
}

/**
 * @brief Gathers info from a certificate.
 *
 * @param[in]  certificate        The certificate to get data from.
 * @param[in]  certificate_len    Length of certificate, -1: null-terminated
 * @param[out] activation_time    Pointer to write activation time to.
 * @param[out] expiration_time    Pointer to write expiration time to.
 * @param[out] md5_fingerprint    Pointer for newly allocated MD5 fingerprint.
 * @param[out] sha256_fingerprint Pointer for newly allocated SHA-256
 *                                fingerprint.
 * @param[out] subject            Pointer for newly allocated subject DN.
 * @param[out] issuer             Pointer for newly allocated issuer DN.
 * @param[out] serial             Pointer for newly allocated serial.
 * @param[out] certificate_format Pointer to certificate format.
 *
 * @return 0 success, -1 error.
 */
int
get_certificate_info (const gchar* certificate, gssize certificate_len,
                      time_t* activation_time, time_t* expiration_time,
                      gchar** md5_fingerprint, gchar **sha256_fingerprint,
                      gchar **subject, gchar** issuer, gchar **serial,
                      gnutls_x509_crt_fmt_t *certificate_format)
{
  gchar *cert_truncated;
  gnutls_x509_crt_fmt_t certificate_format_internal;

  cert_truncated = NULL;
  if (activation_time)
    *activation_time = -1;
  if (expiration_time)
    *expiration_time = -1;
  if (md5_fingerprint)
    *md5_fingerprint = NULL;
  if (sha256_fingerprint)
    *sha256_fingerprint = NULL;
  if (subject)
    *subject = NULL;
  if (issuer)
    *issuer = NULL;
  if (serial)
    *serial = NULL;
  if (certificate_format)
    *certificate_format = GNUTLS_X509_FMT_DER;

  if (certificate)
    {
      int err;
      gnutls_datum_t cert_datum;
      gnutls_x509_crt_t gnutls_cert;
      static const gchar* begin_str = "-----BEGIN ";

      if (g_strstr_len (certificate, certificate_len, begin_str))
        {
          cert_truncated = truncate_certificate (certificate);
          if (cert_truncated == NULL)
            {
              return -1;
            }
          certificate_format_internal = GNUTLS_X509_FMT_PEM;
        }
      else
        {
          if (certificate_len < 0)
            {
              g_warning ("%s: PEM encoded certificate expected if"
                         " certificate_length is negative",
                         __FUNCTION__);
              return -1;
            }

          cert_truncated = g_memdup (certificate, certificate_len);
          certificate_format_internal = GNUTLS_X509_FMT_DER;
        }

      cert_datum.data = (unsigned char*) cert_truncated;
      if (certificate_len < 0)
        cert_datum.size = strlen (cert_truncated);
      else
        cert_datum.size = certificate_len;

      gnutls_x509_crt_init (&gnutls_cert);
      err = gnutls_x509_crt_import (gnutls_cert, &cert_datum,
                                    certificate_format_internal);
      if (err)
        {
          g_free (cert_truncated);
          return -1;
        }

      if (certificate_format)
        *certificate_format = certificate_format_internal;

      if (activation_time)
        {
          *activation_time
            = gnutls_x509_crt_get_activation_time (gnutls_cert);
        }

      if (expiration_time)
        {
          *expiration_time
            = gnutls_x509_crt_get_expiration_time (gnutls_cert);
        }

      if (md5_fingerprint)
        {
          int i;
          size_t buffer_size = 16;
          unsigned char buffer[buffer_size];
          GString *string;

          string = g_string_new ("");

          gnutls_x509_crt_get_fingerprint (gnutls_cert, GNUTLS_DIG_MD5,
                                           buffer, &buffer_size);

          for (i = 0; i < buffer_size; i++)
            {
              if (i != 0)
                {
                  g_string_append_c (string, ':');
                }
              g_string_append_printf (string, "%02x", buffer[i]);
            }

          *md5_fingerprint = string->str;
          g_string_free (string, FALSE);
        }

      if (sha256_fingerprint)
        {
          int i;
          size_t buffer_size = 32;
          unsigned char buffer[buffer_size];
          GString *string;

          string = g_string_new ("");

          gnutls_x509_crt_get_fingerprint (gnutls_cert, GNUTLS_DIG_SHA256,
                                           buffer, &buffer_size);

          for (i = 0; i < buffer_size; i++)
            {
              g_string_append_printf (string, "%02X", buffer[i]);
            }

          *sha256_fingerprint = string->str;
          g_string_free (string, FALSE);
        }

      if (subject)
        {
          size_t buffer_size = 0;
          gchar *buffer;
          gnutls_x509_crt_get_dn (gnutls_cert, NULL, &buffer_size);
          buffer = g_malloc (buffer_size);
          gnutls_x509_crt_get_dn (gnutls_cert, buffer, &buffer_size);

          *subject = buffer;
        }

      if (issuer)
        {
          size_t buffer_size = 0;
          gchar *buffer;
          gnutls_x509_crt_get_issuer_dn (gnutls_cert, NULL, &buffer_size);
          buffer = g_malloc (buffer_size);
          gnutls_x509_crt_get_issuer_dn (gnutls_cert, buffer, &buffer_size);

          *issuer = buffer;
        }

      if (serial)
        {
          int i;
          size_t buffer_size = 0;
          gchar* buffer;
          GString *string;

          string = g_string_new ("");

          gnutls_x509_crt_get_serial (gnutls_cert, NULL, &buffer_size);
          buffer = g_malloc (buffer_size);
          gnutls_x509_crt_get_serial (gnutls_cert, buffer, &buffer_size);

          for (i = 0; i < buffer_size; i++)
            {
              g_string_append_printf (string, "%02X", buffer[i]);
            }

          *serial = string->str;
          g_string_free (string, FALSE);
        }

      gnutls_x509_crt_deinit (gnutls_cert);
      g_free (cert_truncated);
    }
  return 0;
}

/**
 * @brief Converts a certificate time to an ISO time string.
 *
 * @param[in] time  The time as a time_t.
 *
 * @return Newly allocated string.
 */
gchar *
certificate_iso_time (time_t time)
{
  if (time == 0)
    return g_strdup ("unlimited");
  else if (time == -1)
    return g_strdup ("unknown");
  else
    return g_strdup (iso_time (&time));
}

/**
 * @brief Tests the activation and expiration time of a certificate.
 *
 * @param[in] activates  Activation time.
 * @param[in] expires    Expiration time.
 *
 * @return Static status string.
 */
const gchar *
certificate_time_status (time_t activates, time_t expires)
{
  time_t now;
  time (&now);

  if (activates == -1 || expires == -1)
    return "unknown";
  else if (activates > now)
    return "inactive";
  else if (expires != 0 && expires < now)
    return "expired";
  else
    return "valid";
}


/* Helpers. */

/**
 * @brief Truncates text to a maximum length, optionally appends a suffix.
 *
 * Note: The string is modified in place instead of allocating a new one.
 * With the xml option the function will avoid cutting the string in the middle
 *  of XML entities, but element tags will be ignored.
 *
 * @param[in,out] string   The string to truncate.
 * @param[in]     max_len  The maximum length in bytes.
 * @param[in]     xml      Whether to preserve XML entities.
 * @param[in]     suffix   The suffix to append when the string is shortened.
 */
static void
truncate_text (gchar *string, size_t max_len, gboolean xml, const char *suffix)
{
  if (strlen (string) <= max_len)
    return;
  else
    {
      size_t offset;
      offset = max_len;

      // Move offset according according to suffix length
      if (suffix && strlen (suffix) < max_len)
        offset = offset - strlen (suffix);

      // Go back to start of UTF-8 character
      if (offset > 0 && (string[offset] & 0x80) == 0x80)
        {
          offset = g_utf8_find_prev_char (string, string + offset) - string;
        }

      if (xml)
        {
          // If the offset is in the middle of an XML entity,
          //  move the offset to the start of that entity.
          ssize_t entity_start_offset = offset;

          while (entity_start_offset >= 0 
                 && string[entity_start_offset] != '&')
            {
              entity_start_offset --;
            }

          if (entity_start_offset >= 0)
            {
              char *entity_end = strchr(string + entity_start_offset, ';');
              if (entity_end && (entity_end - string) >= offset)
                offset = entity_start_offset;
            }
        }

      // Truncate the string, inserting the suffix if applicable
      if (suffix && strlen (suffix) < max_len)
        sprintf (string + offset, "%s", suffix);
      else
        string[offset] = '\0';
    }
}

/**
 * @brief XML escapes text truncating to a maximum length with a suffix.
 *
 * Note: The function will avoid cutting the string in the middle of XML
 *  entities.
 *
 * @param[in]  string   The string to truncate.
 * @param[in]  max_len  The maximum length in bytes.
 * @param[in]  suffix   The suffix to append when the string is shortened.
 *
 * @return Newly allocated string with XML escaped, truncated text.
 */
gchar *
xml_escape_text_truncated (const char *string, size_t max_len,
                           const char *suffix)
{
  gchar *escaped;
  gssize orig_len;

  orig_len = strlen (string);
  if (orig_len <= max_len)
    escaped = g_markup_escape_text (string, -1);
  else
    {
      gchar *offset_next;
      ssize_t offset;

      offset_next = g_utf8_find_next_char (string + max_len,
                                           string + orig_len);
      offset = offset_next - string;
      escaped = g_markup_escape_text (string, offset);
    }

  truncate_text (escaped, max_len, TRUE, suffix);
  return escaped;
}

/**
 * @brief Return the plural name of a resource type.
 *
 * @param[in]  type  Resource type.
 *
 * @return Plural name of type.
 */
const char *
type_name_plural (const char* type)
{
  if (type == NULL)
    return "ERROR";

  if (strcasecmp (type, "cpe") == 0)
    return "CPEs";
  if (strcasecmp (type, "cve") == 0)
    return "CVEs";
  if (strcasecmp (type, "cert_bund_adv") == 0)
    return "CERT-Bund Advisories";
  if (strcasecmp (type, "dfn_cert_adv") == 0)
    return "DFN-CERT Advisories";
  if (strcasecmp (type, "nvt") == 0)
    return "NVTs";
  if (strcasecmp (type, "ovaldef") == 0)
    return "OVAL Definitions";

  return "ERROR";
}

/**
 * @brief Return the name of a resource type.
 *
 * @param[in]  type  Resource type.
 *
 * @return Name of type.
 */
const char *
type_name (const char* type)
{
  if (type == NULL)
    return "ERROR";

  if (strcasecmp (type, "cpe") == 0)
    return "CPE";
  if (strcasecmp (type, "cve") == 0)
    return "CVE";
  if (strcasecmp (type, "cert_bund_adv") == 0)
    return "CERT-Bund Advisory";
  if (strcasecmp (type, "dfn_cert_adv") == 0)
    return "DFN-CERT Advisory";
  if (strcasecmp (type, "nvt") == 0)
    return "NVT";
  if (strcasecmp (type, "ovaldef") == 0)
    return "OVAL Definition";

  return "ERROR";
}

/**
 * @brief Check if a type is a SCAP type.
 *
 * @param[in]  type  Resource type.
 *
 * @return Name of type.
 */
int
type_is_scap (const char* type)
{
  return (strcasecmp (type, "cpe") == 0)
         || (strcasecmp (type, "cve") == 0)
         || (strcasecmp (type, "ovaldef") == 0);
}

/**
 * @brief Check whether a resource is available.
 *
 * @param[in]   type        Type.
 * @param[out]  resource    Resource.
 * @param[out]  permission  Permission required for this operation.
 *
 * @return 0 success, -1 error, 99 permission denied.
 */
static int
check_available (const gchar *type, resource_t resource,
                 const gchar *permission)
{
  if (resource)
    {
      gchar *uuid;
      resource_t found;

      uuid = resource_uuid (type, resource);
      if (find_resource_with_permission (type, uuid, &found, permission, 0))
        {
          g_free (uuid);
          return -1;
        }
      g_free (uuid);
      if (found == 0)
        return 99;

      return 0;
    }

  return -1;
}


/* Severity related functions. */

/**
 * @brief Get the message type of a threat.
 *
 * @param  threat  Threat.
 *
 * @return Static message type name if threat names a threat, else NULL.
 */
const char *
threat_message_type (const char *threat)
{
  if (strcasecmp (threat, "High") == 0)
    return "Alarm";
  if (strcasecmp (threat, "Medium") == 0)
    return "Alarm";
  if (strcasecmp (threat, "Low") == 0)
    return "Alarm";
  if (strcasecmp (threat, "Log") == 0)
    return "Log Message";
  if (strcasecmp (threat, "Debug") == 0)
    return "Debug Message";
  if (strcasecmp (threat, "Error") == 0)
    return "Error Message";
  if (strcasecmp (threat, "False Positive") == 0)
    return "False Positive";
  return NULL;
}

/**
 * @brief Get the threat of a message type.
 *
 * @param  type  Message type.
 *
 * @return Static threat name if type names a message type, else NULL.
 */
const char *
message_type_threat (const char *type)
{
  if (strcasecmp (type, "Alarm") == 0)
    return "Alarm";
  if (strcasecmp (type, "Security Hole") == 0)
    return "High";
  if (strcasecmp (type, "Security Warning") == 0)
    return "Medium";
  if (strcasecmp (type, "Security Note") == 0)
    return "Low";
  if (strcasecmp (type, "Log Message") == 0)
    return "Log";
  if (strcasecmp (type, "Debug Message") == 0)
    return "Debug";
  if (strcasecmp (type, "Error Message") == 0)
    return "Error";
  if (strcasecmp (type, "False Positive") == 0)
    return "False Positive";
  return NULL;
}

/**
 * @brief Check whether a severity falls within a threat level.
 *
 * @param[in]  severity  Severity.
 * @param[in]  level     Threat level.
 *
 * @return 1 if in level, else 0.
 */
int
severity_in_level (double severity, const char *level)
{
  const char *class;

  class = setting_severity ();
  if (strcmp (class, "classic") == 0)
    {
      if (strcmp (level, "high") == 0)
        return severity > 5 && severity <= 10;
      else if (strcmp (level, "medium") == 0)
        return severity > 2 && severity <= 5;
      else if (strcmp (level, "low") == 0)
        return severity > 0 && severity <= 2;
      else if (strcmp (level, "none") == 0 || strcmp (level, "log") == 0)
        return severity == 0;
      else
        return 0;
    }
  else if (strcmp (class, "pci-dss") == 0)
    {
      if (strcmp (level, "high") == 0)
        return severity >= 4.0;
      else if (strcmp (level, "none") == 0 || strcmp (level, "log") == 0)
        return severity >= 0.0 && severity < 4.0;
      else
        return 0;
    }
  else
    {
      /* NIST/BSI. */
      if (strcmp (level, "high") == 0)
        return severity >= 7 && severity <= 10;
      else if (strcmp (level, "medium") == 0)
        return severity >= 4 && severity < 7;
      else if (strcmp (level, "low") == 0)
        return severity > 0 && severity < 4;
      else if (strcmp (level, "none") == 0  || strcmp (level, "log") == 0)
        return severity == 0;
      else
        return 0;
    }
}

/**
 * @brief Get the threat level matching a severity score.
 *
 * @param[in] severity  severity score
 * @param[in] mode      0 for normal levels, 1 to use "Alarm" for severity > 0.0
 *
 * @return the level as a static string
 */
const char*
severity_to_level (double severity, int mode)
{
  if (severity == SEVERITY_LOG)
    return "Log";
  else if (severity == SEVERITY_FP)
    return "False Positive";
  else if (severity == SEVERITY_DEBUG)
    return "Debug";
  else if (severity == SEVERITY_ERROR)
    return "Error";
  else if (severity > 0.0 && severity <= 10.0)
    {
      if (mode == 1)
        return "Alarm";
      else if (severity_in_level (severity, "high"))
        return "High";
      else if (severity_in_level (severity, "medium"))
        return "Medium";
      else if (severity_in_level (severity, "low"))
        return "Low";
      else
        return "Log";
    }
  else
    {
      g_warning ("%s: Invalid severity score given: %f",
                 __FUNCTION__, severity);
      return NULL;
    }
}

/**
 * @brief Get the message type matching a severity score.
 *
 * @param[in] severity  severity score
 *
 * @return the message type as a static string
 */
const char*
severity_to_type (double severity)
{
  if (severity == SEVERITY_LOG)
    return "Log Message";
  else if (severity == SEVERITY_FP)
    return "False Positive";
  else if (severity == SEVERITY_DEBUG)
    return "Debug Message";
  else if (severity == SEVERITY_ERROR)
    return "Error Message";
  else if (severity > 0.0 && severity <= 10.0)
    return "Alarm";
  else
    {
      g_warning ("%s: Invalid severity score given: %f",
                 __FUNCTION__, severity);
      return NULL;
    }
}


/* Credentials. */

/**
 * @brief Current credentials during any GMP command.
 */
credentials_t current_credentials;


/* Reports. */

/**
 * @brief Delete all the reports for a task.
 *
 * It's up to the caller to ensure that this runs in a contention safe
 * context (for example within an SQL transaction).
 *
 * @param[in]  task  A task descriptor.
 *
 * @return 0 on success, -1 on error.
 */
int
delete_reports (task_t task)
{
  report_t report;
  iterator_t iterator;
  init_report_iterator_task (&iterator, task);
  while (next_report (&iterator, &report))
    if (delete_report_internal (report))
      {
        cleanup_iterator (&iterator);
        return -1;
      }
  cleanup_iterator (&iterator);
  return 0;
}

/**
 * @brief Create a basic filter term to get report results.
 *
 * @param[in]  first            First row.
 * @param[in]  rows             Number of rows.
 * @param[in]  apply_overrides  Whether to apply overrides.
 * @param[in]  autofp           Auto-FP value.
 * @param[in]  min_qod          Minimum QOD.
 *
 * @return Filter term.
 */
static gchar *
report_results_filter_term (int first, int rows,
                            int apply_overrides, int autofp, int min_qod)
{
  return g_strdup_printf ("first=%d rows=%d"
                          " apply_overrides=%d autofp=%d min_qod=%d",
                          first, rows,
                          apply_overrides, autofp, min_qod);
}


/**
 * @brief Create a new basic get_data_t struct to get report results.
 *
 * @param[in]  first            First row.
 * @param[in]  rows             Number of rows.
 * @param[in]  apply_overrides  Whether to apply overrides.
 * @param[in]  autofp           Auto-FP value.
 * @param[in]  min_qod          Minimum QOD.
 *
 * @return GET data struct.
 */
get_data_t*
report_results_get_data (int first, int rows,
                         int apply_overrides, int autofp, int min_qod)
{
  get_data_t* get = g_malloc (sizeof (get_data_t));
  memset (get, 0, sizeof (get_data_t));
  get->type = g_strdup ("result");
  get->filter = report_results_filter_term (first, rows,
                                            apply_overrides, autofp, min_qod);

  return get;
}

/**
 * @brief Array index of severity 0.0 in the severity_data_t.counts array.
 */
#define ZERO_SEVERITY_INDEX 4

/**
 * @brief Convert a severity value into an index in the counts array.
 *
 * @param[in]   severity        Severity value.
 *
 * @return      The index, 0 for invalid severity scores.
 */
static int
severity_data_index (double severity)
{
  int ret;
  if (severity >= 0.0)
    ret = (int)(round (severity * SEVERITY_SUBDIVISIONS)) + ZERO_SEVERITY_INDEX;
  else if (severity == SEVERITY_FP || severity == SEVERITY_DEBUG
           || severity == SEVERITY_ERROR)
    ret = (int)(round (severity)) + ZERO_SEVERITY_INDEX;
  else
    ret = 0;

  return ret;
}

/**
 * @brief Convert an index in the counts array to a severity value.
 *
 * @param[in]   index   Index in the counts array.
 *
 * @return      The corresponding severity value.
 */
double
severity_data_value (int index)
{
  double ret;
  if (index <= ZERO_SEVERITY_INDEX && index > 0)
    ret = ((double) index) - ZERO_SEVERITY_INDEX;
  else if (index <= (ZERO_SEVERITY_INDEX
                     + (SEVERITY_SUBDIVISIONS * SEVERITY_MAX)))
    ret = (((double) (index - ZERO_SEVERITY_INDEX)) / SEVERITY_SUBDIVISIONS);
  else
    ret = SEVERITY_MISSING;

  return ret;
}

/**
 * @brief Initialize a severity data structure.
 *
 * @param[in] data  The data structure to initialize.
 */
void
init_severity_data (severity_data_t* data)
{
  int max_i;
  max_i = ZERO_SEVERITY_INDEX + (SEVERITY_SUBDIVISIONS * SEVERITY_MAX);

  data->counts = g_malloc0 (sizeof (int) * (max_i + 1));

  data->total = 0;
  data->max = SEVERITY_MISSING;
}

/**
 * @brief Clean up a severity data structure.
 *
 * @param[in] data  The data structure to initialize.
 */
void
cleanup_severity_data (severity_data_t* data)
{
  g_free (data->counts);
}

/**
 * @brief Add a severity occurrence to the counts of a severity_data_t.
 *
 * @param[in]   severity_data   The severity count struct to add to.
 * @param[in]   severity        The severity to add.
 */
void
severity_data_add (severity_data_t* severity_data, double severity)
{
  (severity_data->counts)[severity_data_index (severity)]++;

  if (severity_data->total == 0 || severity_data->max <= severity)
    severity_data->max = severity;

  (severity_data->total)++;
}

/**
 * @brief Add a multiple severity occurrences to the counts of a severity_data_t.
 *
 * @param[in]   severity_data   The severity count struct to add to.
 * @param[in]   severity        The severity to add.
 * @param[in]   count           The number of occurrences to add.
 */
void
severity_data_add_count (severity_data_t* severity_data, double severity,
                         int count)
{
  (severity_data->counts)[severity_data_index (severity)] += count;

  if (severity_data->total == 0 || severity_data->max <= severity)
    severity_data->max = severity;

  (severity_data->total) += count;
}

/**
 * @brief Calculate the total of severity counts in a range.
 *
 * @param[in]  severity_data   The severity data struct to get counts from.
 * @param[in]  min_severity    The minimum severity included in the range.
 * @param[in]  max_severity    The maximum severity included in the range.
 *
 * @return     The total of severity counts in the specified range.
 */
static int
severity_data_range_count (const severity_data_t* severity_data,
                           double min_severity, double max_severity)
{
  int i, i_max, count;

  i_max = severity_data_index (max_severity);
  count = 0;

  for (i = severity_data_index (min_severity);
       i <= i_max;
       i++)
    {
      count += (severity_data->counts)[i];
    }
  return count;
}

/**
 * @brief Count the occurrences of severities in the levels.
 *
 * @param[in] severity_data    The severity counts data to evaluate.
 * @param[in] severity_class   The severity class setting to use.
 * @param[out] errors          The number of error messages.
 * @param[out] debugs          The number of debug messages.
 * @param[out] false_positives The number of False Positives.
 * @param[out] logs            The number of Log messages.
 * @param[out] lows            The number of Low severity results.
 * @param[out] mediums         The number of Medium severity results.
 * @param[out] highs           The number of High severity results.
 */
void
severity_data_level_counts (const severity_data_t *severity_data,
                            const gchar *severity_class,
                            int *errors, int *debugs, int *false_positives,
                            int *logs, int *lows, int *mediums, int *highs)
{
  if (errors)
    *errors
      = severity_data_range_count (severity_data,
                                   level_min_severity ("Error",
                                                       severity_class),
                                   level_max_severity ("Error",
                                                       severity_class));

  if (debugs)
    *debugs
      = severity_data_range_count (severity_data,
                                   level_min_severity ("Debug",
                                                       severity_class),
                                   level_max_severity ("Debug",
                                                       severity_class));

  if (false_positives)
    *false_positives
      = severity_data_range_count (severity_data,
                                   level_min_severity ("False Positive",
                                                       severity_class),
                                   level_max_severity ("False Positive",
                                                       severity_class));

  if (logs)
    *logs
      = severity_data_range_count (severity_data,
                                   level_min_severity ("Log",
                                                       severity_class),
                                   level_max_severity ("Log",
                                                       severity_class));

  if (lows)
    *lows
      = severity_data_range_count (severity_data,
                                   level_min_severity ("low",
                                                       severity_class),
                                   level_max_severity ("low",
                                                       severity_class));

  if (mediums)
    *mediums
      = severity_data_range_count (severity_data,
                                   level_min_severity ("medium",
                                                       severity_class),
                                   level_max_severity ("medium",
                                                       severity_class));

  if (highs)
    *highs
      = severity_data_range_count (severity_data,
                                   level_min_severity ("high",
                                                       severity_class),
                                   level_max_severity ("high",
                                                       severity_class));
}


/* Task globals. */

/**
 * @brief The task currently running on the scanner.
 */
task_t current_scanner_task = (task_t) 0;

/**
 * @brief The report of the current task.
 */
report_t global_current_report = (report_t) 0;


/* Alerts. */

/**
 * @brief Frees a alert_report_data_t struct, including contained data.
 *
 * @param[in]  data   The struct to free.
 */
void
alert_report_data_free (alert_report_data_t *data)
{
  if (data == NULL)
    return;

  alert_report_data_reset (data);
  g_free (data);
}

/**
 * @brief Frees content of an alert_report_data_t, but not the struct itself.
 *
 * @param[in]  data   The struct to free.
 */
void
alert_report_data_reset (alert_report_data_t *data)
{
  if (data == NULL)
    return;

  g_free (data->content_type);
  g_free (data->local_filename);
  g_free (data->remote_filename);
  g_free (data->report_format_name);

  memset (data, 0, sizeof (alert_report_data_t));
}

/**
 * @brief Get the name of an alert condition.
 *
 * @param[in]  condition  Condition.
 *
 * @return The name of the condition (for example, "Always").
 */
const char*
alert_condition_name (alert_condition_t condition)
{
  switch (condition)
    {
      case ALERT_CONDITION_ALWAYS:
        return "Always";
      case ALERT_CONDITION_FILTER_COUNT_AT_LEAST:
        return "Filter count at least";
      case ALERT_CONDITION_FILTER_COUNT_CHANGED:
        return "Filter count changed";
      case ALERT_CONDITION_SEVERITY_AT_LEAST:
        return "Severity at least";
      case ALERT_CONDITION_SEVERITY_CHANGED:
        return "Severity changed";
      default:
        return "Internal Error";
    }
}

/**
 * @brief Get the name of an alert event.
 *
 * @param[in]  event  Event.
 *
 * @return The name of the event (for example, "Run status changed").
 */
const char*
event_name (event_t event)
{
  switch (event)
    {
      case EVENT_TASK_RUN_STATUS_CHANGED: return "Task run status changed";
      case EVENT_NEW_SECINFO:             return "New SecInfo arrived";
      case EVENT_UPDATED_SECINFO:         return "Updated SecInfo arrived";
      case EVENT_TICKET_RECEIVED:         return "Ticket received";
      case EVENT_ASSIGNED_TICKET_CHANGED: return "Assigned ticket changed";
      case EVENT_OWNED_TICKET_CHANGED:    return "Owned ticket changed";
      default:                            return "Internal Error";
    }
}

/**
 * @brief Get a description of an alert condition.
 *
 * @param[in]  condition  Condition.
 * @param[in]  alert  Alert.
 *
 * @return Freshly allocated description of condition.
 */
gchar*
alert_condition_description (alert_condition_t condition,
                             alert_t alert)
{
  switch (condition)
    {
      case ALERT_CONDITION_ALWAYS:
        return g_strdup ("Always");
      case ALERT_CONDITION_FILTER_COUNT_AT_LEAST:
        {
          char *level;
          gchar *ret;

          level = alert_data (alert, "condition", "severity");
          ret = g_strdup_printf ("Filter count at least %s",
                                 level ? level : "0");
          free (level);
          return ret;
        }
      case ALERT_CONDITION_FILTER_COUNT_CHANGED:
        return g_strdup ("Filter count changed");
      case ALERT_CONDITION_SEVERITY_AT_LEAST:
        {
          char *level = alert_data (alert, "condition", "severity");
          gchar *ret = g_strdup_printf ("Task severity is at least '%s'",
                                        level);
          free (level);
          return ret;
        }
      case ALERT_CONDITION_SEVERITY_CHANGED:
        {
          char *direction;
          direction = alert_data (alert, "condition", "direction");
          gchar *ret = g_strdup_printf ("Task severity %s", direction);
          free (direction);
          return ret;
        }
      default:
        return g_strdup ("Internal Error");
    }
}

/**
 * @brief Get a description of an alert event.
 *
 * @param[in]  event       Event.
 * @param[in]  event_data  Event data.
 * @param[in]  task_name   Name of task if required in description, else NULL.
 *
 * @return Freshly allocated description of event.
 */
gchar*
event_description (event_t event, const void *event_data, const char *task_name)
{
  switch (event)
    {
      case EVENT_TASK_RUN_STATUS_CHANGED:
        if (task_name)
          return g_strdup_printf
                  ("The security scan task '%s' changed status to '%s'",
                   task_name,
                   run_status_name ((task_status_t) event_data));
        return g_strdup_printf ("Task status changed to '%s'",
                                run_status_name ((task_status_t) event_data));
        break;
      case EVENT_NEW_SECINFO:
        return g_strdup_printf ("New SecInfo arrived");
        break;
      case EVENT_UPDATED_SECINFO:
        return g_strdup_printf ("Updated SecInfo arrived");
        break;
      case EVENT_TICKET_RECEIVED:
        return g_strdup_printf ("Ticket received");
        break;
      case EVENT_ASSIGNED_TICKET_CHANGED:
        return g_strdup_printf ("Assigned ticket changed");
        break;
      case EVENT_OWNED_TICKET_CHANGED:
        return g_strdup_printf ("Owned ticket changed");
        break;
      default:
        return g_strdup ("Internal Error");
    }
}

/**
 * @brief Get the name of an alert method.
 *
 * @param[in]  method  Method.
 *
 * @return The name of the method (for example, "Email" or "SNMP").
 */
const char*
alert_method_name (alert_method_t method)
{
  switch (method)
    {
      case ALERT_METHOD_EMAIL:       return "Email";
      case ALERT_METHOD_HTTP_GET:    return "HTTP Get";
      case ALERT_METHOD_SCP:         return "SCP";
      case ALERT_METHOD_SEND:        return "Send";
      case ALERT_METHOD_SMB:         return "SMB";
      case ALERT_METHOD_SNMP:        return "SNMP";
      case ALERT_METHOD_SOURCEFIRE:  return "Sourcefire Connector";
      case ALERT_METHOD_START_TASK:  return "Start Task";
      case ALERT_METHOD_SYSLOG:      return "Syslog";
      case ALERT_METHOD_TIPPINGPOINT:return "TippingPoint SMS";
      case ALERT_METHOD_VERINICE:    return "verinice Connector";
      case ALERT_METHOD_VFIRE:       return "Alemba vFire";
      default:                       return "Internal Error";
    }
}

/**
 * @brief Get an alert condition from a name.
 *
 * @param[in]  name  Condition name.
 *
 * @return The condition.
 */
alert_condition_t
alert_condition_from_name (const char* name)
{
  if (strcasecmp (name, "Always") == 0)
    return ALERT_CONDITION_ALWAYS;
  if (strcasecmp (name, "Filter count at least") == 0)
    return ALERT_CONDITION_FILTER_COUNT_AT_LEAST;
  if (strcasecmp (name, "Filter count changed") == 0)
    return ALERT_CONDITION_FILTER_COUNT_CHANGED;
  if (strcasecmp (name, "Severity at least") == 0)
    return ALERT_CONDITION_SEVERITY_AT_LEAST;
  if (strcasecmp (name, "Severity changed") == 0)
    return ALERT_CONDITION_SEVERITY_CHANGED;
  return ALERT_CONDITION_ERROR;
}

/**
 * @brief Get an event from a name.
 *
 * @param[in]  name  Event name.
 *
 * @return The event.
 */
event_t
event_from_name (const char* name)
{
  if (strcasecmp (name, "Task run status changed") == 0)
    return EVENT_TASK_RUN_STATUS_CHANGED;
  if (strcasecmp (name, "New SecInfo arrived") == 0)
    return EVENT_NEW_SECINFO;
  if (strcasecmp (name, "Updated SecInfo arrived") == 0)
    return EVENT_UPDATED_SECINFO;
  if (strcasecmp (name, "Ticket received") == 0)
    return EVENT_TICKET_RECEIVED;
  if (strcasecmp (name, "Assigned ticket changed") == 0)
    return EVENT_ASSIGNED_TICKET_CHANGED;
  if (strcasecmp (name, "Owned ticket changed") == 0)
    return EVENT_OWNED_TICKET_CHANGED;
  return EVENT_ERROR;
}

/**
 * @brief Get an alert method from a name.
 *
 * @param[in]  name  Method name.
 *
 * @return The method.
 */
alert_method_t
alert_method_from_name (const char* name)
{
  if (strcasecmp (name, "Email") == 0)
    return ALERT_METHOD_EMAIL;
  if (strcasecmp (name, "HTTP Get") == 0)
    return ALERT_METHOD_HTTP_GET;
  if (strcasecmp (name, "SCP") == 0)
    return ALERT_METHOD_SCP;
  if (strcasecmp (name, "Send") == 0)
    return ALERT_METHOD_SEND;
  if (strcasecmp (name, "SMB") == 0)
    return ALERT_METHOD_SMB;
  if (strcasecmp (name, "SNMP") == 0)
    return ALERT_METHOD_SNMP;
  if (strcasecmp (name, "Sourcefire Connector") == 0)
    return ALERT_METHOD_SOURCEFIRE;
  if (strcasecmp (name, "Start Task") == 0)
    return ALERT_METHOD_START_TASK;
  if (strcasecmp (name, "Syslog") == 0)
    return ALERT_METHOD_SYSLOG;
  if (strcasecmp (name, "TippingPoint SMS") == 0)
    return ALERT_METHOD_TIPPINGPOINT;
  if (strcasecmp (name, "verinice Connector") == 0)
    return ALERT_METHOD_VERINICE;
  if (strcasecmp (name, "Alemba vFire") == 0)
    return ALERT_METHOD_VFIRE;
  return ALERT_METHOD_ERROR;
}


/* General task facilities. */

/**
 * @brief Get the name of a run status.
 *
 * @param[in]  status  Run status.
 *
 * @return The name of the status (for example, "Done" or "Running").
 */
const char*
run_status_name (task_status_t status)
{
  switch (status)
    {
      case TASK_STATUS_DELETE_REQUESTED:
      case TASK_STATUS_DELETE_WAITING:
        return "Delete Requested";
      case TASK_STATUS_DELETE_ULTIMATE_REQUESTED:
      case TASK_STATUS_DELETE_ULTIMATE_WAITING:
        return "Ultimate Delete Requested";
      case TASK_STATUS_DONE:             return "Done";
      case TASK_STATUS_NEW:              return "New";

      case TASK_STATUS_REQUESTED:        return "Requested";

      case TASK_STATUS_RUNNING:          return "Running";

      case TASK_STATUS_STOP_REQUESTED_GIVEUP:
      case TASK_STATUS_STOP_REQUESTED:
      case TASK_STATUS_STOP_WAITING:
        return "Stop Requested";

      case TASK_STATUS_STOPPED:          return "Stopped";
      default:                           return "Interrupted";
    }
}

/**
 * @brief Get the unique name of a run status.
 *
 * @param[in]  status  Run status.
 *
 * @return The name of the status (for example, "Done" or "Running").
 */
const char*
run_status_name_internal (task_status_t status)
{
  switch (status)
    {
      case TASK_STATUS_DELETE_REQUESTED: return "Delete Requested";
      case TASK_STATUS_DELETE_ULTIMATE_REQUESTED:
        return "Ultimate Delete Requested";
      case TASK_STATUS_DELETE_ULTIMATE_WAITING:
        return "Ultimate Delete Waiting";
      case TASK_STATUS_DELETE_WAITING:   return "Delete Waiting";
      case TASK_STATUS_DONE:             return "Done";
      case TASK_STATUS_NEW:              return "New";

      case TASK_STATUS_REQUESTED:        return "Requested";

      case TASK_STATUS_RUNNING:          return "Running";

      case TASK_STATUS_STOP_REQUESTED_GIVEUP:
      case TASK_STATUS_STOP_REQUESTED:
        return "Stop Requested";

      case TASK_STATUS_STOP_WAITING:
        return "Stop Waiting";

      case TASK_STATUS_STOPPED:          return "Stopped";
      default:                           return "Interrupted";
    }
}


/* Slave tasks. */

/* Defined in gmp.c. */
void buffer_config_preference_xml (GString *, iterator_t *, config_t, int);

/**
 * @brief Slave credential UUID.
 */
static gchar *global_slave_ssh_credential_uuid = NULL;

/**
 * @brief Slave credential UUID.
 */
static gchar *global_slave_smb_credential_uuid = NULL;

/**
 * @brief Slave credential UUID.
 */
static gchar *global_slave_esxi_credential_uuid = NULL;

/**
 * @brief Slave credential UUID.
 */
static gchar *global_slave_snmp_credential_uuid = NULL;

/**
 * @brief Slave target UUID.
 */
static gchar *global_slave_target_uuid = NULL;

/**
 * @brief Slave target UUID.
 */
static gchar *global_slave_port_list_uuid = NULL;

/**
 * @brief Slave config UUID.
 */
static gchar *global_slave_config_uuid = NULL;

/**
 * @brief Slave task UUID.
 */
static gchar *global_slave_task_uuid = NULL;

/**
 * @brief Slave report UUID.
 */
static gchar *global_slave_report_uuid = NULL;

/**
 * @brief Slave session.
 */
static gvm_connection_t *global_slave_connection = NULL;

/**
 * @brief Update the locally cached task progress from the slave.
 *
 * @param[in]  get_tasks  Slave GET_TASKS response.
 *
 * @return 0 success, -1 error.
 */
static int
update_slave_progress (entity_t get_tasks)
{
  entity_t entity;

  entity = entity_child (get_tasks, "task");
  if (entity == NULL)
    return -1;
  entity = entity_child (entity, "progress");
  if (entity == NULL)
    return -1;

  if (global_current_report == 0)
    return -1;

  set_report_slave_progress (global_current_report,
                             atoi (entity_text (entity)));

  return 0;
}

/**
 * @brief Authenticate with a slave.
 *
 * @param[in]  connection  Connection.
 *
 * @return 0 success, -1 error.
 */
static int
connection_authenticate (gvm_connection_t *connection)
{
  gmp_authenticate_info_opts_t auth_opts;

  auth_opts = gmp_authenticate_info_opts_defaults;
  auth_opts.username = connection->username;
  auth_opts.password = connection->password;
  if (gmp_authenticate_info_ext_c (connection, auth_opts))
    return -1;
  return 0;
}

/**
 * @brief Authenticate with a slave.
 *
 * @param[in]  session  GNUTLS session.
 * @param[in]  slave    Slave.
 *
 * @return 0 success, -1 error.
 */
static int
slave_authenticate (gnutls_session_t *session, scanner_t slave)
{
  int ret;
  gchar *login, *password;

  login = scanner_login (slave);
  if (login == NULL)
    return -1;

  password = scanner_password (slave);
  if (password == NULL)
    {
      g_free (login);
      return -1;
    }

  ret = gmp_authenticate (session, login, password);
  g_free (login);
  g_free (password);
  if (ret)
    return -1;
  return 0;
}

/**
 * @brief Connect to a slave.
 *
 * @param[in]  connection  Connection.
 *
 * @return 0 success, -1 error, 1 auth failure.
 */
static int
slave_connect (gvm_connection_t *connection)
{
  char *ca_cert;

  connection->tls = 1;
  if (connection->ca_cert == NULL)
    ca_cert = manage_default_ca_cert ();
  else
    ca_cert = NULL;
  connection->socket = gvm_server_open_verify
                        (&connection->session,
                         connection->host_string,
                         connection->port,
                         ca_cert ? ca_cert : connection->ca_cert,
                         connection->pub_key,
                         connection->priv_key,
                         1);
  if (connection->socket == -1)
    {
      g_warning ("%s: failed to open connection to %s on %i",
                 __FUNCTION__,
                 connection->host_string,
                 connection->port);
      return -1;
    }

  {
    int optval;
    optval = 1;
    if (setsockopt (connection->socket,
                    SOL_SOCKET, SO_KEEPALIVE,
                    &optval, sizeof (int)))
      {
        g_warning ("%s: failed to set SO_KEEPALIVE on slave socket: %s",
                   __FUNCTION__,
                   strerror (errno));
        gvm_connection_close (connection);
        return -1;
      }
  }

  g_debug ("   %s: connected", __FUNCTION__);

  /* Authenticate using the slave login. */

  if (connection_authenticate (connection))
    {
      gvm_connection_close (connection);
      return 1;
    }

  g_debug ("   %s: authenticated", __FUNCTION__);

  return 0;
}

/**
 * @brief Sleep then connect to slave.  Retry until success or giveup requested.
 *
 * @param[in]  connection  Connection.
 * @param[in]  task        Local task.
 *
 * @return 0 success, 3 giveup.
 */
static int
slave_sleep_connect (gvm_connection_t *connection, task_t task)
{
  do
    {
      if ((task_run_status (task) == TASK_STATUS_STOP_REQUESTED_GIVEUP)
          || (task_run_status (task) == TASK_STATUS_STOP_REQUESTED))
        {
          if (task_run_status (task) == TASK_STATUS_STOP_REQUESTED_GIVEUP)
            g_debug ("   %s: task stopped for giveup", __FUNCTION__);
          else
            g_debug ("   %s: task stopped", __FUNCTION__);
          set_task_run_status (current_scanner_task, TASK_STATUS_STOPPED);
          return 3;
        }
      g_debug ("   %s: sleeping for %i", __FUNCTION__,
               manage_slave_check_period ());
      gvm_sleep (manage_slave_check_period ());
    }
  while (slave_connect (connection));
  g_debug ("   %s: connected", __FUNCTION__);
  return 0;
}

/**
 * @brief Update end times, and optionally add host details.
 *
 * @param[in]  report            Report.
 *
 * @return 0 success, -1 error.
 */
static int
update_end_times (entity_t report)
{
  entity_t end;
  entities_t entities;

  /* Set the scan end time. */

  entities = report->entities;
  while ((end = first_entity (entities)))
    {
      if (strcmp (entity_name (end), "scan_end") == 0)
        {
          char *text;
          text = entity_text (end);
          while (*text && isspace (*text)) text++;
          if (*text == '\0')
            break;
          set_task_end_time (current_scanner_task,
                             g_strdup (entity_text (end)));
          set_scan_end_time (global_current_report, entity_text (end));
          break;
        }
      entities = next_entities (entities);
    }

  /* Add host details, create assets and set the host end times. */

  entities = report->entities;
  while ((end = first_entity (entities)))
    {
      if (strcmp (entity_name (end), "host") == 0)
        {
          entity_t ip, time;
          char *text;

          ip = entity_child (end, "ip");
          if (ip == NULL)
            return -1;

          time = entity_child (end, "end");
          if (time == NULL)
            return -1;

          text = entity_text (time);
          while (*text && isspace (*text)) text++;
          if ((*text != '\0')
              && (scan_host_end_time (global_current_report, entity_text (ip)) == 0))
            {
              set_scan_host_end_time (global_current_report,
                                      entity_text (ip),
                                      entity_text (time));
              if (manage_report_host_details (global_current_report,
                                              entity_text (ip),
                                              end))
                return -1;
              if (add_assets_from_host_in_report (global_current_report,
                                                  entity_text (ip)))
                return -1;
            }
        }

      entities = next_entities (entities);
    }

  return 0;
}

/**
 * @brief Cleanup slave.  Callback for atexit.
 */
static void
cleanup_slave ()
{
  if (global_slave_connection)
    {
      if (global_slave_task_uuid)
        gmp_stop_task (&global_slave_connection->session,
                       global_slave_task_uuid);
      gvm_connection_close (global_slave_connection);
    }
}

/**
 * @brief Get last report from GET_TASKS response.
 *
 * @param[in]  get_tasks  GET_TASKS response.
 *
 * @return Freshly allocated UUID of last report, or NULL.
 */
static gchar *
get_tasks_last_report (entity_t get_tasks)
{
  entity_t task;
  task = entity_child (get_tasks, "task");
  if (task)
    {
      entity_t get_tasks_current_report;
      get_tasks_current_report = entity_child (task, "current_report");
      if (global_current_report)
        {
          entity_t report;
          report = entity_child (get_tasks_current_report, "report");
          if (report && entity_attribute (report, "id"))
            return g_strdup (entity_attribute (report, "id"));
        }
      else
        {
          entity_t last_report;
          last_report = entity_child (task, "last_report");
          if (last_report)
            {
              entity_t report;
              report = entity_child (last_report, "report");
              if (report && entity_attribute (report, "id"))
                return g_strdup (entity_attribute (report, "id"));
            }
        }
    }
  return NULL;
}

/**
 * @brief Setup ID variables for slave_setup.
 *
 * @param[in]   connection  Connection to slave.
 * @param[in]   task        The task.
 * @param[in]   get_tasks   GET_TASKS response.
 * @param[out]  slave_config_uuid           UUID of slave config.
 * @param[out]  slave_target_uuid           UUID of slave target.
 * @param[out]  slave_port_list_uuid        UUID of slave port list.
 * @param[out]  slave_ssh_credential_uuid   UUID of slave SSH credential.
 * @param[out]  slave_smb_credential_uuid   UUID of slave SMB credential.
 * @param[out]  slave_esxi_credential_uuid  UUID of slave ESXi credential.
 * @param[out]  slave_snmp_credential_uuid  UUID of slave SNMP credential.
 *
 * @return 0 success, 1 giveup.
 */
static int
setup_ids (gvm_connection_t *connection, task_t task,
           entity_t get_tasks, gchar **slave_config_uuid,
           gchar **slave_target_uuid, gchar **slave_port_list_uuid,
           gchar **slave_ssh_credential_uuid, gchar **slave_smb_credential_uuid,
           gchar **slave_esxi_credential_uuid,
           gchar **slave_snmp_credential_uuid)
{
  entity_t get_tasks_task;

  assert (slave_config_uuid);
  assert (slave_target_uuid);
  assert (slave_port_list_uuid);
  assert (slave_ssh_credential_uuid);
  assert (slave_smb_credential_uuid);
  assert (slave_esxi_credential_uuid);
  assert (slave_snmp_credential_uuid);

  get_tasks_task = entity_child (get_tasks, "task");
  if (get_tasks_task)
    {
      entity_t entity;

      entity = entity_child (get_tasks_task, "config");
      if (entity && entity_attribute (entity, "id"))
        *slave_config_uuid = g_strdup (entity_attribute (entity, "id"));

      entity = entity_child (get_tasks_task, "target");
      if (entity && entity_attribute (entity, "id"))
        *slave_target_uuid = g_strdup (entity_attribute (entity, "id"));

      if (*slave_target_uuid)
        {
          entity_t get_targets;
          int ret;

          while ((ret = gmp_get_targets (&connection->session,
                                         *slave_target_uuid, 0, 0,
                                         &get_targets)))
            {
              if (ret == 404)
                {
                  /* Target missing. */
                  break;
                }
              else if (ret)
                {
                  gvm_connection_close (connection);
                  ret = slave_sleep_connect (connection, task);
                  if (ret == 3)
                    return 1;
                }
            }
          if (ret == 0)
            {
              entity_t target;
              target = entity_child (get_targets, "target");
              if (target)
                {
                  entity = entity_child (target, "port_list");
                  if (entity && entity_attribute (entity, "id"))
                    *slave_port_list_uuid = g_strdup (entity_attribute
                                                       (entity, "id"));

                  entity = entity_child (target, "ssh_credential");
                  if (entity && entity_attribute (entity, "id"))
                    *slave_ssh_credential_uuid = g_strdup (entity_attribute
                                                            (entity, "id"));

                  entity = entity_child (target, "smb_credential");
                  if (entity && entity_attribute (entity, "id"))
                    *slave_smb_credential_uuid = g_strdup (entity_attribute
                                                            (entity, "id"));

                  entity = entity_child (target, "esxi_credential");
                  if (entity && entity_attribute (entity, "id"))
                    *slave_esxi_credential_uuid = g_strdup (entity_attribute
                                                             (entity, "id"));

                  entity = entity_child (target, "snmp_credential");
                  if (entity && entity_attribute (entity, "id"))
                    *slave_snmp_credential_uuid = g_strdup (entity_attribute
                                                             (entity, "id"));
                }
              free_entity (get_targets);
            }
        }
    }
  return 0;
}

/**
 * @brief Set a task to interrupted.
 *
 * Expects global_current_report to match the task.
 *
 * @param[in]   task     Task
 * @param[in]   message  Message for error result.
 */
void
set_task_interrupted (task_t task, const gchar *message)
{
  set_task_run_status (task, TASK_STATUS_INTERRUPTED);
  if (global_current_report)
    {
      result_t result;
      result = make_result (task, "", "", "", "", "Error Message", message);
      report_add_result (global_current_report, result);
    }
}

/**
 * @brief Setup a task on a slave.
 *
 * @param[in]   connection  Connection to slave.
 * @param[in]   name        Name of task on slave.
 * @param[in]   task        The task.
 * @param[out]  target      Task target.
 * @param[out]  target_ssh_credential    Target SSH credential.
 * @param[out]  target_smb_credential    Target SMB credential.
 * @param[out]  target_esxi_credential    Target ESXi credential.
 * @param[out]  target_snmp_credential    Target SNMP credential.
 * @param[out]  last_stopped_report  Last stopped report if any, else 0.
 *
 * @return 0 success, 1 retry, 3 giveup.
 */
static int
slave_setup (gvm_connection_t *connection, const char *name, task_t task,
             target_t target, credential_t target_ssh_credential,
             credential_t target_smb_credential,
             credential_t target_esxi_credential,
             credential_t target_snmp_credential,
             report_t last_stopped_report)
{
  const int ret_giveup = 3;
  int ret_fail, next_result;
  iterator_t credentials, targets;
  gmp_delete_opts_t del_opts;

  ret_fail = 1;
  del_opts = gmp_delete_opts_ultimate_defaults;
  global_slave_connection = connection;

  /* Register a cleanup callback to stop the slave task if the process is
   * killed, for example by a reboot.  On restart Manager will set the task
   * status to Stopped, which will match the slave. */
  if (atexit (&cleanup_slave))
    {
      g_critical ("%s: failed to register `atexit' slave_cleanup function",
                  __FUNCTION__);
      goto fail;
    }

  if (last_stopped_report)
    {
      /* Resume the task on the slave. */

      global_slave_task_uuid = report_slave_task_uuid (last_stopped_report);
      if (global_slave_task_uuid == NULL)
        {
          /* This may happen if someone sets a slave on a local task.  Clear
           * all the report results and start the task from the beginning.  */
          trim_report (last_stopped_report);
          last_stopped_report = 0;
        }
      else
        {
          int ret;
          entity_t get_tasks;
          const char *status;

          /* Check if the task is running or complete on the slave. */

          while ((ret = gmp_get_tasks (&connection->session, global_slave_task_uuid,
                                       0, 0, &get_tasks)))
            {
              if (ret == 404)
                {
                  /* Task missing.  Perhaps someone removed the task on the slave.
                   * Clear all the report results and start the task from the
                   * beginning. */
                  trim_report (last_stopped_report);
                  last_stopped_report = 0;
                  break;
                }
              else if (ret)
                {
                  gvm_connection_close (connection);
                  ret = slave_sleep_connect (connection, task);
                  if (ret == 3)
                    goto giveup;
                }
            }

          if (ret == 0)
            {
              status = gmp_task_status (get_tasks);
              if (status == NULL)
                {
                  /* An error somewhere.  Clear all the report results and
                   * start the task from the beginning. */
                  trim_report (last_stopped_report);
                  last_stopped_report = 0;
                }
              else if ((strcmp (status, "Running") == 0)
                       || (strcmp (status, "Done") == 0))
                {
                  /* Task on slave is Running or Done, continue using it as
                   * is. */

                  global_slave_report_uuid = get_tasks_last_report (get_tasks);
                  if (global_slave_report_uuid == NULL)
                    {
                      g_warning ("%s: slave report %s missing UUID", __FUNCTION__,
                                 global_slave_task_uuid);
                      goto fail;
                    }

                  setup_ids (connection, task,
                             get_tasks, &global_slave_config_uuid,
                             &global_slave_target_uuid,
                             &global_slave_port_list_uuid,
                             &global_slave_ssh_credential_uuid,
                             &global_slave_smb_credential_uuid,
                             &global_slave_esxi_credential_uuid,
                             &global_slave_snmp_credential_uuid);
                }
              else
                {
                  /* Task is there, try resume it. */
                  switch (gmp_resume_task_report (&connection->session,
                                                  global_slave_task_uuid,
                                                  &global_slave_report_uuid))
                    {
                      case 0:
                        if (global_slave_report_uuid == NULL)
                          goto fail;
                        setup_ids (connection, task,
                                   get_tasks, &global_slave_config_uuid,
                                   &global_slave_target_uuid,
                                   &global_slave_port_list_uuid,
                                   &global_slave_ssh_credential_uuid,
                                   &global_slave_smb_credential_uuid,
                                   &global_slave_esxi_credential_uuid,
                                   &global_slave_snmp_credential_uuid);
                        set_task_run_status (task, TASK_STATUS_REQUESTED);
                        break;
                      case 1:
                        /* The resume may have failed because the task slave changed or
                         * because someone removed the task on the slave.  Clear all the
                         * report results and start the task from the beginning.
                         *
                         * This and the if above both "leak" the resources on the slave,
                         * because on the report these resources are replaced with the new
                         * resources. */
                        trim_report (last_stopped_report);
                        last_stopped_report = 0;
                        break;
                      default:
                        free (global_slave_task_uuid);
                        goto fail;
                    }
                }
            }
        }
    }

  if (last_stopped_report == 0)
    {
      /* Create the target credentials on the slave. */

      if (target_ssh_credential)
        {
          init_credential_iterator_one (&credentials,
                                            target_ssh_credential);
          if (next (&credentials))
            {
              int ret;
              const char *user, *password, *private_key;
              gchar *user_copy, *password_copy, *private_key_copy;
              gmp_create_lsc_credential_opts_t opts;

              user = credential_iterator_login (&credentials);
              password = credential_iterator_password (&credentials);
              private_key = credential_iterator_private_key (&credentials);

              if (user == NULL
                  || (private_key == NULL && password == NULL))
                {
                  cleanup_iterator (&credentials);
                  global_slave_ssh_credential_uuid = NULL;
                  g_warning ("Could not create slave SSH credential"
                             " (needs login and password or private key)."
                             " Continuing without credential.");
                }
              else
                {
                  user_copy = g_strdup (user);
                  password_copy = g_strdup (password);

                  opts = gmp_create_lsc_credential_opts_defaults;
                  opts.name = name;
                  opts.login = user_copy;
                  opts.passphrase = password_copy;
                  if (private_key)
                    {
                      private_key_copy = g_strdup (private_key);
                      opts.private_key = private_key_copy;
                    }
                  else
                    private_key_copy = NULL;
                  opts.comment = "Slave SSH credential created by Master";

                  cleanup_iterator (&credentials);

                  ret = gmp_create_lsc_credential_ext
                         (&connection->session,
                          opts,
                          &global_slave_ssh_credential_uuid);

                  g_free (user_copy);
                  g_free (password_copy);
                  g_free (private_key_copy);

                  if (ret)
                    {
                      g_warning ("Could not create slave SSH credential"
                                 " (status %d)."
                                 " Continuing without credential.",
                                 ret);
                      global_slave_ssh_credential_uuid = NULL;
                    }
                }
            }
        }

      if (target_smb_credential)
        {
          init_credential_iterator_one (&credentials,
                                        target_smb_credential);
          if (next (&credentials))
            {
              int ret;
              const char *user, *password;
              gchar *user_copy, *password_copy, *smb_name;
              gmp_create_lsc_credential_opts_t opts;

              user = credential_iterator_login (&credentials);
              password = credential_iterator_password (&credentials);

              if (user == NULL || password == NULL)
                {
                  cleanup_iterator (&credentials);
                  global_slave_smb_credential_uuid = NULL;
                  g_warning ("Could not create slave SMB credential"
                             " (missing login or password)."
                             " Continuing without credential.");
                }
              else
                {
                  user_copy = g_strdup (user);
                  password_copy = g_strdup (password);

                  opts = gmp_create_lsc_credential_opts_defaults;
                  smb_name = g_strdup_printf ("%ssmb", name);
                  opts.name = smb_name;
                  opts.login = user_copy;
                  opts.passphrase = password_copy;
                  opts.comment = "Slave SMB credential created by Master";

                  cleanup_iterator (&credentials);

                  ret = gmp_create_lsc_credential_ext
                           (&connection->session,
                            opts,
                            &global_slave_smb_credential_uuid);

                  g_free (smb_name);
                  g_free (user_copy);
                  g_free (password_copy);

                  if (ret)
                    {
                      g_warning ("Could not create slave SMB credential"
                                 " (status %d)."
                                 " Continuing without credential.",
                                 ret);
                      global_slave_smb_credential_uuid = NULL;
                    }
                }
            }
        }

      if (target_esxi_credential)
        {
          init_credential_iterator_one (&credentials,
                                        target_esxi_credential);
          if (next (&credentials))
            {
              int ret;
              const char *user, *password;
              gchar *user_copy, *password_copy, *esxi_name;
              gmp_create_lsc_credential_opts_t opts;

              user = credential_iterator_login (&credentials);
              password = credential_iterator_password (&credentials);

              if (user == NULL || password == NULL)
                {
                  cleanup_iterator (&credentials);
                  global_slave_esxi_credential_uuid = NULL;
                  g_warning ("Could not create slave ESXi credential"
                             " (missing login or password)."
                             " Continuing without credential.");
                }
              else
                {
                  user_copy = g_strdup (user);
                  password_copy = g_strdup (password);

                  opts = gmp_create_lsc_credential_opts_defaults;
                  esxi_name = g_strdup_printf ("%sesxi", name);
                  opts.name = esxi_name;
                  opts.login = user_copy;
                  opts.passphrase = password_copy;
                  opts.comment = "Slave ESXi credential created by Master";

                  cleanup_iterator (&credentials);

                  ret = gmp_create_lsc_credential_ext
                           (&connection->session,
                            opts,
                            &global_slave_esxi_credential_uuid);

                  g_free (esxi_name);
                  g_free (user_copy);
                  g_free (password_copy);
                  if (ret)
                    {
                      g_warning ("Could not create slave ESXi credential"
                                 " (status %d)."
                                 " Continuing without credential.",
                                 ret);
                      global_slave_esxi_credential_uuid = NULL;
                    }
                }
            }
        }

      if (target_snmp_credential)
        {
          init_credential_iterator_one (&credentials,
                                        target_snmp_credential);
          if (next (&credentials))
            {
              int ret;
              const char *community, *user, *password, *auth_algorithm;
              const char *privacy_password, *privacy_algorithm;
              gchar *community_copy, *user_copy, *password_copy;
              gchar *auth_algorithm_copy, *privacy_password_copy;
              gchar *privacy_algorithm_copy, *snmp_name;
              gmp_create_lsc_credential_opts_t opts;

              community = credential_iterator_community (&credentials);
              user = credential_iterator_login (&credentials);
              password = credential_iterator_password (&credentials);
              auth_algorithm
                = credential_iterator_auth_algorithm (&credentials);
              privacy_password
                = credential_iterator_privacy_password (&credentials);
              privacy_algorithm
                = credential_iterator_privacy_algorithm (&credentials);

              if (password && strcmp (password, "")
                  && (auth_algorithm == NULL || strcmp (auth_algorithm, "")))
                {
                  cleanup_iterator (&credentials);
                  global_slave_snmp_credential_uuid = NULL;
                  g_warning ("Could not create slave SNMP credential"
                             " (auth_algorithm must be set if password is"
                             " given)."
                             " Continuing without credential.");
                }
              else if (((privacy_password && strcmp (privacy_password, ""))
                        || (privacy_algorithm
                            && strcmp (privacy_algorithm, "")))
                       && (password == NULL
                           || auth_algorithm == NULL
                           || privacy_password == NULL
                           || privacy_algorithm == NULL))
                {
                  cleanup_iterator (&credentials);
                  global_slave_snmp_credential_uuid = NULL;
                  g_warning ("Could not create slave SNMP credential"
                             " (password, auth_algorithm, privacy_password"
                             " and privacy_algorithm are mandatory if"
                             " privacy_password or privacy_algorithm are"
                             " given)."
                             " Continuing without credential.");
                }
              else
                {
                  community_copy
                      = g_strdup (community ? community : "");
                  user_copy
                      = g_strdup (user ? user : "");
                  password_copy
                      = g_strdup (password ? password : "");
                  auth_algorithm_copy
                      = g_strdup (auth_algorithm ? auth_algorithm : "");
                  privacy_password_copy
                      = g_strdup (privacy_password ? privacy_password : "");
                  privacy_algorithm_copy
                      = g_strdup (privacy_algorithm ? privacy_algorithm : "");

                  opts = gmp_create_lsc_credential_opts_defaults;
                  snmp_name = g_strdup_printf ("%ssnmp", name);
                  opts.name = snmp_name;
                  opts.community = community_copy;
                  opts.login = user_copy;
                  opts.passphrase = password_copy;
                  opts.auth_algorithm = auth_algorithm_copy;
                  opts.privacy_password = privacy_password_copy;
                  opts.privacy_algorithm = privacy_algorithm_copy;
                  opts.comment = "Slave SNMP credential created by Master";

                  cleanup_iterator (&credentials);

                  ret = gmp_create_lsc_credential_ext
                           (&connection->session,
                            opts,
                            &global_slave_snmp_credential_uuid);

                  g_free (snmp_name);
                  g_free (community_copy);
                  g_free (user_copy);
                  g_free (password_copy);
                  g_free (auth_algorithm_copy);
                  g_free (privacy_password_copy);
                  g_free (privacy_algorithm_copy);
                  if (ret)
                    {
                      g_warning ("Could not create slave SNMP credential"
                                 " (status %d)."
                                 " Continuing without credential",
                                 ret);
                      global_slave_snmp_credential_uuid = NULL;
                    }
                }
            }
        }

      g_debug ("   %s: slave SSH credential uuid: %s", __FUNCTION__,
               global_slave_ssh_credential_uuid);

      g_debug ("   %s: slave SMB credential uuid: %s", __FUNCTION__,
               global_slave_smb_credential_uuid);

      g_debug ("   %s: slave ESXi credential uuid: %s", __FUNCTION__,
               global_slave_esxi_credential_uuid);

      g_debug ("   %s: slave SNMP credential uuid: %s", __FUNCTION__,
               global_slave_snmp_credential_uuid);

      /* Create the target on the slave. */

      init_target_iterator_one (&targets, target);
      if (next (&targets))
        {
          int ret;
          const char *hosts, *port, *exclude_hosts, *alive_tests;
          const char *reverse_lookup_only, *reverse_lookup_unify;
          const char *port_list_uuid;
          gchar *hosts_copy, *exclude_hosts_copy;
          gchar *alive_tests_copy, *port_range;
          gmp_create_target_opts_t opts;
          int ssh_port;
          entity_t get_targets, child;

          hosts = target_iterator_hosts (&targets);
          exclude_hosts = target_iterator_exclude_hosts (&targets);
          alive_tests = target_iterator_alive_tests (&targets);
          reverse_lookup_only
            = target_iterator_reverse_lookup_only (&targets);
          reverse_lookup_unify
            = target_iterator_reverse_lookup_unify (&targets);
          if (hosts == NULL)
            {
              cleanup_iterator (&targets);
              goto fail_credentials;
            }

          port = target_iterator_ssh_port (&targets);
          if (port == NULL)
            ssh_port = 0;
          else
            ssh_port = atoi (port);

          hosts_copy = g_strdup (hosts);
          exclude_hosts_copy = g_strdup (exclude_hosts);
          alive_tests_copy = g_strdup (alive_tests);
          port_range = target_port_range (get_iterator_resource (&targets));
          cleanup_iterator (&targets);

          opts = gmp_create_target_opts_defaults;
          opts.hosts = hosts_copy;
          opts.exclude_hosts = exclude_hosts_copy;
          opts.alive_tests = alive_tests_copy;
          opts.ssh_credential_id = global_slave_ssh_credential_uuid;
          opts.ssh_credential_port = ssh_port;
          opts.smb_credential_id = global_slave_smb_credential_uuid;
          opts.esxi_credential_id = global_slave_esxi_credential_uuid;
          opts.snmp_credential_id = global_slave_snmp_credential_uuid;
          opts.port_range = port_range;
          opts.name = name;
          opts.comment = "Slave target created by Master";
          opts.reverse_lookup_only
            = reverse_lookup_only ? atoi (reverse_lookup_only) : 0;
          opts.reverse_lookup_unify
            = reverse_lookup_unify ? atoi (reverse_lookup_unify) : 0;

          ret = gmp_create_target_ext (&connection->session, opts,
                                       &global_slave_target_uuid);
          g_free (hosts_copy);
          g_free (exclude_hosts_copy);
          g_free (alive_tests_copy);
          g_free (port_range);
          if (ret == -2)
            goto fail_credentials;
          if (ret)
            {
              set_task_interrupted (task,
                                    "Failed to create target on slave."
                                    "  Interrupting scan.");
              ret_fail = ret_giveup;
              goto fail_credentials;
            }

          if (gmp_get_targets (&connection->session, global_slave_target_uuid,
                               0, 0, &get_targets))
            goto fail_target;
          child = entity_child (get_targets, "target");
          if (child == NULL)
            {
              free_entity (get_targets);
              goto fail_target;
            }
          child = entity_child (child, "port_list");
          if (child == NULL)
            {
              free_entity (get_targets);
              goto fail_target;
            }
          port_list_uuid = entity_attribute (child, "id");
          if (port_list_uuid == NULL)
            {
              free_entity (get_targets);
              goto fail_target;
            }
          global_slave_port_list_uuid = g_strdup (port_list_uuid);
          free_entity (get_targets);
        }
      else
        {
          cleanup_iterator (&targets);
          goto fail_credentials;
        }

      g_debug ("   %s: slave target uuid: %s",
               __FUNCTION__,
               global_slave_target_uuid);

      /* Create the config on the slave. */

      {
        config_t config;
        iterator_t prefs, selectors;

        /* This must follow the GET_CONFIGS_RESPONSE export case. */

        config = task_config (task);
        if (config == 0)
          goto fail_target;

        if (gvm_server_sendf (&connection->session,
                              "<create_config>"
                              "<get_configs_response"
                              " status=\"200\""
                              " status_text=\"OK\">"
                              "<config id=\"XXX\">"
                              "<type>0</type>"
                              "<name>%s</name>"
                              "<comment>"
                              "Slave config created by Master"
                              "</comment>"
                              "<preferences>",
                              name))
          goto fail_target;

        /* Send NVT timeout preferences where a timeout has been
         * specified. */
        init_config_timeout_iterator (&prefs, config);
        while (next (&prefs))
          {
            const char *timeout;

            timeout = config_timeout_iterator_value (&prefs);

            if (timeout && strlen (timeout)
                && gvm_server_sendf (&connection->session,
                                     "<preference>"
                                     "<nvt oid=\"%s\">"
                                     "<name>%s</name>"
                                     "</nvt>"
                                     "<name>Timeout</name>"
                                     "<id>0</id>"
                                     "<type>entry</type>"
                                     "<value>%s</value>"
                                     "</preference>",
                                     config_timeout_iterator_oid (&prefs),
                                     config_timeout_iterator_nvt_name (&prefs),
                                     timeout))
              {
                cleanup_iterator (&prefs);
                goto fail_target;
              }
          }
        cleanup_iterator (&prefs);

        init_nvt_preference_iterator (&prefs, NULL);
        while (next (&prefs))
          {
            GString *buffer = g_string_new ("");
            buffer_config_preference_xml (buffer, &prefs, config, 0);
            if (gvm_server_sendf (&connection->session, "%s", buffer->str))
              {
                cleanup_iterator (&prefs);
                goto fail_target;
              }
            g_string_free (buffer, TRUE);
          }
        cleanup_iterator (&prefs);

        if (gvm_server_sendf (&connection->session,
                              "</preferences>"
                              "<nvt_selectors>"))
          {
            cleanup_iterator (&prefs);
            goto fail_target;
          }

        init_nvt_selector_iterator (&selectors,
                                    NULL,
                                    config,
                                    NVT_SELECTOR_TYPE_ANY);
        while (next (&selectors))
          {
            int type = nvt_selector_iterator_type (&selectors);
            if (gvm_server_sendf
                 (&connection->session,
                  "<nvt_selector>"
                  "<name>%s</name>"
                  "<include>%i</include>"
                  "<type>%i</type>"
                  "<family_or_nvt>%s</family_or_nvt>"
                  "</nvt_selector>",
                  nvt_selector_iterator_name (&selectors),
                  nvt_selector_iterator_include (&selectors),
                  type,
                  (type == NVT_SELECTOR_TYPE_ALL
                    ? ""
                    : nvt_selector_iterator_nvt (&selectors))))
              goto fail_target;
          }
        cleanup_iterator (&selectors);

        if (gvm_server_sendf (&connection->session,
                              "</nvt_selectors>"
                              "</config>"
                              "</get_configs_response>"
                              "</create_config>")
            || (gmp_read_create_response (&connection->session,
                                          &global_slave_config_uuid)
                != 201))
          goto fail_target;
      }

      g_debug ("   %s: slave config uuid: %s",
               __FUNCTION__,
               global_slave_config_uuid);

      /* Create the task on the slave. */

      {
        int ret;
        gchar *max_checks, *max_hosts, *source_iface;
        gchar *hosts_ordering, *comment;
        gmp_create_task_opts_t opts;
        char *name_task, *uuid_report, *uuid_task;

        opts = gmp_create_task_opts_defaults;
        opts.config_id = global_slave_config_uuid;
        opts.target_id = global_slave_target_uuid;
        opts.name = name;
        task_uuid (task, &uuid_task);
        name_task = task_name (task);
        uuid_report = report_uuid (global_current_report);
        comment = g_strdup_printf ("Slave task for master task %s (%s)"
                                   " report %s.",
                                   name_task,
                                   uuid_task,
                                   uuid_report);
        opts.comment = comment;
        free (uuid_task);
        free (name_task);
        free (uuid_report);

        max_checks = task_preference_value (task, "max_checks");
        max_hosts = task_preference_value (task, "max_hosts");
        source_iface = task_preference_value (task, "source_iface");
        hosts_ordering = task_hosts_ordering (task);

        opts.alterable = 0;
        opts.in_assets = "no";
        opts.max_checks = max_checks ? max_checks : MAX_CHECKS_DEFAULT;
        opts.max_hosts = max_hosts ? max_hosts : MAX_HOSTS_DEFAULT;
        opts.source_iface = source_iface;
        opts.hosts_ordering = hosts_ordering;

        opts.alert_ids = NULL;
        opts.observers = NULL;
        opts.observer_groups = NULL;
        opts.scanner_id = NULL;
        opts.schedule_id = NULL;
        opts.slave_id = NULL;

        ret = gmp_create_task_ext (&connection->session,
                                   opts,
                                   &global_slave_task_uuid);
        g_free (comment);
        g_free (max_checks);
        g_free (max_hosts);
        g_free (source_iface);
        g_free (hosts_ordering);
        if (ret)
          goto fail_config;
      }

      /* Start the task on the slave. */

      if (gmp_start_task_report (&connection->session,
                                 global_slave_task_uuid,
                                 &global_slave_report_uuid))
        goto fail_task;
      if (global_slave_report_uuid == NULL)
        goto fail_stop_task;

      set_report_slave_task_uuid (global_current_report, global_slave_task_uuid);
    }

  /* Setup the current task for functions like set_task_run_status. */

  current_scanner_task = task;

  /* Poll the slave until the task is finished. */

  next_result = 1;
  while (1)
    {
      entity_t get_tasks, report, get_report;
      const char *status;
      task_status_t run_status;
      int ret, status_done;

      /* Check if some other process changed the task status. */

      run_status = task_run_status (task);
      switch (run_status)
        {
          case TASK_STATUS_DELETE_REQUESTED:
          case TASK_STATUS_DELETE_ULTIMATE_REQUESTED:
          case TASK_STATUS_STOP_REQUESTED:
            switch (gmp_stop_task (&connection->session,
                                   global_slave_task_uuid))
              {
                case 0:
                  break;
                case 404:
                  /* Resource Missing. */
                  set_task_interrupted (task,
                                        "Failed to find task on slave."
                                        "  Interrupting scan.");
                  goto giveup;
                default:
                  goto fail_stop_task;
              }
            if (run_status == TASK_STATUS_DELETE_REQUESTED)
              set_task_run_status (current_scanner_task,
                                   TASK_STATUS_DELETE_WAITING);
            else if (run_status == TASK_STATUS_DELETE_ULTIMATE_REQUESTED)
              set_task_run_status (current_scanner_task,
                                   TASK_STATUS_DELETE_ULTIMATE_WAITING);
            else
              set_task_run_status (current_scanner_task,
                                   TASK_STATUS_STOP_WAITING);
            break;
          case TASK_STATUS_STOP_REQUESTED_GIVEUP:
            g_debug ("   %s: task stopped for giveup", __FUNCTION__);
            set_task_run_status (current_scanner_task, TASK_STATUS_STOPPED);
            goto giveup;
            break;
          case TASK_STATUS_STOPPED:
            assert (0);
            goto fail_stop_task;
            break;
          case TASK_STATUS_DELETE_WAITING:
          case TASK_STATUS_DELETE_ULTIMATE_WAITING:
          case TASK_STATUS_DONE:
          case TASK_STATUS_NEW:
          case TASK_STATUS_REQUESTED:
          case TASK_STATUS_RUNNING:
          case TASK_STATUS_STOP_WAITING:
          case TASK_STATUS_INTERRUPTED:
            break;
        }

      ret = gmp_get_tasks (&connection->session, global_slave_task_uuid, 0, 0,
                           &get_tasks);
      if (ret == 404)
        {
          /* Resource Missing. */
          set_task_interrupted (task,
                                "Task missing on slave."
                                "  Interrupting scan.");
          goto giveup;
        }
      else if (ret)
        {
          gvm_connection_close (connection);
          ret = slave_sleep_connect (connection, task);
          if (ret == 3)
            goto giveup;
          continue;
        }

      status = gmp_task_status (get_tasks);
      if (status == NULL)
        {
          set_task_interrupted (task,
                                "Error in slave task status."
                                "  Interrupting scan.");
          goto giveup;
        }
      status_done = (strcmp (status, "Done") == 0);
      if ((strcmp (status, "Running") == 0)
          || status_done)
        {
          int ret2 = 0;
          gmp_get_report_opts_t opts;

          if (run_status == TASK_STATUS_REQUESTED)
            set_task_run_status (task, TASK_STATUS_RUNNING);

          if ((strcmp (status, "Running") == 0)
              && update_slave_progress (get_tasks))
            {
              free_entity (get_tasks);
              goto fail_stop_task;
            }

          opts = gmp_get_report_opts_defaults;
          opts.report_id = global_slave_report_uuid;
          opts.format_id = "a994b278-1f62-11e1-96ac-406186ea4fc5";
          opts.filter = g_strdup_printf
                         ("first=%i rows=-1 levels=hmlgd apply_overrides=0"
                          " min_qod=0 autofp=0 result_hosts_only=%i"
                          " sort=created",
                          next_result,
                          status_done
                           /* Request all the hosts to get their end times. */
                           ? 0
                           : 1);

          ret = gmp_get_report_ext (&connection->session, opts, &get_report);
          if (ret)
            {
              opts.format_id = "d5da9f67-8551-4e51-807b-b6a873d70e34";
              ret2 = gmp_get_report_ext (&connection->session, opts,
                                         &get_report);
            }
          g_free (opts.filter);
          if ((ret == 404) && (ret2 == 404))
            {
              /* Resource Missing. */
              set_task_interrupted (task,
                                    "Task report missing on slave."
                                    "  Interrupting scan.");
              goto giveup;
            }
          if (ret && ret2)
            {
              free_entity (get_tasks);
              gvm_connection_close (connection);
              ret = slave_sleep_connect (connection, task);
              if (ret == 3)
                goto giveup;
              continue;
            }

          if (update_from_slave (task, get_report, &report, &next_result))
            {
              free_entity (get_tasks);
              free_entity (get_report);
              goto fail_stop_task;
            }

          if (strcmp (status, "Running") == 0)
            {
              if (update_end_times (report))
                goto fail_stop_task;
              free_entity (get_report);
            }
        }
      else if (strcmp (status, "Stopped") == 0)
        {
          set_task_run_status (task, TASK_STATUS_STOPPED);
          goto succeed_stopped;
        }
      else if (strcmp (status, "Stop Requested") == 0)
        set_task_run_status (task, TASK_STATUS_STOP_WAITING);
      else if ((strcmp (status, "Interrupted") == 0)
               /* Keeping this in case the slave is older. */
               || (strcmp (status, "Internal Error") == 0)
               || (strcmp (status, "Delete Requested") == 0))
        {
          free_entity (get_tasks);
          goto fail_stop_task;
        }

      if (status_done)
        {
          if (update_end_times (report))
            {
              free_entity (get_tasks);
              free_entity (get_report);
              goto fail_stop_task;
            }
          free_entity (get_report);
          if (update_slave_progress (get_tasks))
            {
              free_entity (get_report);
              free_entity (get_tasks);
              goto fail_stop_task;
            }
          free_entity (get_tasks);

          /* Add results to assets. */
          if (global_current_report)
            {
              hosts_set_identifiers (global_current_report);
              hosts_set_max_severity (global_current_report, NULL, NULL);
              hosts_set_details (global_current_report);
            }

          set_task_run_status (task, TASK_STATUS_DONE);
          break;
        }

      free_entity (get_tasks);

      g_debug ("   %s: sleeping for %i\n", __FUNCTION__,
               manage_slave_check_period ());
      gvm_sleep (manage_slave_check_period ());
    }

  /* Cleanup. */

  current_scanner_task = (task_t) 0;

  gmp_delete_task_ext (&connection->session,
                       global_slave_task_uuid,
                       del_opts);
  set_report_slave_task_uuid (global_current_report, "");
  gmp_delete_config_ext (&connection->session,
                         global_slave_config_uuid,
                         del_opts);
  gmp_delete_target_ext (&connection->session,
                         global_slave_target_uuid,
                         del_opts);
  gmp_delete_port_list_ext (&connection->session,
                            global_slave_port_list_uuid,
                            del_opts);
  if (global_slave_ssh_credential_uuid)
    gmp_delete_lsc_credential_ext (&connection->session,
                                   global_slave_ssh_credential_uuid,
                                   del_opts);
  if (global_slave_smb_credential_uuid)
    gmp_delete_lsc_credential_ext (&connection->session,
                                   global_slave_smb_credential_uuid,
                                   del_opts);
  if (global_slave_esxi_credential_uuid)
    gmp_delete_lsc_credential_ext (&connection->session,
                                   global_slave_esxi_credential_uuid,
                                   del_opts);
  if (global_slave_snmp_credential_uuid)
    gmp_delete_lsc_credential_ext (&connection->session,
                                   global_slave_snmp_credential_uuid,
                                   del_opts);
 succeed_stopped:
  free (global_slave_task_uuid);
  global_slave_task_uuid = NULL;
  free (global_slave_report_uuid);
  global_slave_report_uuid = NULL;
  free (global_slave_config_uuid);
  global_slave_config_uuid = NULL;
  free (global_slave_target_uuid);
  global_slave_target_uuid = NULL;
  free (global_slave_port_list_uuid);
  global_slave_port_list_uuid = NULL;
  free (global_slave_snmp_credential_uuid);
  global_slave_snmp_credential_uuid = NULL;
  free (global_slave_esxi_credential_uuid);
  global_slave_esxi_credential_uuid = NULL;
  free (global_slave_smb_credential_uuid);
  global_slave_smb_credential_uuid = NULL;
  free (global_slave_ssh_credential_uuid);
  global_slave_ssh_credential_uuid = NULL;
  gvm_connection_close (connection);
  global_slave_connection = NULL;
  g_debug ("   %s: succeed", __FUNCTION__);
  return 0;

 fail_stop_task:
  gmp_stop_task (&connection->session,
                 global_slave_task_uuid);
  free (global_slave_report_uuid);
 fail_task:
  gmp_delete_task_ext (&connection->session,
                       global_slave_task_uuid,
                       del_opts);
  set_report_slave_task_uuid (global_current_report, "");
  free (global_slave_task_uuid);
 fail_config:
  gmp_delete_config_ext (&connection->session,
                         global_slave_config_uuid,
                         del_opts);
  free (global_slave_config_uuid);
 fail_target:
  gmp_delete_target_ext (&connection->session,
                         global_slave_target_uuid,
                         del_opts);
  free (global_slave_target_uuid);
  gmp_delete_port_list_ext (&connection->session,
                            global_slave_port_list_uuid,
                            del_opts);
  free (global_slave_port_list_uuid);
 fail_credentials:
  if (global_slave_snmp_credential_uuid)
    gmp_delete_lsc_credential_ext (&connection->session,
                                   global_slave_snmp_credential_uuid,
                                   del_opts);
  free (global_slave_snmp_credential_uuid);
  if (global_slave_esxi_credential_uuid)
    gmp_delete_lsc_credential_ext (&connection->session,
                                   global_slave_esxi_credential_uuid,
                                   del_opts);
  free (global_slave_esxi_credential_uuid);
  if (global_slave_smb_credential_uuid)
    gmp_delete_lsc_credential_ext (&connection->session,
                                   global_slave_smb_credential_uuid,
                                   del_opts);
  free (global_slave_smb_credential_uuid);
  if (global_slave_ssh_credential_uuid)
    gmp_delete_lsc_credential_ext (&connection->session,
                                   global_slave_ssh_credential_uuid,
                                   del_opts);
  free (global_slave_ssh_credential_uuid);
 fail:
  g_debug ("   %s: fail (%i)", __FUNCTION__, ret_fail);
  gvm_connection_close (connection);
  global_slave_connection = NULL;
  return ret_fail;

 giveup:
  g_debug ("   %s: giveup (%i)", __FUNCTION__, ret_giveup);
  gvm_connection_close (connection);
  global_slave_connection = NULL;
  return ret_giveup;
}

/**
 * @brief Start a task on a slave.
 *
 * @param[in]  task                     The task.
 * @param[in]  target                   Task target.
 * @param[in]  target_ssh_credential    Target SSH credential.
 * @param[in]  target_smb_credential    Target SMB credential.
 * @param[in]  target_esxi_credential   Target ESXi credential.
 * @param[in]  target_snmp_credential   Target SNMP credential.
 * @param[in]  last_stopped_report      Last stopped report if any, else 0.
 * @param[in]  connection               Connection, with slave info.
 * @param[in]  slave_id                 UUID of slave.
 * @param[in]  slave_name               Name of slave.
 *
 * @return 0 success, 1 login failed, -1 failed to make UUID, -2 Failed to get
 *         task name.
 */
static int
handle_slave_task (task_t task, target_t target,
                   credential_t target_ssh_credential,
                   credential_t target_smb_credential,
                   credential_t target_esxi_credential,
                   credential_t target_snmp_credential,
                   report_t last_stopped_report,
                   gvm_connection_t *connection,
                   const gchar *slave_id,
                   const gchar *slave_name)
{
  char *name, *uuid;
  int ret;
  gchar *slave_task_name;

  /* Some of the cases in here must write to the session outside an open
   * statement.  For example, the gmp_create_lsc_credential must come after
   * cleaning up the credential iterator.  This is because the slave may be
   * the master, and the open statement would prevent the slave from getting
   * a lock on the database and fulfilling the request. */

  g_debug ("   Running slave task %llu", task);

  // FIX permission checks  may the user still access the slave, target, port list etc?

  report_set_slave_uuid (global_current_report, slave_id);
  report_set_slave_name (global_current_report, slave_name);
  report_set_slave_port (global_current_report, connection->port);
  report_set_slave_host (global_current_report, connection->host_string);

  uuid = gvm_uuid_make ();
  if (uuid == NULL)
    {
      g_warning ("%s: Failed to make UUID", __FUNCTION__);
      return -1;
    }

  name = task_name (task);
  if (name == NULL)
    {
      free (uuid);
      g_warning ("%s: Failed to get task name", __FUNCTION__);
      return -2;
    }
  slave_task_name = g_strdup_printf ("%s for %s", uuid, name);
  free (name);
  free (uuid);

  while ((ret = slave_connect (connection)))
    if (ret == 1)
      {
        result_t result;
        gchar *port_string;

        /* Login failed. */

        port_string = g_strdup_printf ("%i", connection->port);
        result = make_result (task,
                              connection->host_string,
                              "",
                              port_string,
                              /* NVT: Global variable settings. */
                              OID_GLOBAL_SETTINGS,
                              "Error Message",
                              "Authentication with the slave failed.");
        g_free (port_string);
        if (global_current_report)
          report_add_result (global_current_report, result);

        g_free (slave_task_name);
        return 1;
      }
    else
      {
        int current_signal = get_termination_signal ();
        if ((task_run_status (task) == TASK_STATUS_STOP_REQUESTED_GIVEUP)
            || (task_run_status (task) == TASK_STATUS_STOP_REQUESTED)
            || current_signal)
          {
            if (current_signal)
              {
                g_debug ("%s: Received %s signal.",
                         __FUNCTION__,
                         sys_siglist[get_termination_signal()]);
              }
            if (global_current_report)
              {
                set_report_scan_run_status (global_current_report,
                                            TASK_STATUS_STOPPED);
              }
            set_task_run_status (task, TASK_STATUS_STOPPED);
            g_free (slave_task_name);
            return 0;
          }
        g_debug ("   %s: sleeping for %i\n", __FUNCTION__,
                 manage_slave_check_period ());
        gvm_sleep (manage_slave_check_period ());
      }

  while (1)
    {
      if (get_termination_signal ())
        {
          g_debug ("%s: Received %s signal.",
                   __FUNCTION__,
                   sys_siglist[get_termination_signal()]);
          if (global_current_report)
            {
              set_report_scan_run_status (global_current_report,
                                          TASK_STATUS_STOPPED);
            }
          set_task_run_status (task, TASK_STATUS_STOPPED);
          g_free (slave_task_name);
          return 0;
        }

      ret = slave_setup (connection, slave_task_name,
                         task, target, target_ssh_credential,
                         target_smb_credential, target_esxi_credential,
                         target_snmp_credential, last_stopped_report);
      if (ret == 1)
        {
          ret = slave_sleep_connect (connection, task);
          if (ret == 3)
            /* User requested "giveup". */
            break;
        }
      else
        break;
    }

  current_scanner_task = (task_t) 0;
  g_free (slave_task_name);

  return 0;
}


/* OSP tasks. */

/**
 * @brief Give a task's OSP scan options in a hash table.
 *
 * @param[in]   task        The task.
 * @param[in]   target      The target.
 *
 * @return Hash table with options names and their values.
 */
static GHashTable *
task_scanner_options (task_t task, target_t target)
{
  GHashTable *table;
  config_t config;
  iterator_t prefs;

  config = task_config (task);
  init_config_preference_iterator (&prefs, config);
  table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  while (next (&prefs))
    {
      char *name, *value = NULL;
      const char *type;

      name = g_strdup (config_preference_iterator_name (&prefs));
      type = config_preference_iterator_type (&prefs);

      if (g_str_has_prefix (type, "credential_"))
        {
          credential_t credential = 0;
          iterator_t iter;
          const char *uuid = config_preference_iterator_value (&prefs);

          if (!strcmp (config_preference_iterator_value (&prefs), "0"))
            credential = target_ssh_credential (target);
          else if (find_resource ("credential", uuid, &credential))
            {
              g_warning ("Error getting credential for osp parameter %s", name);
              g_free (name);
              continue;
            }
          if (credential == 0)
            {
              g_warning ("No credential for osp parameter %s", name);
              g_free (name);
              continue;
            }

          init_credential_iterator_one (&iter, credential);
          if (!next (&iter))
            {
              g_warning ("No credential for credential_id %llu", credential);
              g_free (name);
              continue;
            }
          if (!strcmp (type, "credential_up")
              && !strcmp (credential_iterator_type (&iter), "up"))
            value = g_strdup_printf ("%s:%s", credential_iterator_login (&iter),
                                     credential_iterator_password (&iter));
          else if (!strcmp (type, "credential_up"))
            {
              g_warning ("OSP Parameter %s requires credentials of type"
                         " username+password", name);
              g_free (name);
              continue;
            }
          else
            abort ();
          cleanup_iterator (&iter);
          if (!value)
            {
              g_warning ("No adequate %s for parameter %s", type, name);
              g_free (name);
              continue;
            }
        }
      else if (!strcmp (name, "definitions_file"))
        {
          char *fname;

          if (!config_preference_iterator_value (&prefs))
            continue;
          fname = g_strdup_printf ("%s/%s", GVM_SCAP_DATA_DIR "/",
                                   config_preference_iterator_value (&prefs));
          value = gvm_file_as_base64 (fname);
          if (!value)
            continue;
        }
      else
        value = g_strdup (config_preference_iterator_value (&prefs));
      g_hash_table_insert (table, name, value);
    }
  cleanup_iterator (&prefs);
  return table;
}

/**
 * @brief Delete an OSP scan.
 *
 * @param[in]   report_id   Report ID.
 * @param[in]   host        Scanner host.
 * @param[in]   port        Scanner port.
 * @param[in]   ca_pub      CA Certificate.
 * @param[in]   key_pub     Certificate.
 * @param[in]   key_priv    Private key.
 */
static void
delete_osp_scan (const char *report_id, const char *host, int port,
                 const char *ca_pub, const char *key_pub, const char *key_priv)
{
  osp_connection_t *connection;

  connection = osp_connect_with_data (host, port, ca_pub, key_pub, key_priv);
  if (!connection)
    {
      return;
    }
  osp_delete_scan (connection, report_id);
  osp_connection_close (connection);
}

/**
 * @brief Get an OSP scan's report.
 *
 * @param[in]   scan_id     Scan ID.
 * @param[in]   host        Scanner host.
 * @param[in]   port        Scanner port.
 * @param[in]   ca_pub      CA Certificate.
 * @param[in]   key_pub     Certificate.
 * @param[in]   key_priv    Private key.
 * @param[in]   details     1 for detailed report, 0 otherwise.
 * @param[in]   pop_results 1 to pop results, 0 to leave results intact.
 * @param[out]  report_xml  Scan report.
 *
 * @return -1 on error, progress value between 0 and 100 on success.
 */
static int
get_osp_scan_report (const char *scan_id, const char *host, int port,
                     const char *ca_pub, const char *key_pub, const char
                     *key_priv, int details, int pop_results,
                     char **report_xml)
{
  osp_connection_t *connection;
  int progress;
  char *error = NULL;

  connection = osp_connect_with_data (host, port, ca_pub, key_pub, key_priv);
  if (!connection)
    {
      return -1;
    }
  progress = osp_get_scan_pop (connection, scan_id, report_xml, details,
                               pop_results, &error);
  if (progress > 100 || progress < 0)
    {
      g_warning ("OSP get_scan %s: %s", scan_id, error);
      g_free (error);
      progress = -1;
    }

  osp_connection_close (connection);
  return progress;
}


/**
 * @brief Get an OSP scan's status.
 *
 * @param[in]   scan_id     Scan ID.
 * @param[in]   host        Scanner host.
 * @param[in]   port        Scanner port.
 * @param[in]   ca_pub      CA Certificate.
 * @param[in]   key_pub     Certificate.
 * @param[in]   key_priv    Private key.
 *
 * @return 0 in success, -1 otherwise.
 */
static osp_scan_status_t
get_osp_scan_status (const char *scan_id, const char *host, int port,
                     const char *ca_pub, const char *key_pub, const char
                     *key_priv)
{
  osp_connection_t *connection;
  char *error = NULL;
  osp_get_scan_status_opts_t get_scan_opts;
  osp_scan_status_t status = OSP_SCAN_STATUS_ERROR;

  connection = osp_connect_with_data (host, port, ca_pub, key_pub, key_priv);
  if (!connection)
    {
      return status;
    }

  get_scan_opts.scan_id = scan_id;
  status = osp_get_scan_status_ext (connection, get_scan_opts, &error);
  if (status == OSP_SCAN_STATUS_ERROR)
    {
      g_warning ("OSP %s %s: %s", __func__, scan_id, error);
      g_free (error);
      return status;
    }

  osp_connection_close (connection);
  return status;
}

/**
 * @brief Handle an ongoing OSP scan, until success or failure.
 *
 * @param[in]   task      The task.
 * @param[in]   report    The report.
 * @param[in]   scan_id   The UUID of the scan on the scanner.
 *
 * @return 0 if success, -1 if error, -2 if scan was stopped.
 */
static int
handle_osp_scan (task_t task, report_t report, const char *scan_id)
{
  char *host, *ca_pub, *key_pub, *key_priv;
  int rc, port;
  scanner_t scanner;
  gboolean started;

  scanner = task_scanner (task);
  host = scanner_host (scanner);
  port = scanner_port (scanner);
  ca_pub = scanner_ca_pub (scanner);
  key_pub = scanner_key_pub (scanner);
  key_priv = scanner_key_priv (scanner);
  started = FALSE;

  while (1)
    {
      char *report_xml = NULL;
      int run_status;
      osp_scan_status_t osp_scan_status;

      run_status = task_run_status (task);
      if (run_status == TASK_STATUS_STOPPED
          || run_status == TASK_STATUS_STOP_REQUESTED)
        {
          rc = -2;
          break;
        }
      int progress = get_osp_scan_report (scan_id, host, port, ca_pub, key_pub,
                                          key_priv, 0, 0, &report_xml);
      if (progress < 0 || progress > 100)
        {
          result_t result = make_osp_result
                             (task, "", "", "",
                              threat_message_type ("Error"),
                              "Erroneous scan progress value", "", "",
                              QOD_DEFAULT);
          report_add_result (report, result);
          delete_osp_scan (scan_id, host, port, ca_pub, key_pub,
                           key_priv);
          rc = -1;
          break;
        }
      else
        {
          /* Get the full OSP report. */
          progress = get_osp_scan_report (scan_id, host, port, ca_pub, key_pub,
                                          key_priv, 1, 1, &report_xml);
          if (progress < 0 || progress > 100)
            {
              result_t result = make_osp_result
                                 (task, "", "", "",
                                  threat_message_type ("Error"),
                                  "Erroneous scan progress value", "", "",
                                  QOD_DEFAULT);
              report_add_result (report, result);
              rc = -1;
              break;
            }
          else
            {
              set_report_slave_progress (report, progress);
              parse_osp_report (task, report, report_xml);
              g_free (report_xml);

              osp_scan_status = get_osp_scan_status (scan_id, host, port,
                                                     ca_pub, key_pub, key_priv);
              if (progress >= 0 && progress < 100
                  && osp_scan_status == OSP_SCAN_STATUS_STOPPED)
                {
                  result_t result = make_osp_result
                    (task, "", "", "",
                     threat_message_type ("Error"),
                     "Scan stopped unexpectedly by the server", "", "",
                     QOD_DEFAULT);
                  report_add_result (report, result);
                  delete_osp_scan (scan_id, host, port, ca_pub, key_pub,
                                   key_priv);
                  rc = -1;
                  break;
                }
              else if (progress == 100
                       && osp_scan_status == OSP_SCAN_STATUS_FINISHED)
                {
                  delete_osp_scan (scan_id, host, port, ca_pub, key_pub,
                                   key_priv);
                  rc = 0;
                  break;
                }
              else if (osp_scan_status == OSP_SCAN_STATUS_RUNNING
                       && started == FALSE)
                {
                  set_task_run_status (task, TASK_STATUS_RUNNING);
                  set_report_scan_run_status (global_current_report,
                                              TASK_STATUS_RUNNING);
                  started = TRUE;
                }
            }
        }

      gvm_sleep (5);
    }

  g_free (host);
  g_free (ca_pub);
  g_free (key_pub);
  g_free (key_priv);
  return rc;
}

/**
 * @brief Get an OSP Task's scan options.
 *
 * @param[in]   task        The task.
 * @param[in]   target      The target.
 *
 * @return OSP Task options, NULL if failure.
 */
static GHashTable *
get_osp_task_options (task_t task, target_t target)
{
  char *ssh_port;
  const char *user, *pass;
  iterator_t iter;
  credential_t cred;
  GHashTable *options = task_scanner_options (task, target);

  if (!options)
    return NULL;

  cred = target_ssh_credential (target);
  if (cred)
    {
      ssh_port = target_ssh_port (target);
      g_hash_table_insert (options, g_strdup ("port"), ssh_port);

      init_credential_iterator_one (&iter, cred);
      if (!next (&iter))
        {
          g_warning ("%s: LSC Credential not found.", __FUNCTION__);
          g_hash_table_destroy (options);
          cleanup_iterator (&iter);
          return NULL;
        }
      if (credential_iterator_private_key (&iter))
        {
          g_warning ("%s: LSC Credential not a user/pass pair.", __FUNCTION__);
          g_hash_table_destroy (options);
          cleanup_iterator (&iter);
          return NULL;
        }
      user = credential_iterator_login (&iter);
      pass = credential_iterator_password (&iter);
      g_hash_table_insert (options, g_strdup ("username"), g_strdup (user));
      g_hash_table_insert (options, g_strdup ("password"), g_strdup (pass));
      cleanup_iterator (&iter);
    }
  return options;
}

/**
 * @brief Launch an OSP task.
 *
 * @param[in]   task        The task.
 * @param[in]   target      The target.
 * @param[out]  scan_id     The new scan uuid.
 * @param[out]  error       Error return.
 *
 * @return 0 success, -1 if scanner is down.
 */
static int
launch_osp_task (task_t task, target_t target, const char *scan_id,
                 char **error)
{
  osp_connection_t *connection;
  char *target_str, *ports_str;
  GHashTable *options;
  int ret;

  options = get_osp_task_options (task, target);
  if (!options)
    return -1;
  connection = osp_scanner_connect (task_scanner (task));
  if (!connection)
    {
      g_hash_table_destroy (options);
      return -1;
    }
  target_str = target_hosts (target);
  ports_str = target_port_range (target);
  ret = osp_start_scan (connection, target_str, ports_str, options, scan_id,
                        error);

  g_hash_table_destroy (options);
  osp_connection_close (connection);
  g_free (target_str);
  g_free (ports_str);
  return ret;
}

/**
 * @brief Get the SSH credential of a target as an osp_credential_t
 *
 * @param[in]  target  The target to get the credential from.
 *
 * @return  Pointer to a newly allocated osp_credential_t
 */
static osp_credential_t *
target_osp_ssh_credential (target_t target)
{
  credential_t credential;
  credential = target_ssh_credential (target);
  if (credential)
    {
      iterator_t iter;
      const char *type;
      char *ssh_port;
      osp_credential_t *osp_credential;

      init_credential_iterator_one (&iter, credential);
      if (!next (&iter))
        {
          g_warning ("%s: SSH Credential not found.", __FUNCTION__);
          cleanup_iterator (&iter);
          return NULL;
        }
      type = credential_iterator_type (&iter);
      if (strcmp (type, "up") && strcmp (type, "usk"))
        {
          g_warning ("%s: SSH Credential not a user/pass pair"
                     " or user/ssh key.", __FUNCTION__);
          cleanup_iterator (&iter);
          return NULL;
        }

      ssh_port = target_ssh_port (target);
      osp_credential = osp_credential_new (type, "ssh", ssh_port);
      free (ssh_port);
      osp_credential_set_auth_data (osp_credential,
                                    "username",
                                    credential_iterator_login (&iter));
      osp_credential_set_auth_data (osp_credential,
                                    "password",
                                    credential_iterator_password (&iter));
      if (strcmp (type, "usk") == 0)
        {
          const char *private_key = credential_iterator_private_key (&iter);
          gchar *base64 = g_base64_encode ((guchar *) private_key,
                                           strlen (private_key));
          osp_credential_set_auth_data (osp_credential,
                                        "private", base64);
          g_free (base64);

        }
      cleanup_iterator (&iter);
      return osp_credential;
    }
  return NULL;
}

/**
 * @brief Get the SMB credential of a target as an osp_credential_t
 *
 * @param[in]  target  The target to get the credential from.
 *
 * @return  Pointer to a newly allocated osp_credential_t
 */
static osp_credential_t *
target_osp_smb_credential (target_t target)
{
  credential_t credential;
  credential = target_smb_credential (target);
  if (credential)
    {
      iterator_t iter;
      osp_credential_t *osp_credential;

      init_credential_iterator_one (&iter, credential);
      if (!next (&iter))
        {
          g_warning ("%s: SMB Credential not found.", __FUNCTION__);
          cleanup_iterator (&iter);
          return NULL;
        }
      if (strcmp (credential_iterator_type (&iter), "up"))
        {
          g_warning ("%s: SMB Credential not a user/pass pair.", __FUNCTION__);
          cleanup_iterator (&iter);
          return NULL;
        }

      osp_credential = osp_credential_new ("up", "smb", NULL);
      osp_credential_set_auth_data (osp_credential,
                                    "username",
                                    credential_iterator_login (&iter));
      osp_credential_set_auth_data (osp_credential,
                                    "password",
                                    credential_iterator_password (&iter));
      cleanup_iterator (&iter);
      return osp_credential;
    }
  return NULL;
}

/**
 * @brief Get the SMB credential of a target as an osp_credential_t
 *
 * @param[in]  target  The target to get the credential from.
 *
 * @return  Pointer to a newly allocated osp_credential_t
 */
static osp_credential_t *
target_osp_esxi_credential (target_t target)
{
  credential_t credential;
  credential = target_esxi_credential (target);
  if (credential)
    {
      iterator_t iter;
      osp_credential_t *osp_credential;

      init_credential_iterator_one (&iter, credential);
      if (!next (&iter))
        {
          g_warning ("%s: ESXi Credential not found.", __FUNCTION__);
          cleanup_iterator (&iter);
          return NULL;
        }
      if (strcmp (credential_iterator_type (&iter), "up"))
        {
          g_warning ("%s: ESXi Credential not a user/pass pair.",
                     __FUNCTION__);
          cleanup_iterator (&iter);
          return NULL;
        }

      osp_credential = osp_credential_new ("up", "esxi", NULL);
      osp_credential_set_auth_data (osp_credential,
                                    "username",
                                    credential_iterator_login (&iter));
      osp_credential_set_auth_data (osp_credential,
                                    "password",
                                    credential_iterator_password (&iter));
      cleanup_iterator (&iter);
      return osp_credential;
    }
  return NULL;
}

/**
 * @brief Get the SMB credential of a target as an osp_credential_t
 *
 * @param[in]  target  The target to get the credential from.
 *
 * @return  Pointer to a newly allocated osp_credential_t
 */
static osp_credential_t *
target_osp_snmp_credential (target_t target)
{
  credential_t credential;
  credential = target_credential (target, "snmp");
  if (credential)
    {
      iterator_t iter;
      osp_credential_t *osp_credential;

      init_credential_iterator_one (&iter, credential);
      if (!next (&iter))
        {
          g_warning ("%s: SNMP Credential not found.", __FUNCTION__);
          cleanup_iterator (&iter);
          return NULL;
        }
      if (strcmp (credential_iterator_type (&iter), "snmp"))
        {
          g_warning ("%s: SNMP Credential not of type 'snmp'.",
                     __FUNCTION__);
          cleanup_iterator (&iter);
          return NULL;
        }

      osp_credential = osp_credential_new ("snmp", "snmp", NULL);
      osp_credential_set_auth_data (osp_credential,
                                    "username",
                                    credential_iterator_login (&iter)
                                      ?: "");
      osp_credential_set_auth_data (osp_credential,
                                    "password",
                                    credential_iterator_password (&iter)
                                      ?: "");
      osp_credential_set_auth_data (osp_credential,
                                    "community",
                                    credential_iterator_community (&iter)
                                      ?: "");
      osp_credential_set_auth_data (osp_credential,
                                    "auth_algorithm",
                                    credential_iterator_auth_algorithm (&iter)
                                      ?: "");
      osp_credential_set_auth_data (osp_credential,
                                    "privacy_algorithm",
                                    credential_iterator_privacy_algorithm
                                      (&iter) ?: "");
      osp_credential_set_auth_data (osp_credential,
                                    "privacy_password",
                                    credential_iterator_privacy_password
                                      (&iter) ?: "");
      cleanup_iterator (&iter);
      return osp_credential;
    }
  return NULL;
}

/**
 * @brief Prepare a report for resuming an OSP scan
 *
 * @param[in]  task     The task of the scan.
 * @param[in]  scan_id  The scan uuid.
 * @param[out] error    Error return.
 *
 * @return 0 scan finished or still running,
 *         1 scan must be started,
 *         -1 error
 */
static int
prepare_osp_scan_for_resume (task_t task, const char *scan_id, char **error)
{
  osp_connection_t *connection;
  osp_get_scan_status_opts_t status_opts;
  osp_scan_status_t status;

  assert (task);
  assert (scan_id);
  assert (global_current_report);
  assert (error);

  status_opts.scan_id = scan_id;

  connection = osp_scanner_connect (task_scanner (task));
  if (!connection)
    {
      *error = g_strdup ("Could not connect to Scanner");
      return -1;
    }
  status = osp_get_scan_status_ext (connection, status_opts, error);

  if (status == OSP_SCAN_STATUS_ERROR)
    {
      if (g_str_has_prefix (*error, "Failed to find scan"))
        {
          g_debug ("%s: Scan %s not found", __func__, scan_id);
          g_free (*error);
          *error = NULL;
          osp_connection_close (connection);
          trim_partial_report (global_current_report);
          return 1;
        }
      else
        {
          g_warning ("%s: Error getting status of scan %s: %s",
                     __func__, scan_id, *error);
          osp_connection_close (connection);
          return -1;
        }
    }
  else if (status == OSP_SCAN_STATUS_RUNNING
           || status == OSP_SCAN_STATUS_FINISHED)
    {
      g_debug ("%s: Scan %s running or finished", __func__, scan_id);
      /* It would be possible to simply continue getting the results
       * from the scanner, but gvmd may have crashed while receiving
       * or storing the results, so some may be missing. */
      if (osp_stop_scan (connection, scan_id, error))
        {
          osp_connection_close (connection);
          return -1;
        }
      if (osp_delete_scan (connection, scan_id))
        {
          *error = g_strdup ("Failed to delete old report");
          osp_connection_close (connection);
          return -1;
        }
      osp_connection_close (connection);
      trim_partial_report (global_current_report);
      return 1;
    }
  else if (status == OSP_SCAN_STATUS_STOPPED)
    {
      g_debug ("%s: Scan %s stopped", __func__, scan_id);
      if (osp_delete_scan (connection, scan_id))
        {
          *error = g_strdup ("Failed to delete old report");
          osp_connection_close (connection);
          return -1;
        }
      osp_connection_close (connection);
      trim_partial_report (global_current_report);
      return 1;
    }

  g_warning ("%s: Unexpected scanner status %d", __func__, status);
  *error = g_strdup_printf ("Unexpected scanner status %d", status);
  osp_connection_close (connection);
  return -1;
}

/**
 * @brief Add OSP preferences for limiting ifaces and hosts for users.
 *
 * @param[in]  scanner_options  The scanner preferences table to add to.
 */
static void
add_user_scan_preferences (GHashTable *scanner_options)
{
  gchar *hosts, *ifaces, *name;
  int hosts_allow, ifaces_allow;

  // Limit access to hosts
  hosts = user_hosts (current_credentials.uuid);
  hosts_allow = user_hosts_allow (current_credentials.uuid);

  if (hosts_allow == 1)
    name = g_strdup ("hosts_allow");
  else if (hosts_allow == 0)
    name = g_strdup ("hosts_deny");
  else
    name = NULL;

  if (name
      && (hosts_allow || (hosts && strlen (hosts))))
    g_hash_table_replace (scanner_options,
                          name,
                          hosts ? hosts : g_strdup (""));
  else
    g_free (hosts);

  // Limit access to ifaces
  ifaces = user_ifaces (current_credentials.uuid);
  ifaces_allow = user_ifaces_allow (current_credentials.uuid);

  if (ifaces_allow == 1)
    name = g_strdup ("ifaces_allow");
  else if (ifaces_allow == 0)
    name = g_strdup ("ifaces_deny");
  else
    name = NULL;

  if (name
      && (ifaces_allow || (ifaces && strlen (ifaces))))
    g_hash_table_replace (scanner_options,
                          name,
                          ifaces ? ifaces : g_strdup (""));
  else
    g_free (ifaces);
}

/**
 * @brief Launch an OpenVAS via OSP task.
 *
 * @param[in]   task        The task.
 * @param[in]   target      The target.
 * @param[in]   scan_id     The scan uuid.
 * @param[in]   from        0 start from beginning, 1 continue from stopped,
 *                          2 continue if stopped else start from beginning.
 * @param[out]  error       Error return.
 *
 * @return 0 success, -1 if scanner is down.
 */
static int
launch_osp_openvas_task (task_t task, target_t target, const char *scan_id,
                         int from, char **error)
{
  osp_connection_t *connection;
  char *hosts_str, *ports_str, *exclude_hosts_str, *finished_hosts_str;
  osp_target_t *osp_target;
  GSList *osp_targets, *vts;
  GHashTable *vts_hash_table;
  osp_credential_t *ssh_credential, *smb_credential, *esxi_credential;
  osp_credential_t *snmp_credential;
  gchar *max_checks, *max_hosts, *source_iface, *hosts_ordering;
  gchar *reverse_lookup_unify, *reverse_lookup_only;
  GHashTable *scanner_options;
  int ret;
  config_t config;
  iterator_t scanner_prefs_iter, families, prefs;
  osp_start_scan_opts_t start_scan_opts;
  alive_test_t alive_test;
  osp_vt_single_t *alive_test_vt;

  config = task_config (task);

  connection = NULL;

  /* Prepare the report */
  if (from)
    {
      ret = prepare_osp_scan_for_resume (task, scan_id, error);
      if (ret == 0)
        return 0;
      else if (ret == -1)
        return -1;
      finished_hosts_str = report_finished_hosts_str (global_current_report);
    }
  else
    finished_hosts_str = NULL;

  /* Set up target(s) */
  hosts_str = target_hosts (target);
  ports_str = target_port_range (target);
  exclude_hosts_str = target_exclude_hosts (target);
  if (finished_hosts_str)
    {
      gchar *new_exclude_hosts;

      new_exclude_hosts = g_strdup_printf ("%s,%s",
                                           exclude_hosts_str,
                                           finished_hosts_str);
      free (exclude_hosts_str);
      exclude_hosts_str = new_exclude_hosts;
    }

  osp_target = osp_target_new (hosts_str, ports_str, exclude_hosts_str);
  if (finished_hosts_str)
    osp_target_set_finished_hosts (osp_target, finished_hosts_str);

  free (hosts_str);
  free (ports_str);
  free (exclude_hosts_str);
  free (finished_hosts_str);
  osp_targets = g_slist_append (NULL, osp_target);

  ssh_credential = target_osp_ssh_credential (target);
  if (ssh_credential)
    osp_target_add_credential (osp_target, ssh_credential);

  smb_credential = target_osp_smb_credential (target);
  if (smb_credential)
    osp_target_add_credential (osp_target, smb_credential);

  esxi_credential = target_osp_esxi_credential (target);
  if (esxi_credential)
    osp_target_add_credential (osp_target, esxi_credential);

  snmp_credential = target_osp_snmp_credential (target);
  if (snmp_credential)
    osp_target_add_credential (osp_target, snmp_credential);

  /* Setup general scanner preferences */
  scanner_options
    = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  init_preference_iterator (&scanner_prefs_iter, config, "SERVER_PREFS");
  while (next (&scanner_prefs_iter))
    {
      const char *name, *value;
      name = preference_iterator_name (&scanner_prefs_iter);
      value = preference_iterator_value (&scanner_prefs_iter);
      if (name && value)
        {
          const char *osp_value;

          // Workaround for boolean scanner preferences
          if (strcmp (value, "yes") == 0)
            osp_value = "1";
          else if (strcmp (value, "no") == 0)
            osp_value = "0";
          else
            osp_value = value;
          g_hash_table_replace (scanner_options,
                                g_strdup (name),
                                g_strdup (osp_value));
        }
    }

  /* Setup user-specific scanner preference */
  add_user_scan_preferences (scanner_options);

  /* Setup general task preferences */
  max_checks = task_preference_value (task, "max_checks");
  g_hash_table_insert (scanner_options, g_strdup ("max_checks"),
                       max_checks ? max_checks : g_strdup (MAX_CHECKS_DEFAULT));

  max_hosts = task_preference_value (task, "max_hosts");
  g_hash_table_insert (scanner_options, g_strdup ("max_hosts"),
                       max_hosts ? max_hosts : g_strdup (MAX_HOSTS_DEFAULT));

  source_iface = task_preference_value (task, "source_iface");
  if (source_iface)
    g_hash_table_insert (scanner_options, g_strdup ("source_iface"),
                        source_iface);

  hosts_ordering = task_hosts_ordering (task);
  if (hosts_ordering)
    g_hash_table_insert (scanner_options, g_strdup ("hosts_ordering"),
                         hosts_ordering);

  /* Setup reverse_lookup_only preference. */
  reverse_lookup_only = target_reverse_lookup_only (target);
  if (reverse_lookup_only == NULL || strcmp (reverse_lookup_only, "0") == 0)
    g_hash_table_insert (scanner_options, g_strdup ("reverse_lookup_only"),
                         g_strdup ("no"));
  else
    g_hash_table_insert (scanner_options, g_strdup ("reverse_lookup_only"),
                         g_strdup ("yes"));
  g_free (reverse_lookup_only);

  /* Send reverse_lookup_unify preference. */
  reverse_lookup_unify = target_reverse_lookup_unify (target);
  if (reverse_lookup_unify == NULL || strcmp (reverse_lookup_unify, "0") == 0)
    g_hash_table_insert (scanner_options, g_strdup ("reverse_lookup_unify"),
                         g_strdup("no"));
  else
    g_hash_table_insert (scanner_options, g_strdup ("reverse_lookup_unify"),
                         g_strdup ("yes"));
  g_free (reverse_lookup_unify);

  /* Setup vulnerability tests (without preferences) */
  vts = NULL;
  vts_hash_table
    = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  init_family_iterator (&families, 0, NULL, 1);
  while (next (&families))
    {
      const char *family = family_iterator_name (&families);
      if (family)
        {
          iterator_t nvts;
          init_nvt_iterator (&nvts, 0, config, family, NULL, 1, NULL);
          while (next (&nvts))
            {
              const char *oid;
              osp_vt_single_t *new_vt;

              oid = nvt_iterator_oid (&nvts);
              new_vt = osp_vt_single_new (oid);

              vts = g_slist_prepend (vts, new_vt);
              g_hash_table_replace (vts_hash_table, g_strdup (oid), new_vt);
            }
          cleanup_iterator (&nvts);
        }
    }
  cleanup_iterator (&families);

  /* Setup VT preferences */
  init_preference_iterator (&prefs, config, "PLUGINS_PREFS");
  while (next (&prefs))
    {
      const char *full_name, *value;
      osp_vt_single_t *osp_vt;
      gchar **split_name;

      full_name = preference_iterator_name (&prefs);
      value = preference_iterator_value (&prefs);
      split_name = g_strsplit (full_name, ":", 4);

      osp_vt = NULL;
      if (split_name && split_name[0] && split_name[1] && split_name[2])
        {
          const char *oid = split_name[0];
          const char *pref_id = split_name[1];
          const char *type = split_name[2];
          gchar *osp_value = NULL;

          if (strcmp (type, "checkbox") == 0)
            {
              if (strcmp (value, "yes") == 0)
                osp_value = g_strdup ("1");
              else
                osp_value = g_strdup ("0");
            }
          else if (strcmp (type, "radio") == 0)
            {
              gchar** split_value;
              split_value = g_strsplit (value, ";", 2);
              osp_value = g_strdup (split_value[0]);
              g_strfreev (split_value);
            }
          else if (strcmp (type, "file") == 0)
            osp_value = g_base64_encode ((guchar*) value, strlen (value));

          osp_vt = g_hash_table_lookup (vts_hash_table, oid);
          if (osp_vt)
            osp_vt_single_add_value (osp_vt, pref_id,
                                     osp_value ? osp_value : value);
          g_free (osp_value);
        }

      g_strfreev (split_name);
    }
  cleanup_iterator (&prefs);

  /* Setup alive tests. This has priority over scan config settings */
  alive_test = target_alive_tests (target);
  if (alive_test != 0)
    {
      alive_test_vt = g_hash_table_lookup (vts_hash_table, OID_PING_HOST);
      if (alive_test_vt == NULL)
        {
          alive_test_vt = osp_vt_single_new (OID_PING_HOST);
          vts = g_slist_prepend (vts, alive_test_vt);
          g_hash_table_replace (vts_hash_table, g_strdup (OID_PING_HOST),
                                alive_test_vt);

        }

      osp_vt_single_add_value (alive_test_vt, "1",
                               (alive_test & ALIVE_TEST_TCP_ACK_SERVICE
                                || alive_test & ALIVE_TEST_TCP_SYN_SERVICE)
                               ? "1" : "0");

      osp_vt_single_add_value (alive_test_vt, "2",
                               ((alive_test & ALIVE_TEST_TCP_SYN_SERVICE)
                                && (alive_test & ALIVE_TEST_TCP_ACK_SERVICE))
                               ? "1" : "0");

      osp_vt_single_add_value (alive_test_vt, "7",
                               ((alive_test & ALIVE_TEST_TCP_SYN_SERVICE)
                                && !(alive_test & ALIVE_TEST_TCP_ACK_SERVICE))
                               ? "1" : "0");

      osp_vt_single_add_value (alive_test_vt, "3",
                               (alive_test & ALIVE_TEST_ICMP) ? "1" : "0");

      osp_vt_single_add_value (alive_test_vt, "4",
                               (alive_test & ALIVE_TEST_ARP) ? "1" : "0");

      osp_vt_single_add_value (alive_test_vt, "5",
                               (alive_test & ALIVE_TEST_CONSIDER_ALIVE)
                               ? "0" : "1");

      if (alive_test == ALIVE_TEST_CONSIDER_ALIVE)
        {
          /* Also select a method, otherwise Ping Host logs a warning. */
          osp_vt_single_add_value (alive_test_vt, "1", "1");
        }
    }

  /* Start the scan */
  connection = osp_scanner_connect (task_scanner (task));
  if (!connection)
    {
      if (error)
        *error = g_strdup ("Could not connect to Scanner");
      g_slist_free_full (osp_targets, (GDestroyNotify) osp_target_free);
      // Credentials are freed with target
      g_slist_free_full (vts, (GDestroyNotify) osp_vt_single_free);
      g_hash_table_destroy (scanner_options);
      return -1;
    }

  start_scan_opts.targets = osp_targets;
  start_scan_opts.vt_groups = NULL;
  start_scan_opts.vts = vts;
  start_scan_opts.scanner_params = scanner_options;
  start_scan_opts.parallel = 1;
  start_scan_opts.scan_id = scan_id;

  ret = osp_start_scan_ext (connection,
                            start_scan_opts,
                            error);

  osp_connection_close (connection);
  g_slist_free_full (osp_targets, (GDestroyNotify) osp_target_free);
  // Credentials are freed with target
  g_slist_free_full (vts, (GDestroyNotify) osp_vt_single_free);
  g_hash_table_destroy (scanner_options);
  return ret;
}

/**
 * @brief Get the last stopped report or a new one for an OSP scan.
 *
 * @param[in]   task      The task.
 * @param[in]   from      0 start from beginning, 1 continue from stopped,
 *                        2 continue if stopped else start from beginning.
 * @param[out]  report_id UUID of the report.
 *
 * @return 0 success, -1 error
 */
static int
run_osp_scan_get_report (task_t task, int from, char **report_id)
{
  report_t resume_report;

  resume_report = 0;
  *report_id = NULL;

  if (from == 1
      && scanner_type (task_scanner (task)) == SCANNER_TYPE_OSP)
    {
      g_warning ("%s: Scanner type does not support resuming scans",
                 __FUNCTION__);
      return -1;
    }

  if (from
      && scanner_type (task_scanner (task)) != SCANNER_TYPE_OSP
      && task_last_resumable_report (task, &resume_report))
    {
      g_warning ("%s: error getting report to resume", __func__);
      return -1;
    }

  if (resume_report)
    {
      // Report to resume found
      if (global_current_report)
        {
           g_warning ("%s: global_current_report already set", __func__);
          return -1;
        }
      global_current_report = resume_report;
      *report_id = report_uuid (resume_report);

      /* Ensure the report is marked as requested. */
      set_report_scan_run_status (resume_report, TASK_STATUS_REQUESTED);

      /* Clear the end times of the task and partial report. */
      set_task_start_time_epoch (task,
                                 scan_start_time_epoch (resume_report));
      set_task_end_time (task, NULL);
      set_scan_end_time (resume_report, NULL);
    }
  else if (from == 1)
    // No report to resume and starting a new one is not allowed
    return -1;

  // Try starting a new report
  if (resume_report == 0
      && create_current_report (task, report_id, TASK_STATUS_REQUESTED))
    {
      g_debug ("   %s: failed to create report", __func__);
      return -1;
    }

  return 0;
}

/**
 * @brief Fork a child to handle an OSP scan's fetching and inserting.
 *
 * @param[in]   task       The task.
 * @param[in]   target     The target.
 * @param[in]   from       0 start from beginning, 1 continue from stopped,
 *                         2 continue if stopped else start from beginning.
 * @param[out]  report_id_return   UUID of the report.
 *
 * @return Parent returns with 0 if success, -1 if failure. Child process
 *         doesn't return and simply exits.
 */
static int
fork_osp_scan_handler (task_t task, target_t target, int from,
                       char **report_id_return)
{
  char *report_id, title[128], *error = NULL;
  int rc;

  assert (task);
  assert (target);

  if (report_id_return)
    *report_id_return = NULL;

  if (run_osp_scan_get_report (task, from, &report_id))
    return -1;

  set_task_run_status (task, TASK_STATUS_REQUESTED);

  switch (fork ())
    {
      case 0:
        break;
      case -1:
        /* Parent, failed to fork. */
        global_current_report = 0;
        g_warning ("%s: Failed to fork: %s",
                   __FUNCTION__,
                   strerror (errno));
        set_task_interrupted (task,
                              "Error forking scan handler."
                              "  Interrupting scan.");
        set_report_scan_run_status (global_current_report,
                                    TASK_STATUS_INTERRUPTED);
        global_current_report = (report_t) 0;
        g_free (report_id);
        return -9;
      default:
        /* Parent, successfully forked. */
        global_current_report = 0;
        if (report_id_return)
          *report_id_return = report_id;
        else
          g_free (report_id);
        return 0;
    }

  /* Child: Re-open DB after fork and periodically check scan progress.
   * If progress == 100%: Parse the report results and other info then exit(0).
   * Else, exit(1) in error cases like connection to scanner failure.
   */
  reinit_manage_process ();
  manage_session_init (current_credentials.uuid);

  if (scanner_type (task_scanner (task)) == SCANNER_TYPE_OPENVAS
      || scanner_type (task_scanner (task) == SCANNER_TYPE_OSP_SENSOR))
    {
      rc = launch_osp_openvas_task (task, target, report_id, from, &error);
    }
  else
    {
      rc = launch_osp_task (task, target, report_id, &error);
    }

  if (rc)
    {
      result_t result;

      g_warning ("OSP start_scan %s: %s", report_id, error);
      result = make_osp_result (task, "", "", "",
                                threat_message_type ("Error"),
                                error, "", "", QOD_DEFAULT);
      report_add_result (global_current_report, result);
      set_task_run_status (task, TASK_STATUS_DONE);
      set_report_scan_run_status (global_current_report, TASK_STATUS_DONE);
      set_task_end_time_epoch (task, time (NULL));
      set_scan_end_time_epoch (global_current_report, time (NULL));

      g_free (error);
      g_free (report_id);
      exit (-1);
    }

  snprintf (title, sizeof (title), "gvmd: OSP: Handling scan %s", report_id);
  proctitle_set (title);

  rc = handle_osp_scan (task, global_current_report, report_id);
  g_free (report_id);
  if (rc == 0)
    {
      hosts_set_identifiers (global_current_report);
      hosts_set_max_severity (global_current_report, NULL, NULL);
      hosts_set_details (global_current_report);
      set_task_run_status (task, TASK_STATUS_DONE);
      set_report_scan_run_status (global_current_report, TASK_STATUS_DONE);
    }
  else if (rc == -1 || rc == -2)
    {
      set_task_run_status (task, TASK_STATUS_STOPPED);
      set_report_scan_run_status (global_current_report, TASK_STATUS_STOPPED);
    }

  set_task_end_time_epoch (task, time (NULL));
  set_scan_end_time_epoch (global_current_report, time (NULL));
  global_current_report = 0;
  current_scanner_task = (task_t) 0;
  exit (rc);
}

/**
 * @brief Start a task on an OSP or OpenVAS via OSP scanner.
 *
 * @param[in]   task       The task.
 * @param[in]   from       0 start from beginning, 1 continue from stopped,
 *                         2 continue if stopped else start from beginning.
 * @param[out]  report_id  The report ID.
 *
 * @return 0 success, 99 permission denied, -1 error.
 */
static int
run_osp_task (task_t task, int from, char **report_id)
{
  target_t target;

  target = task_target (task);
  if (target)
    {
      char *uuid;
      target_t found;

      uuid = target_uuid (target);
      if (find_target_with_permission (uuid, &found, "get_targets"))
        {
          g_free (uuid);
          return -1;
        }
      g_free (uuid);
      if (found == 0)
        return 99;
    }

  if (fork_osp_scan_handler (task, target, from, report_id))
    {
      g_warning ("Couldn't fork OSP scan handler");
      return -1;
    }
  return 0;
}


/* CVE tasks. */

/**
 * @brief Perform a CVE "scan" on a host.
 *
 * @param[in]  task      Task.
 * @param[in]  gvm_host  Host.
 *
 * @return 0 success, 1 failed to get nthlast report for a host.
 */
static int
cve_scan_host (task_t task, gvm_host_t *gvm_host)
{
  report_host_t report_host;
  gchar *ip, *host;

  host = gvm_host_value_str (gvm_host);

  ip = report_host_ip (host);
  if (ip == NULL)
    ip = g_strdup (host);

  g_debug ("%s: ip: %s", __FUNCTION__, ip);

  /* Get the last report that applies to the host. */

  if (host_nthlast_report_host (ip, &report_host, 1))
    {
      g_warning ("%s: Failed to get nthlast report", __FUNCTION__);
      g_free (ip);
      return 1;
    }

  g_debug ("%s: report_host: %llu", __FUNCTION__, report_host);

  if (report_host)
    {
      iterator_t report_hosts;

      /* Get the report_host for the host. */

      init_report_host_iterator (&report_hosts, 0, NULL, report_host);
      if (next (&report_hosts))
        {
          iterator_t prognosis;
          int prognosis_report_host, start_time;

          /* Add report_host with prognosis results and host details. */

          start_time = time (NULL);
          prognosis_report_host = 0;
          init_host_prognosis_iterator (&prognosis, report_host);
          while (next (&prognosis))
            {
              const char *app, *cve;
              double severity;
              gchar *desc, *location;
              result_t result;

              if (global_current_report && (prognosis_report_host == 0))
                prognosis_report_host = manage_report_host_add (global_current_report,
                                                                ip,
                                                                start_time,
                                                                0);

              severity = prognosis_iterator_cvss_double (&prognosis);

              app = prognosis_iterator_cpe (&prognosis);
              cve = prognosis_iterator_cve (&prognosis);
              location = app_location (report_host, app);

              desc = g_strdup_printf ("The host carries the product: %s\n"
                                      "It is vulnerable according to: %s.\n"
                                      "%s%s%s"
                                      "\n"
                                      "%s",
                                      app,
                                      cve,
                                      location
                                       ? "The product was found at: "
                                       : "",
                                      location ? location : "",
                                      location ? ".\n" : "",
                                      prognosis_iterator_description
                                       (&prognosis));

              g_debug ("%s: making result with severity %1.1f desc [%s]",
                       __FUNCTION__, severity, desc);

              result = make_cve_result (task, ip, cve, severity, desc);
              g_free (desc);
              if (global_current_report)
                {
                  report_add_result (global_current_report, result);

                  insert_report_host_detail (global_current_report, ip, "cve", cve,
                                             "CVE Scanner", "App", app);

                  if (location)
                    {
                      insert_report_host_detail (global_current_report, ip, "cve", cve,
                                                 "CVE Scanner", app, location);

                      insert_report_host_detail (global_current_report, ip, "cve", cve,
                                                 "CVE Scanner", "detected_at",
                                                 location);
                      insert_report_host_detail (global_current_report, ip, "cve", cve,
                                                 "CVE Scanner", "detected_by",
                                                 /* Detected by itself. */
                                                 cve);
                    }
                }
              g_free (location);
            }
          cleanup_iterator (&prognosis);

          if (prognosis_report_host)
            {
              /* Complete the report_host. */

              report_host_set_end_time (prognosis_report_host, time (NULL));
              insert_report_host_detail (global_current_report, ip, "cve", "",
                                         "CVE Scanner", "CVE Scan", "1");
            }
        }
      cleanup_iterator (&report_hosts);
    }

  g_free (ip);
  return 0;
}

/**
 * @brief Fork a child to handle a CVE scan's calculating and inserting.
 *
 * A process is forked to run the task, but the forked process never returns.
 *
 * @param[in]   task        The task.
 * @param[in]   target      The target.
 *
 * @return 0 success, -1 error, -9 failed to fork.
 */
static int
fork_cve_scan_handler (task_t task, target_t target)
{
  int pid;
  char *report_id, title[128], *hosts;
  gvm_hosts_t *gvm_hosts;
  gvm_host_t *gvm_host;

  assert (task);
  assert (target);

  if (create_current_report (task, &report_id, TASK_STATUS_REQUESTED))
    {
      g_debug ("   %s: failed to create report", __FUNCTION__);
      return -1;
    }

  set_task_run_status (task, TASK_STATUS_REQUESTED);

  pid = fork ();
  switch (pid)
    {
      case 0:
        break;
      case -1:
        /* Parent, failed to fork. */
        g_warning ("%s: Failed to fork: %s",
                   __FUNCTION__,
                   strerror (errno));
        set_task_interrupted (task,
                              "Error forking scan handler."
                              "  Interrupting scan.");
        set_report_scan_run_status (global_current_report,
                                    TASK_STATUS_INTERRUPTED);
        global_current_report = (report_t) 0;
        return -9;
      default:
        /* Parent, successfully forked. */
        g_debug ("%s: %i forked %i", __FUNCTION__, getpid (), pid);
        return 0;
    }

  /* Child.
   *
   * Re-open DB and do prognostic calculation.  On success exit(0), else
   * exit(1). */
  reinit_manage_process ();
  manage_session_init (current_credentials.uuid);

  /* Setup the task. */

  set_task_run_status (task, TASK_STATUS_RUNNING);

  snprintf (title, sizeof (title), "gvmd: CVE: Handling scan %s", report_id);
  g_free (report_id);
  proctitle_set (title);

  hosts = target_hosts (target);
  if (hosts == NULL)
    {
      set_task_interrupted (task,
                            "Error in target host list."
                            "  Interrupting scan.");
      set_report_scan_run_status (global_current_report, TASK_STATUS_INTERRUPTED);
      exit (1);
    }

  reset_task (task);
  set_task_start_time_epoch (task, time (NULL));
  set_scan_start_time_epoch (global_current_report, time (NULL));

  /* Add the results. */

  gvm_hosts = gvm_hosts_new (hosts);
  free (hosts);
  while ((gvm_host = gvm_hosts_next (gvm_hosts)))
    if (cve_scan_host (task, gvm_host))
      {
        set_task_interrupted (task,
                              "Failed to get nthlast report."
                              "  Interrupting scan.");
        set_report_scan_run_status (global_current_report, TASK_STATUS_INTERRUPTED);
        gvm_hosts_free (gvm_hosts);
        exit (1);
      }
  gvm_hosts_free (gvm_hosts);

  /* Set the end states. */

  set_scan_end_time_epoch (global_current_report, time (NULL));
  set_task_end_time_epoch (task, time (NULL));
  set_task_run_status (task, TASK_STATUS_DONE);
  set_report_scan_run_status (global_current_report, TASK_STATUS_DONE);
  global_current_report = 0;
  current_scanner_task = (task_t) 0;
  exit (0);
}

/**
 * @brief Start a CVE task.
 *
 * @param[in]   task    The task.
 *
 * @return 0 success, 99 permission denied, -1 error, -9 failed to fork.
 */
static int
run_cve_task (task_t task)
{
  target_t target;

  target = task_target (task);
  if (target)
    {
      char *uuid;
      target_t found;

      uuid = target_uuid (target);
      if (find_target_with_permission (uuid, &found, "get_targets"))
        {
          g_free (uuid);
          return -1;
        }
      g_free (uuid);
      if (found == 0)
        return 99;
    }

  if (fork_cve_scan_handler (task, target))
    {
      g_warning ("Couldn't fork CVE scan handler");
      return -1;
    }
  return 0;
}


/* Tasks. */

/**
 * @brief Initialise variables required for running a scan.
 *
 * @param[in]  task             Task.
 * @param[out] config           Config.
 * @param[out] target           Target.
 * @param[out] port_list        Port list.
 * @param[out] ssh_credential   SSH credential.
 * @param[out] smb_credential   SMB credential.
 * @param[out] esxi_credential  ESXI credential.
 * @param[out] snmp_credential  SNMP credential.
 *
 * @return 0 success, -1 error, 99 permission denied.
 */
static int
run_task_setup (task_t task, config_t *config, target_t *target,
                port_list_t *port_list, credential_t *ssh_credential,
                credential_t *smb_credential, credential_t *esxi_credential,
                credential_t *snmp_credential)
{
  int ret;

  *config = task_config (task);
  *target = task_target (task);
  *port_list = target_port_list (*target);
  *ssh_credential = target_ssh_credential (*target);
  *smb_credential = target_smb_credential (*target);
  *esxi_credential = target_esxi_credential (*target);
  *snmp_credential = target_credential (*target, "snmp");

  ret = check_available ("config", *config, "get_configs");
  if (ret)
    return ret;

  ret = check_available ("target", *target, "get_targets");
  if (ret)
    return ret;

  ret = check_available ("port_list", *port_list, "get_port_lists");
  if (ret)
    return ret;

  if (*ssh_credential
      && ((ret = check_available ("credential",
                                  *ssh_credential,
                                  "get_credentials"))))
    return ret;

  if (*smb_credential
      && ((ret = check_available ("credential",
                                  *smb_credential,
                                  "get_credentials"))))
    return ret;

  if (*esxi_credential
      && ((ret = check_available ("credential",
                                  *esxi_credential,
                                  "get_credentials"))))
    return ret;

  if (*snmp_credential
      && ((ret = check_available ("credential",
                                  *snmp_credential,
                                  "get_credentials"))))
    return ret;

  return 0;
}

/**
 * @brief Prepare report for running a task.
 *
 * @param[in]   task        The task.
 * @param[out]  report_id   The report ID.
 * @param[in]   from        0 start from beginning, 1 continue from stopped, 2
 *                          continue if stopped else start from beginning.
 * @param[in]   run_status  The task's run status.
 * @param[in]   last_stopped_report  Last stopped report of task.
 * @param[in]   for_slave_scan  If the report is for a slave scan.
 *
 * @return 0 success, -1 error, -3 creating the report failed.
 */
static int
run_task_prepare_report (task_t task, char **report_id, int from,
                         task_status_t run_status,
                         report_t *last_stopped_report,
                         gboolean for_slave_scan)
{
  if ((from == 1)
      || ((from == 2)
          && ((run_status == TASK_STATUS_STOPPED)
              || (run_status == TASK_STATUS_INTERRUPTED))))
    {
      if (task_last_resumable_report (task, last_stopped_report))
        {
          g_debug ("   error getting last stopped report");
          return -1;
        }

      global_current_report = *last_stopped_report;
      if (report_id) *report_id = report_uuid (*last_stopped_report);

      /* Remove partial host information from the report. */

      if (for_slave_scan)
        trim_report (*last_stopped_report);
      else
        trim_partial_report (*last_stopped_report);

      /* Ensure the report is marked as requested. */

      set_report_scan_run_status (global_current_report, TASK_STATUS_REQUESTED);

      /* Clear the end times of the task and partial report. */

      set_task_start_time_epoch (task, scan_start_time_epoch (global_current_report));
      set_task_end_time (task, NULL);
      set_scan_end_time (*last_stopped_report, NULL);
    }
  else if ((from == 0) || (from == 2))
    {
      *last_stopped_report = 0;

      /* Create the report. */

      if (create_current_report (task, report_id, TASK_STATUS_REQUESTED))
        return -3;

      reset_task (task);
    }
  else
    {
      /* "from" must be 0, 1 or 2. */
      assert (0);
      return -1;
    }

  return 0;
}

/**
 * @brief Start a slave GMP task.
 *
 * A process is forked to run the task, but the forked process never returns.
 *
 * @param[in]  task        The task.
 * @param[in]  from        0 start from beginning, 1 continue from stopped, 2
 *                         continue if stopped else start from beginning.
 * @param[out] report_id   The report ID.
 * @param[in]  connection  Connection, with slave info.
 * @param[in]  slave_id    UUID of slave.
 * @param[in]  slave_name  Name of slave.
 *
 * @return 1 task is active already, 3 failed to find task,
 *         4 resuming not supported, 99 permission denied, -1 error,
 *         -3 creating report failed, -4 target host is NULL,
 *         -9 failed to fork.
 */
static int
run_gmp_slave_task (task_t task, int from, char **report_id,
                    gvm_connection_t *connection,
                    const gchar *slave_id,
                    const gchar *slave_name)
{
  int ret, pid;
  task_status_t run_status;
  report_t last_stopped_report;
  char title[128], *uuid;
  target_t target;
  config_t config;
  credential_t ssh_credential, smb_credential, esxi_credential, snmp_credential;
  port_list_t port_list;

  ret = run_task_setup (task, &config, &target, &port_list, &ssh_credential,
                        &smb_credential, &esxi_credential, &snmp_credential);
  if (ret)
    return ret;

  /* When resuming, check that the scanner supports it. */

  if (from > 0 && config_type (config) > 0)
    return 4;

  /* Mark task "Requested".
   *
   * Every fail exit from here must reset the run status. */

  if (set_task_requested (task, &run_status))
    return 1;

  /* Prepare the report. */

  ret = run_task_prepare_report (task, report_id, from, run_status,
                                 &last_stopped_report, TRUE);
  if (ret)
    {
      set_task_run_status (task, run_status);
      return ret;
    }

  /* Check target. */

  if (target_hosts (target) == NULL)
    {
      g_debug ("   target hosts is NULL");
      set_task_run_status (task, run_status);
      return -4;
    }

  set_report_scheduled (global_current_report);

  /* Every fail exit from here must reset to this run status, and must
   * clear global_current_report. */

  /** @todo On fail exits only, may need to honour request states that one of
   *        the other processes has set on the task (stop_task,
   *        request_delete_task). */

  /** @todo Also reset status on report, as current_scanner_task is 0 here. */

  run_status = TASK_STATUS_INTERRUPTED;

  /* Fork a child to start and handle the task while the parent responds to
   * the client. */

  pid = fork ();
  switch (pid)
    {
      case 0:
        /* Child.  Carry on starting the task, reopen the database (required
         * after fork). */
        reinit_manage_process ();
        manage_session_init (current_credentials.uuid);
        break;
      case -1:
        /* Parent when error. */
        set_task_interrupted (task,
                              "Failed to fork task child."
                              "  Interrupting scan.");
        set_report_scan_run_status (global_current_report, run_status);
        global_current_report = (report_t) 0;
        return -9;
        break;
      default:
        g_debug ("%s: forked %i to run slave/gmp task",
                 __FUNCTION__,
                 pid);
        /* Parent.  Return, in order to respond to client. */
        global_current_report = (report_t) 0;
        return 0;
        break;
    }

  /* Reset any running information. */

  {
    char *iface;
    iface = task_preference_value (task, "source_iface");
    if (iface)
      report_set_source_iface (global_current_report, iface);
    else
      report_set_source_iface (global_current_report, "");
    free (iface);
  }

  uuid = report_uuid (global_current_report);
  snprintf (title, sizeof (title),
            "gvmd: GMP: Handling slave scan %s",
            uuid);
  free (uuid);
  proctitle_set (title);

  switch (handle_slave_task (task, target, ssh_credential, smb_credential,
                             esxi_credential, snmp_credential,
                             last_stopped_report, connection, slave_id,
                             slave_name))
    {
      case 0:
        break;
      default:
        assert (0);
      case 1:
        set_task_interrupted (task,
                              "Authentication with slave failed."
                              "  Interrupting scan.");
        set_report_scan_run_status (global_current_report, run_status);
        exit (EXIT_FAILURE);
      case -1:
        set_task_interrupted (task,
                              "Failed to make UUID."
                              "  Interrupting scan.");
        set_report_scan_run_status (global_current_report, run_status);
        exit (EXIT_FAILURE);
      case -2:
        set_task_interrupted (task,
                              "Failed to get slave task name."
                              "  Interrupting scan.");
        set_report_scan_run_status (global_current_report, run_status);
        exit (EXIT_FAILURE);
    }
  exit (EXIT_SUCCESS);
}

/**
 * @brief Gets the current path of the relay mapper executable.
 *
 * @return The current relay mapper path.
 */
const char *
get_relay_mapper_path ()
{
  return relay_mapper_path;
}

/**
 * @brief Gets the current path of the relay mapper executable.
 *
 * @param[in]  new_path  The new relay mapper path.
 */
void
set_relay_mapper_path (const char *new_path)
{
  g_free (relay_mapper_path);
  relay_mapper_path = new_path ? g_strdup (new_path) : NULL;
}

/**
 * @brief Gets whether to migrate sensors if relays do not match.
 *
 * @return Whether to migrate sensors if relays do not match.
 */
int
get_relay_migrate_sensors ()
{
  return relay_migrate_sensors;
}

/**
 * @brief Sets whether to migrate sensors if relays do not match.
 *
 * @param[in]  new_value  The new value.
 */
void
set_relay_migrate_sensors (int new_value)
{
  relay_migrate_sensors = new_value;
}

/**
 * @brief Gets the info about a scanner relay as an XML entity_t.
 *
 * @param[in]  original_host    The original hostname or IP address.
 * @param[in]  original_port    The original port number.
 * @param[in]  protocol         The protocol to look for, e.g. "GMP" or "OSP".
 * @param[out] ret_entity       Return location for the parsed XML.
 *
 * @return 0: success, -1 error.
 */
static int
get_relay_info_entity (const char *original_host, int original_port,
                       const char *protocol, entity_t *ret_entity)
{
  gchar **cmd, *stdout_str, *stderr_str;
  int ret, exit_code;
  GError *err;
  entity_t relay_entity;

  if (ret_entity == NULL)
    return -1;

  *ret_entity = NULL;
  stdout_str = NULL;
  stderr_str = NULL;
  ret = -1;
  exit_code = -1;
  err = NULL;

  cmd = (gchar **) g_malloc (8 * sizeof (gchar *));
  cmd[0] = g_strdup (relay_mapper_path);
  cmd[1] = g_strdup ("--host");
  cmd[2] = g_strdup (original_host);
  cmd[3] = g_strdup ("--port");
  cmd[4] = g_strdup_printf ("%d", original_port);
  cmd[5] = g_strdup ("--protocol");
  cmd[6] = g_strdup (protocol);
  cmd[7] = NULL;

  if (g_spawn_sync (NULL,
                    cmd,
                    NULL,
                    G_SPAWN_SEARCH_PATH,
                    NULL,
                    NULL,
                    &stdout_str,
                    &stderr_str,
                    &exit_code,
                    &err) == FALSE)
    {
      g_warning ("%s: g_spawn_sync failed: %s",
                  __FUNCTION__, err ? err->message : "");
      g_strfreev (cmd);
      g_free (stdout_str);
      g_free (stderr_str);
      return -1;
    }
  else if (exit_code)
    {
      g_warning ("%s: mapper exited with code %d",
                  __FUNCTION__, exit_code);
      g_message ("%s: mapper stderr:\n%s", __FUNCTION__, stderr_str);
      g_debug ("%s: mapper stdout:\n%s", __FUNCTION__, stdout_str);
      g_strfreev (cmd);
      g_free (stdout_str);
      g_free (stderr_str);
      return -1;
    }

  relay_entity = NULL;
  if (parse_entity (stdout_str, &relay_entity))
    {
      g_warning ("%s: failed to parse mapper output",
                  __FUNCTION__);
      g_message ("%s: mapper stdout:\n%s", __FUNCTION__, stdout_str);
      g_message ("%s: mapper stderr:\n%s", __FUNCTION__, stderr_str);
    }
  else
    {
      ret = 0;
      *ret_entity = relay_entity;
    }

  g_strfreev (cmd);
  g_free (stdout_str);
  g_free (stderr_str);

  return ret;
}

/**
 * @brief Gets whether there is a relay supporting the scanner type.
 *
 * @param[in]  original_host    The original hostname or IP address.
 * @param[in]  original_port    The original port number.
 * @param[in]  type             The scanner type to check.
 *
 * @return Whether there is a relay supporting the scanner type.
 */
gboolean
relay_supports_scanner_type (const char *original_host, int original_port,
                             scanner_type_t type)
{
  entity_t relay_entity = NULL;
  const char *protocol;
  gboolean ret = FALSE;

  if (type == SCANNER_TYPE_GMP)
    protocol = "GMP";
  else if (type == SCANNER_TYPE_OSP_SENSOR)
    protocol = "OSP";
  else
    return FALSE;

  if (get_relay_info_entity (original_host, original_port,
                             protocol, &relay_entity) == 0)
    {
      entity_t host_entity;
      host_entity = entity_child (relay_entity, "host");

      if (host_entity
          && strcmp (entity_text (host_entity), ""))
        {
          ret = TRUE;
        }
    }
  free_entity (relay_entity);
  return ret;
}

/**
 * @brief Gets a relay hostname and port for a sensor scanner.
 *
 * If no mapper is available, a copy of the original host, port and
 *  CA certificate are returned.
 *
 * @param[in]  original_host    The original hostname or IP address.
 * @param[in]  original_port    The original port number.
 * @param[in]  original_ca_cert The original CA certificate.
 * @param[in]  protocol         The protocol to look for, e.g. "GMP" or "OSP".
 * @param[out] new_host         The hostname or IP address of the relay.
 * @param[out] new_port         The port number of the relay.
 * @param[out] new_ca_cert      The CA certificate of the relay.
 *
 * @return 0 success, 1 relay not found, -1 error.
 */
int
slave_get_relay (const char *original_host,
                 int original_port,
                 const char *original_ca_cert,
                 const char *protocol,
                 gchar **new_host,
                 int *new_port,
                 gchar **new_ca_cert)
{
  int ret = -1;

  assert (new_host);
  assert (new_port);
  assert (new_ca_cert);

  if (relay_mapper_path == NULL)
    {
      *new_host = original_host ? g_strdup (original_host) : NULL;
      *new_port = original_port;
      *new_ca_cert = original_ca_cert ? g_strdup (original_ca_cert) : NULL;

      return 0;
    }
  else
    {
      entity_t relay_entity = NULL;

      if (get_relay_info_entity (original_host, original_port,
                                 protocol, &relay_entity) == 0)
        {
          entity_t host_entity, port_entity, ca_cert_entity;

          host_entity = entity_child (relay_entity, "host");
          port_entity = entity_child (relay_entity, "port");
          ca_cert_entity = entity_child (relay_entity, "ca_cert");

          if (host_entity && port_entity && ca_cert_entity)
            {
              if (entity_text (host_entity)
                  && entity_text (port_entity)
                  && strcmp (entity_text (host_entity), "")
                  && strcmp (entity_text (port_entity), ""))
                {
                  *new_host = g_strdup (entity_text (host_entity));
                  *new_port = atoi (entity_text (port_entity));

                  if (entity_text (ca_cert_entity)
                      && strcmp (entity_text (ca_cert_entity), ""))
                    {
                      *new_ca_cert = g_strdup (entity_text (ca_cert_entity));
                    }
                  else
                    {
                      *new_ca_cert = NULL;
                    }
                  ret = 0;
                }
              else
                {
                  // Consider relay not found if host or port is empty
                  ret = 1; 
                }
            }
          else
            {
              g_warning ("%s: mapper output did not contain"
                         " HOST, PORT and CA_CERT",
                         __FUNCTION__);
            }
          free_entity (relay_entity);
        }
    }

  return ret;
}

/**
 * @brief Sets up modified connection data to connect to a sensors list host.
 *
 * @param[in]     old_conn  Connection data for the original host.
 * @param[in,out] new_conn  Struct to write local connection data to.
 *
 * @return 0 success, 1 relay not found, -1 error.
 */
int
slave_relay_connection (gvm_connection_t *old_conn,
                        gvm_connection_t *new_conn)
{
  int ret;

  assert (old_conn);
  assert (new_conn);
  assert (old_conn->host_string);

  memset (new_conn, 0, sizeof (*new_conn));

  ret = slave_get_relay (old_conn->host_string,
                         old_conn->port,
                         old_conn->ca_cert,
                         "GMP",
                         &new_conn->host_string,
                         &new_conn->port,
                         &new_conn->ca_cert);

  if (ret)
    return ret;

  new_conn->tls = old_conn->tls;
  new_conn->session = old_conn->session;
  new_conn->username
    = old_conn->username ? g_strdup (old_conn->username) : NULL;
  new_conn->password
    = old_conn->password ? g_strdup (old_conn->password) : NULL;

  return 0;
}

/**
 * @brief Start a task on a GMP scanner.
 *
 * A process is forked to run the task, but the forked process never returns.
 *
 * @param[in]   task        The task.
 * @param[in]   scanner     Slave scanner to run task on.
 * @param[in]   from        0 start from beginning, 1 continue from stopped, 2
 *                          continue if stopped else start from beginning.
 * @param[out]  report_id   The report ID.
 *
 * @return 1 task is active already, 3 failed to find task,
 *         4 resuming not supported, 99 permission denied, -1 error,
 *         -3 creating report failed, -4 target host is NULL,
 *         -9 failed to fork.
 */
static int
run_gmp_task (task_t task, scanner_t scanner, int from, char **report_id)
{
  int ret;
  gvm_connection_t connection, relay_connection;
  char *scanner_id, *name;

  memset (&connection, 0, sizeof (connection));

  connection.host_string = scanner_host (scanner);
  if (connection.host_string == NULL)
    {
      g_warning ("%s: Scanner has no host", __FUNCTION__);
      return -1;
    }

  g_debug ("   %s: connection.host: %s", __FUNCTION__,
           connection.host_string);

  connection.port = scanner_port (scanner);
  if (connection.port == -1)
    {
      free (connection.host_string);
      g_warning ("%s: Scanner has no port", __FUNCTION__);
      return -1;
    }

  connection.username = scanner_login (scanner);
  if (connection.username == NULL)
    {
      free (connection.host_string);
      g_warning ("%s: Scanner has no login username", __FUNCTION__);
      return -1;
    }

  connection.password = scanner_password (scanner);
  if (connection.password == NULL)
    {
      free (connection.username);
      free (connection.host_string);
      g_warning ("%s: Scanner has no login password", __FUNCTION__);
      return -1;
    }

  connection.ca_cert = scanner_ca_pub (scanner);

  scanner_id = scanner_uuid (scanner);
  name = scanner_name (scanner);

  connection.tls = 1;

  ret = slave_relay_connection (&connection, &relay_connection);
  if (ret)
    {
      free (connection.host_string);
      free (connection.username);
      free (connection.password);
      free (connection.ca_cert);
      free (scanner_id);
      free (name);
      return -1;
    }

  // TODO: Relay only when actual connection is established.
  ret = run_gmp_slave_task (task, from, report_id, &relay_connection,
                            scanner_id, name);

  free (connection.host_string);
  free (connection.username);
  free (connection.password);
  free (connection.ca_cert);
  free (scanner_id);
  free (name);

  return ret;
}

/**
 * @brief Start or resume a task.
 *
 * A process will be forked to handle the task, but the forked process will
 * never return.
 *
 * @param[in]   task_id     The task ID.
 * @param[out]  report_id   The report ID.
 * @param[in]   from        0 start from beginning, 1 continue from stopped, 2
 *                          continue if stopped else start from beginning.
 *
 * @return 1 task is active already,
 *         3 failed to find task,
 *         4 resuming task not supported,
 *         99 permission denied,
 *         -1 error,
 *         -2 task is missing a target,
 *         -3 creating the report failed,
 *         -4 target missing hosts,
 *         -6 already a task running in this process,
 *         -9 fork failed.
 */
static int
run_task (const char *task_id, char **report_id, int from)
{
  task_t task;
  scanner_t scanner;
  int ret;
  const char *permission;

  if (current_scanner_task)
    return -6;

  if (from == 0)
    permission = "start_task";
  else if (from == 1)
    permission = "resume_task";
  else
    {
      assert (0);
      permission = "internal_error";
    }

  task = 0;
  if (find_task_with_permission (task_id, &task, permission))
    return -1;
  if (task == 0)
    return 3;

  scanner = task_scanner (task);
  assert (scanner);
  ret = check_available ("scanner", scanner, "get_scanners");
  if (ret)
    return ret;

  if (scanner_type (scanner) == SCANNER_TYPE_CVE)
    return run_cve_task (task);

  if (scanner_type (scanner) == SCANNER_TYPE_GMP)
    return run_gmp_task (task, scanner, from, report_id);

  if (scanner_type (scanner) == SCANNER_TYPE_OPENVAS
      || scanner_type (scanner) == SCANNER_TYPE_OSP
      || scanner_type (scanner) == SCANNER_TYPE_OSP_SENSOR)
    return run_osp_task (task, from, report_id);

  return -1; // Unknown scanner type
}

/**
 * @brief Start a task.
 *
 * A process will be forked to handle the task, but the forked process will
 * never return.
 *
 * @param[in]   task_id    The task ID.
 * @param[out]  report_id  The report ID.
 *
 * @return 1 task is active already,
 *         3 failed to find task,
 *         4 resuming task not supported,
 *         99 permission denied,
 *         -1 error,
 *         -2 task is missing a target,
 *         -3 creating the report failed,
 *         -4 target missing hosts,
 *         -6 already a task running in this process,
 *         -9 fork failed.
 */
int
start_task (const char *task_id, char **report_id)
{
  if (acl_user_may ("start_task") == 0)
    return 99;

  return run_task (task_id, report_id, 0);
}

/**
 * @brief Stop an OSP task.
 *
 * @param[in]   task  The task.
 *
 * @return 0 on success, else -1.
 */
static int
stop_osp_task (task_t task)
{
  osp_connection_t *connection;
  int ret = -1;
  report_t scan_report;
  char *scan_id;

  scan_report = task_running_report (task);
  scan_id = report_uuid (scan_report);
  if (!scan_id)
    goto end_stop_osp;
  connection = osp_scanner_connect (task_scanner (task));
  if (!connection)
    goto end_stop_osp;
  set_task_run_status (task, TASK_STATUS_STOP_REQUESTED);
  ret = osp_stop_scan (connection, scan_id, NULL);
  osp_connection_close (connection);
  if (ret)
    {
      g_free (scan_id);
      goto end_stop_osp;
    }

  connection = osp_scanner_connect (task_scanner (task));
  if (!connection)
    goto end_stop_osp;
  ret = osp_delete_scan (connection, scan_id);
  osp_connection_close (connection);
  g_free (scan_id);

end_stop_osp:
  set_task_end_time_epoch (task, time (NULL));
  set_task_run_status (task, TASK_STATUS_STOPPED);
  if (scan_report)
    {
      set_scan_end_time_epoch (scan_report, time (NULL));
      set_report_scan_run_status (scan_report, TASK_STATUS_STOPPED);
    }
  if (ret)
    return -1;
  return 0;
}

/**
 * @brief Initiate stopping a task.
 *
 * @param[in]  task  Task.
 *
 * @return 0 on success, 1 if stop requested.
 */
int
stop_task_internal (task_t task)
{
  task_status_t run_status;

  run_status = task_run_status (task);
  if (run_status == TASK_STATUS_REQUESTED
      || run_status == TASK_STATUS_RUNNING)
    {
      set_task_run_status (task, TASK_STATUS_STOP_REQUESTED);
      return 1;
    }
  else if (run_status == TASK_STATUS_DELETE_REQUESTED
           || run_status == TASK_STATUS_DELETE_WAITING
           || run_status == TASK_STATUS_DELETE_ULTIMATE_REQUESTED
           || run_status == TASK_STATUS_DELETE_ULTIMATE_WAITING
           || run_status == TASK_STATUS_STOP_REQUESTED
           || run_status == TASK_STATUS_STOP_WAITING)
    {
      scanner_t scanner;

      scanner = task_scanner (task);
      assert (scanner);
      if (scanner_type (scanner) == SCANNER_TYPE_GMP)
        {
          /* A special request from the user to get the task out of a requested
           * state when contact with the slave is lost. */
          set_task_run_status (task, TASK_STATUS_STOP_REQUESTED_GIVEUP);
          return 1;
        }
    }

  return 0;
}

/**
 * @brief Initiate stopping a task.
 *
 * @param[in]  task_id  Task UUID.
 *
 * @return 0 on success, 1 if stop requested, 3 failed to find task,
 *         99 permission denied, -1 error.
 */
int
stop_task (const char *task_id)
{
  task_t task;

  if (acl_user_may ("stop_task") == 0)
    return 99;

  task = 0;
  if (find_task_with_permission (task_id, &task, "stop_task"))
    return -1;
  if (task == 0)
    return 3;

  if (scanner_type (task_scanner (task)) == SCANNER_TYPE_OPENVAS
      || scanner_type (task_scanner (task)) == SCANNER_TYPE_OSP
      || scanner_type (task_scanner (task)) == SCANNER_TYPE_OSP_SENSOR)
    return stop_osp_task (task);

  return stop_task_internal (task);
}

/**
 * @brief Resume a task.
 *
 * A process will be forked to handle the task, but the forked process will
 * never return.
 *
 * @param[in]   task_id    Task UUID.
 * @param[out]  report_id  If successful, ID of the resultant report.
 *
 * @return 1 task is active already,
 *         3 failed to find task,
 *         4 resuming task not supported,
 *         22 caller error (task must be in "stopped" or "interrupted" state),
 *         99 permission denied,
 *         -1 error,
 *         -2 task is missing a target,
 *         -3 creating the report failed,
 *         -4 target missing hosts,
 *         -6 already a task running in this process,
 *         -9 fork failed.
 */
int
resume_task (const char *task_id, char **report_id)
{
  task_t task;
  task_status_t run_status;

  if (acl_user_may ("resume_task") == 0)
    return 99;

  task = 0;
  if (find_task_with_permission (task_id, &task, "resume_task"))
    return -1;
  if (task == 0)
    return 3;

  run_status = task_run_status (task);
  if ((run_status == TASK_STATUS_STOPPED)
      || (run_status == TASK_STATUS_INTERRUPTED))
    return run_task (task_id, report_id, 1);
  return 22;
}

/**
 * @brief Reassign a task to another slave.
 *
 * @param[in]  task_id    UUID of task.
 * @param[in]  slave_id   UUID of slave.
 *
 * @return 0 success, 2 task not found,
 *         3 slave not found, 4 slaves not supported by scanner, 5 task cannot
 *         be stopped currently, 6 scanner does not allow stopping, 7 new
 *         scanner does not support slaves, 98 stop and resume permission
 *         denied, 99 permission denied, -1 error.
 */
int
move_task (const char *task_id, const char *slave_id)
{
  task_t task;
  int task_scanner_type, slave_scanner_type;
  scanner_t slave, scanner;
  task_status_t status;
  int should_resume_task = 0;

  if (task_id == NULL)
    return -1;
  if (slave_id == NULL)
    return -1;

  if (acl_user_may ("modify_task") == 0)
    return 99;

  /* Find the task. */

  if (find_task_with_permission (task_id, &task, "get_tasks"))
    return -1;
  if (task == 0)
    return 2;

  /* Make sure destination scanner supports slavery. */

  if (strcmp (slave_id, "") == 0)
    slave_id = SCANNER_UUID_DEFAULT;

  if (find_scanner_with_permission (slave_id, &slave, "get_scanners"))
    return -1;
  if (slave == 0)
    return 3;

  slave_scanner_type = scanner_type (slave);
  if (slave_scanner_type != SCANNER_TYPE_OPENVAS
      && slave_scanner_type != SCANNER_TYPE_GMP)
    return 7;

  /* Make sure current scanner supports slavery. */

  scanner = task_scanner (task);
  if (scanner == 0)
    return -1;

  task_scanner_type = scanner_type (scanner);
  if (task_scanner_type != SCANNER_TYPE_OPENVAS
      && task_scanner_type != SCANNER_TYPE_GMP)
    return 4;

  /* Stop task if required. */

  status = task_run_status (task);

  switch (status)
    {
      case TASK_STATUS_DELETE_REQUESTED:
      case TASK_STATUS_DELETE_ULTIMATE_REQUESTED:
      case TASK_STATUS_DELETE_WAITING:
      case TASK_STATUS_DELETE_ULTIMATE_WAITING:
      case TASK_STATUS_REQUESTED:
        // Task cannot be stopped now
        return 5;
        break;
      case TASK_STATUS_RUNNING:
        if (task_scanner_type == SCANNER_TYPE_CVE)
          return 6;
        // Check permissions to stop and resume task
        if (acl_user_has_access_uuid ("task", task_id, "stop_task", 0)
            && acl_user_has_access_uuid ("task", task_id, "resume_task", 0))
          {
            // Stop the task, wait and resume after changes
            stop_task_internal (task);
            should_resume_task = 1;

            status = task_run_status (task);
            while (status == TASK_STATUS_STOP_REQUESTED
                   || status == TASK_STATUS_STOP_REQUESTED_GIVEUP
                   || status == TASK_STATUS_STOP_WAITING)
              {
                sleep (5);
                status = task_run_status (task);
              }
          }
        else
          return 98;
        break;
      case TASK_STATUS_STOP_REQUESTED:
      case TASK_STATUS_STOP_REQUESTED_GIVEUP:
      case TASK_STATUS_STOP_WAITING:
        while (status == TASK_STATUS_STOP_REQUESTED
                || status == TASK_STATUS_STOP_REQUESTED_GIVEUP
                || status == TASK_STATUS_STOP_WAITING)
          {
            sleep (5);
            status = task_run_status (task);
          }
        break;
      default:
        break;
    }

  /* Update scanner. */

  set_task_scanner (task, slave);

  /* Resume task if required. */

  if (should_resume_task)
    resume_task (task_id, NULL);

  return 0;
}


/* Credentials. */

/**
 * @brief Get the written-out name of an LSC Credential type.
 *
 * @param[in]  abbreviation  The type abbreviation.
 *
 * @return The written-out type name.
 */
const char*
credential_full_type (const char* abbreviation)
{
  if (abbreviation == NULL)
    return NULL;
  else if (strcasecmp (abbreviation, "cc") == 0)
    return "client certificate";
  else if (strcasecmp (abbreviation, "pw") == 0)
    return "password only";
  else if (strcasecmp (abbreviation, "snmp") == 0)
    return "SNMP";
  else if (strcasecmp (abbreviation, "up") == 0)
    return "username + password";
  else if (strcasecmp (abbreviation, "usk") == 0)
    return "username + SSH key";
  else
    return abbreviation;
}


/* System reports. */

/**
 * @brief Get a performance report from an OSP scanner.
 *
 * @param[in]  scanner          The scanner to get the performance report from.
 * @param[in]  start            The start time of the performance report.
 * @param[in]  end              The end time of the performance report.
 * @param[in]  titles           The end titles for the performance report.
 * @param[in]  performance_str  The performance string.
 *
 * @return 0 if successful, 4 could not connect to scanner,
 *         6 failed to get performance report, -1 error
 */
static int
get_osp_performance_string (scanner_t scanner, int start, int end,
                            const char *titles, gchar **performance_str)
{
  char *host, *ca_pub, *key_pub, *key_priv;
  int port;
  osp_connection_t *connection;
  osp_get_performance_opts_t opts;
  gchar *error;

  host = scanner_host (scanner);
  port = scanner_port (scanner);
  ca_pub = scanner_ca_pub (scanner);
  key_pub = scanner_key_pub (scanner);
  key_priv = scanner_key_priv (scanner);

  connection = osp_connect_with_data (host, port, ca_pub, key_pub, key_priv);

  free (host);
  free (ca_pub);
  free (key_pub);
  free (key_priv);

  if (connection == NULL)
    return 4;

  opts.start = start;
  opts.end = end;
  opts.titles = g_strdup (titles);
  error = NULL;

  if (osp_get_performance_ext (connection, opts, performance_str, &error))
    {
      osp_connection_close (connection);
      g_warning ("Error getting OSP performance report: %s", error);
      g_free (error);
      g_free (opts.titles);
      return 4;
    }

  osp_connection_close (connection);
  g_free (opts.titles);

  return 0;
}

/**
 * @brief Get system report types from a GMP slave.
 *
 * @param[in]   required_type  Single type to limit types to.
 * @param[out]  types          Types on success.
 * @param[out]  start          Actual start of types, which caller must free.
 * @param[out]  slave          GMP slave.
 *
 * @return 0 if successful, 4 could not connect to slave, 5 authentication
 * failed, 6 failed to get system report, -1 otherwise.
 */
static int
get_slave_system_report_types (const char *required_type, gchar ***start,
                               gchar ***types, scanner_t slave)
{
  char *original_host, *original_ca_cert, **end;
  int original_port, new_port, socket;
  gnutls_session_t session;
  entity_t get, report;
  entities_t reports;
  gchar *new_host, *new_ca_cert;
  int ret;

  if (slave == 0)
    return -1;

  original_host = scanner_host (slave);
  if (original_host == NULL)
    return -1;

  g_debug ("   %s: host: %s", __FUNCTION__, original_host);

  original_port = scanner_port (slave);
  if (original_port == -1)
    {
      free (original_host);
      return -1;
    }

  original_ca_cert = scanner_ca_pub (slave);

  ret = slave_get_relay (original_host,
                         original_port,
                         original_ca_cert,
                         "GMP",
                         &new_host,
                         &new_port,
                         &new_ca_cert);

  if (ret == 1)
    {
      g_message ("%s: no relay found for %s:%d",
                 __FUNCTION__, original_host, original_port);
      free (original_host);
      free (original_ca_cert);
      return 4;
    }
  else if (ret)
    {
      free (original_host);
      free (original_ca_cert);
      return -1;
    }

  free (original_host);
  free (original_ca_cert);

  socket = gvm_server_open_verify (&session,
                                   new_host,
                                   new_port,
                                   new_ca_cert,
                                   NULL,
                                   NULL,
                                   1);

  g_free (new_host);
  g_free (new_ca_cert);

  if (socket == -1)
    return 4;

  g_debug ("   %s: connected", __FUNCTION__);

  /* Authenticate using the slave login. */

  if (slave_authenticate (&session, slave))
    {
      ret = 5;
      goto fail;
    }

  g_debug ("   %s: authenticated", __FUNCTION__);

  if (gmp_get_system_reports (&session, required_type, 1, &get))
    {
      ret = 6;
      goto fail;
    }

  gvm_server_close (socket, session);

  reports = get->entities;
  end = *types = *start = g_malloc ((xml_count_entities (reports) + 1)
                                    * sizeof (gchar*));
  while ((report = first_entity (reports)))
    {
      if (strcmp (entity_name (report), "system_report") == 0)
        {
          entity_t name, title;
          gchar *pair;
          char *name_text, *title_text;
          name = entity_child (report, "name");
          title = entity_child (report, "title");
          if (name == NULL || title == NULL)
            {
              *end = NULL;
              g_strfreev (*start);
              free_entity (get);
              return -1;
            }
          name_text = entity_text (name);
          title_text = entity_text (title);
          *end = pair = g_malloc (strlen (name_text) + strlen (title_text) + 2);
          strcpy (pair, name_text);
          pair += strlen (name_text) + 1;
          strcpy (pair, title_text);
          end++;
        }
      reports = next_entities (reports);
    }
  *end = NULL;

  free_entity (get);

  return 0;

 fail:
  gvm_server_close (socket, session);
  return ret;
}

/**
 * @brief Command called by get_system_report_types.
 *        gvmcg stands for gvm-create-graphs.
 */
#define COMMAND "gvmcg 0 titles"

/**
 * @brief Get system report types.
 *
 * @param[in]   required_type  Single type to limit types to.
 * @param[out]  types          Types on success.
 * @param[out]  start          Actual start of types, which caller must free.
 * @param[out]  slave_id       ID of slave.
 *
 * @return 0 if successful, 1 failed to find report type, 2 failed to find
 *         slave, 3 serving the fallback, 4 could not connect to slave,
 *         5 authentication failed, 6 failed to get system report,
 *         -1 otherwise.
 */
static int
get_system_report_types (const char *required_type, gchar ***start,
                         gchar ***types, const char *slave_id)
{
  gchar *astdout = NULL;
  gchar *astderr = NULL;
  GError *err = NULL;
  gint exit_status;

  if (slave_id && strcmp (slave_id, "0"))
    {
      scanner_t slave;
      scanner_type_t slave_type;

      slave = 0;

      if (find_scanner_with_permission (slave_id, &slave, "get_scanners"))
        return -1;
      if (slave == 0)
        return 2;

      slave_type = scanner_type (slave);
      if (slave_type == SCANNER_TYPE_GMP)
        return get_slave_system_report_types (required_type, start, types,
                                              slave);
      else
        {
          int ret;

          // Assume OSP scanner
          ret = get_osp_performance_string (slave, 0, 0, "titles", &astdout);

          if (ret)
            return ret;
        }
    }
  else
    {
      g_debug ("   command: " COMMAND);

      if ((g_spawn_command_line_sync (COMMAND,
                                      &astdout,
                                      &astderr,
                                      &exit_status,
                                      &err)
          == FALSE)
          || (WIFEXITED (exit_status) == 0)
          || WEXITSTATUS (exit_status))
        {
          g_debug ("%s: gvmcg failed with %d", __FUNCTION__, exit_status);
          g_debug ("%s: stdout: %s", __FUNCTION__, astdout);
          g_debug ("%s: stderr: %s", __FUNCTION__, astderr);
          g_free (astdout);
          g_free (astderr);
          *start = *types = g_malloc0 (sizeof (gchar*) * 2);
          (*start)[0] = g_strdup ("fallback Fallback Report");
          (*start)[0][strlen ("fallback")] = '\0';
          return 3;
        }
    }

  if (astdout)
    {
      char **type;
      *start = *types = type = g_strsplit (g_strchomp (astdout), "\n", 0);
      while (*type)
        {
          char *space;
          space = strchr (*type, ' ');
          if (space == NULL)
            {
              g_strfreev (*types);
              *types = NULL;
              g_free (astdout);
              g_free (astderr);
              return -1;
            }
          *space = '\0';
          if (required_type && (strcmp (*type, required_type) == 0))
            {
              char **next;
              /* Found the single given type. */
              next = type + 1;
              while (*next)
                {
                  free (*next);
                  next++;
                }
              next = type + 1;
              *next = NULL;
              *types = type;
              g_free (astdout);
              g_free (astderr);
              return 0;
            }
          type++;
        }
      if (required_type)
        {
          /* Failed to find the single given type. */
          g_free (astdout);
          g_free (astderr);
          g_strfreev (*types);
          return 1;
        }
    }
  else
    *start = *types = g_malloc0 (sizeof (gchar*));

  g_free (astdout);
  g_free (astderr);
  return 0;
}

#undef COMMAND

/**
 * @brief Initialise a system report type iterator.
 *
 * @param[in]  iterator    Iterator.
 * @param[in]  type        Single report type to iterate over, NULL for all.
 * @param[in]  slave_id    ID of slave to get reports from.  0 for local.
 *
 * @return 0 on success, 1 failed to find report type, 2 failed to find slave,
 *         3 used the fallback report,  4 could not connect to slave,
 *         5 authentication failed, 6 failed to get system report,
 *         99 permission denied, -1 on error.
 */
int
init_system_report_type_iterator (report_type_iterator_t* iterator,
                                  const char* type,
                                  const char* slave_id)
{
  int ret;

  if (acl_user_may ("get_system_reports") == 0)
    return 99;

  ret = get_system_report_types (type, &iterator->start, &iterator->current,
                                 slave_id);
  if (ret == 0 || ret == 3)
    {
      iterator->current--;
      return ret;
    }
  return ret;
}

/**
 * @brief Cleanup a report type iterator.
 *
 * @param[in]  iterator  Iterator.
 */
void
cleanup_report_type_iterator (report_type_iterator_t* iterator)
{
  g_strfreev (iterator->start);
}

/**
 * @brief Increment a report type iterator.
 *
 * The caller must stop using this after it returns FALSE.
 *
 * @param[in]  iterator  Task iterator.
 *
 * @return TRUE if there was a next item, else FALSE.
 */
gboolean
next_report_type (report_type_iterator_t* iterator)
{
  iterator->current++;
  if (*iterator->current == NULL) return FALSE;
  return TRUE;
}

/**
 * @brief Return the name from a report type iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Name.
 */
const char*
report_type_iterator_name (report_type_iterator_t* iterator)
{
  return (const char*) *iterator->current;
}

/**
 * @brief Return the title from a report type iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Title.
 */
const char*
report_type_iterator_title (report_type_iterator_t* iterator)
{
  const char *name = *iterator->current;
  return name + strlen (name) + 1;
}

/**
 * @brief Get a system report from a GMP slave.
 *
 * @param[in]  name       Name of report.
 * @param[in]  duration   Time range of report, in seconds.
 * @param[in]  start_time Time of first data point in report.
 * @param[in]  end_time   Time of last data point in report.
 * @param[in]  slave      GMP scanner slave to get report from.
 * @param[out] report     On success, report in base64 if such a report exists
 *                        else NULL.  Arbitrary on error.
 *
 * @return 0 if successful, 4 could not connect to slave, 5 authentication
 * failed, 6 failed to get system report, -1 otherwise.
 */
static int
gmp_slave_system_report (const char *name, const char *duration,
                         const char *start_time, const char *end_time,
                         scanner_t slave, char **report)
{
  char *original_host, *original_ca_cert;
  int original_port, new_port, socket;
  gnutls_session_t session;
  entity_t get, entity;
  entities_t reports;
  gmp_get_system_reports_opts_t opts;
  gchar *new_host, *new_ca_cert;
  int ret;

  if (slave == 0)
    return -1;

  original_host = scanner_host (slave);
  if (original_host == NULL)
    return -1;

  g_debug ("   %s: host: %s", __FUNCTION__, original_host);

  original_port = scanner_port (slave);
  if (original_port == -1)
    {
      free (original_host);
      return -1;
    }

  original_ca_cert = scanner_ca_pub (slave);

  ret = slave_get_relay (original_host,
                         original_port,
                         original_ca_cert,
                         "GMP",
                         &new_host,
                         &new_port,
                         &new_ca_cert);

  if (ret == 1)
    {
      g_message ("%s: no relay found for %s:%d",
                 __FUNCTION__, original_host, original_port);
      free (original_host);
      free (original_ca_cert);
      return 4;
    }
  else if (ret)
    {
      free (original_host);
      free (original_ca_cert);
      return -1;
    }

  free (original_host);
  free (original_ca_cert);

  socket = gvm_server_open_verify (&session,
                                   new_host,
                                   new_port,
                                   new_ca_cert,
                                   NULL,
                                   NULL,
                                   1);

  g_free (new_host);
  g_free (new_ca_cert);

  if (socket == -1)
    return 4;

  g_debug ("   %s: connected", __FUNCTION__);

  /* Authenticate using the slave login. */

  if (slave_authenticate (&session, slave))
    {
      ret = 5;
      goto fail;
    }

  g_debug ("   %s: authenticated", __FUNCTION__);

  opts = gmp_get_system_reports_opts_defaults;
  opts.name = name;
  opts.duration = duration;
  opts.start_time = start_time;
  opts.end_time = end_time;
  opts.brief = 0;

  if (gmp_get_system_reports_ext (&session, opts, &get))
    {
      ret = 6;
      goto fail;
    }

  gvm_server_close (socket, session);

  reports = get->entities;
  if ((entity = first_entity (reports))
      && (strcmp (entity_name (entity), "system_report") == 0))
    {
      entity = entity_child (entity, "report");
      if (entity)
        {
          *report = g_strdup (entity_text (entity));
          return 0;
        }
    }

  free_entity (get);
  g_warning ("   %s: error getting entity", __FUNCTION__);
  return 6;

 fail:
  gvm_server_close (socket, session);
  return ret;
}

/**
 * @brief Header for fallback system report.
 */
#define FALLBACK_SYSTEM_REPORT_HEADER \
"This is the most basic, fallback report.  The system can be configured to\n" \
"produce more powerful reports.  Please contact your system administrator\n" \
"for more information.\n\n"

/**
 * @brief Default duration for system reports.
 */
#define DEFAULT_DURATION 86400L

/**
 * @brief Generate params for gvmcg or OSP get_performance.
 *
 * @param[in]  duration     The duration as a string
 * @param[in]  start_time   The start time as a string
 * @param[in]  end_time     The end time as a string
 * @param[out] param_1      Output of the first parameter (start or duration)
 * @param[out] param_2      Output of the second parameter (end time)
 * @param[out] params_count The number of valid parameters
 */
void
parse_performance_params (const char *duration,
                          const char *start_time,
                          const char *end_time,
                          time_t *param_1,
                          time_t *param_2,
                          int *params_count)
{
  time_t start_time_num, end_time_num, duration_num;
  start_time_num = 0;
  end_time_num = 0;
  duration_num = 0;

  *param_1 = 0;
  *param_2 = 0;
  *params_count = 0;

  if (duration && strcmp (duration, ""))
    {
      duration_num = atol (duration);
      if (duration_num == 0)
        return;
    }
  if (start_time && strcmp (start_time, ""))
    {
      start_time_num = parse_iso_time (start_time);
      if (start_time_num == 0)
        return;
    }
  if (end_time && strcmp (end_time, ""))
    {
      end_time_num = parse_iso_time (end_time);
      if (end_time_num == 0)
        return;
    }

  if (start_time && strcmp (start_time, ""))
    {
      if (end_time && strcmp (end_time, ""))
        {
          *param_1 = start_time_num;
          *param_2 = end_time_num;
          *params_count = 2;
        }
      else if (duration && strcmp (duration, ""))
        {
          *param_1 = start_time_num;
          *param_2 = start_time_num + duration_num;
          *params_count = 2;
        }
      else
        {
          *param_1 = start_time_num;
          *param_2 = start_time_num + DEFAULT_DURATION;
          *params_count = 2;
        }
    }
  else if (end_time && strcmp (end_time, ""))
    {
      if (duration && strcmp (duration, ""))
        {
          *param_1 = end_time_num - duration_num;
          *param_2 = end_time_num;
          *params_count = 2;
        }
      else
        {
          *param_1 = end_time_num - DEFAULT_DURATION,
          *param_1 = end_time_num,
          *params_count = 2;
        }
    }
  else
    {
      if (duration && strcmp (duration, ""))
        {
          *param_1 = duration_num;
          *params_count = 1;
        }
      else
        {
          *param_1 = DEFAULT_DURATION;
          *params_count = 1;
        }
    }
}

/**
 * @brief Get a system report.
 *
 * @param[in]  name       Name of report.
 * @param[in]  duration   Time range of report, in seconds.
 * @param[in]  start_time Time of first data point in report.
 * @param[in]  end_time   Time of last data point in report.
 * @param[in]  slave_id   ID of slave to get report from.  0 for local.
 * @param[out] report     On success, report in base64 if such a report exists
 *                        else NULL.  Arbitrary on error.
 *
 * @return 0 if successful (including failure to find report), -1 on error,
 *         2 could not find slave scanner,
 *         3 if used the fallback report,  4 could not connect to slave,
 *         5 authentication failed, 6 failed to get system report.
 */
int
manage_system_report (const char *name, const char *duration,
                      const char *start_time, const char *end_time,
                      const char *slave_id, char **report)
{
  gchar *astdout = NULL;
  gchar *astderr = NULL;
  GError *err = NULL;
  gint exit_status;
  gchar *command;
  time_t cmd_param_1, cmd_param_2;
  int params_count;

  assert (name);

  parse_performance_params (duration, start_time, end_time,
                            &cmd_param_1, &cmd_param_2, &params_count);

  if (params_count == 0)
    return manage_system_report ("blank", NULL, NULL, NULL, NULL, report);

  if (slave_id && strcmp (slave_id, "0"))
    {
      scanner_t slave;
      scanner_type_t slave_type;

      slave = 0;

      if (find_scanner_with_permission (slave_id, &slave, "get_scanners"))
        return -1;
      if (slave == 0)
        return 2;

      slave_type = scanner_type (slave);
      if (slave_type == SCANNER_TYPE_GMP)
        return gmp_slave_system_report (name, duration, start_time, end_time,
                                        slave, report);
      else
        {
          if (params_count == 1)
            {
              // only duration
              time_t now;
              now = time (NULL);
              return get_osp_performance_string (slave,
                                                 now - cmd_param_1,
                                                 now,
                                                 name,
                                                 report);
            }
          else
            {
              // start and end time
              return get_osp_performance_string (slave,
                                                 cmd_param_1,
                                                 cmd_param_2,
                                                 name,
                                                 report);
            }
        }
    }

  /* For simplicity, it's up to the command to do the base64 encoding. */
  if (params_count == 1)
    command = g_strdup_printf ("gvmcg %ld %s",
                               cmd_param_1,
                               name);
  else
    command = g_strdup_printf ("gvmcg %ld %ld %s",
                               cmd_param_1,
                               cmd_param_2,
                               name);

  g_debug ("   command: %s", command);

  if ((g_spawn_command_line_sync (command,
                                  &astdout,
                                  &astderr,
                                  &exit_status,
                                  &err)
       == FALSE)
      || (WIFEXITED (exit_status) == 0)
      || WEXITSTATUS (exit_status))
    {
      int ret;
      double load[3];
      GError *get_error;
      gchar *output;
      gsize output_len;
      GString *buffer;

      g_debug ("%s: gvmcg failed with %d", __FUNCTION__, exit_status);
      g_debug ("%s: stdout: %s", __FUNCTION__, astdout);
      g_debug ("%s: stderr: %s", __FUNCTION__, astderr);
      g_free (astdout);
      g_free (astderr);
      g_free (command);

      buffer = g_string_new (FALLBACK_SYSTEM_REPORT_HEADER);

      ret = getloadavg (load, 3);
      if (ret == 3)
        {
          g_string_append_printf (buffer,
                                  "Load average for past minute:     %.1f\n",
                                  load[0]);
          g_string_append_printf (buffer,
                                  "Load average for past 5 minutes:  %.1f\n",
                                  load[1]);
          g_string_append_printf (buffer,
                                  "Load average for past 15 minutes: %.1f\n",
                                  load[2]);
        }
      else
        g_string_append (buffer, "Error getting load averages.\n");

      get_error = NULL;
      g_file_get_contents ("/proc/meminfo",
                           &output,
                           &output_len,
                           &get_error);
      if (get_error)
        g_error_free (get_error);
      else
        {
          gchar *safe;
          g_string_append (buffer, "\n/proc/meminfo:\n\n");
          safe = g_markup_escape_text (output, strlen (output));
          g_free (output);
          g_string_append (buffer, safe);
          g_free (safe);
        }

      *report = g_string_free (buffer, FALSE);
      return 3;
    }
  g_free (astderr);
  g_free (command);
  if (astdout == NULL || strlen (astdout) == 0)
    {
      g_free (astdout);
      if (strcmp (name, "blank") == 0)
        return -1;
      return manage_system_report ("blank", NULL, NULL, NULL,
                                   NULL, report);
    }
  else
    *report = astdout;
  return 0;
}


/* Scheduling. */

/**
 * @brief Flag for manage_auth_allow_all.
 *
 * 1 if set via scheduler, 2 if set via event, else 0.
 */
int authenticate_allow_all = 0;

/**
 * @brief UUID of user whose scheduled task is to be started (in connection
 *        with authenticate_allow_all).
 */
static gchar* schedule_user_uuid = NULL;

/**
 * @brief Ensure that any subsequent authentications succeed.
 *
 * @param[in]  scheduled  Whether this is happening from the scheduler.
 */
void
manage_auth_allow_all (int scheduled)
{
  authenticate_allow_all = scheduled ? 1 : 2;
}

/**
 * @brief Access UUID of user that scheduled the current task.
 *
 * @return UUID of user that scheduled the current task.
 */
const gchar*
get_scheduled_user_uuid ()
{
  return schedule_user_uuid;
}

/**
 * @brief Set UUID of user that scheduled the current task.
 * The previous value is freed and a copy of the UUID is created.
 *
 * @param user_uuid UUID of user that scheduled the current task.
 */
void
set_scheduled_user_uuid (const gchar* user_uuid)
{
  gchar *user_uuid_copy = user_uuid ? g_strdup (user_uuid) : NULL;
  g_free (schedule_user_uuid);
  schedule_user_uuid = user_uuid_copy;
}

/**
 * @brief Task info, for scheduler.
 */
typedef struct
{
  gchar *owner_uuid;   ///< UUID of owner.
  gchar *owner_name;   ///< Name of owner.
  gchar *task_uuid;    ///< UUID of task.
} scheduled_task_t;

/**
 * @brief Create a schedule task structure.
 *
 * @param[in] task_uuid   UUID of task.
 * @param[in] owner_uuid  UUID of owner.
 * @param[in] owner_name  Name of owner.
 *
 * @return Scheduled task structure.
 */
static scheduled_task_t *
scheduled_task_new (const gchar* task_uuid, const gchar* owner_uuid,
                    const gchar* owner_name)
{
  scheduled_task_t *scheduled_task;

  scheduled_task = g_malloc (sizeof (*scheduled_task));
  scheduled_task->task_uuid = g_strdup (task_uuid);
  scheduled_task->owner_uuid = g_strdup (owner_uuid);
  scheduled_task->owner_name = g_strdup (owner_name);

  return scheduled_task;
}

/**
 * @brief Set UUID of user that scheduled the current task.
 *
 * @param[in] scheduled_task  Scheduled task.
 */
static void
scheduled_task_free (scheduled_task_t *scheduled_task)
{
  g_free (scheduled_task->task_uuid);
  g_free (scheduled_task->owner_uuid);
  g_free (scheduled_task->owner_name);
  g_free (scheduled_task);
}

/**
 * @brief Start a task, for the scheduler.
 *
 * @param[in]  scheduled_task   Scheduled task.
 * @param[in]  fork_connection  Function that forks a child which is connected
 *                              to the Manager.  Must return PID in parent, 0
 *                              in child, or -1 on error.
 * @param[in]  sigmask_current  Sigmask to restore in child.
 *
 * @return 0 success, -1 error.  Child does not return.
 */
static int
scheduled_task_start (scheduled_task_t *scheduled_task,
                      manage_connection_forker_t fork_connection,
                      sigset_t *sigmask_current)
{
  char title[128];
  int pid;
  gvm_connection_t connection;
  gmp_authenticate_info_opts_t auth_opts;

  /* Fork a child to start the task and wait for the response, so that the
   * parent can return to the main loop.  Only the parent returns. */

  pid = fork ();
  switch (pid)
    {
      case 0:
        /* Child.  Carry on to start the task, reopen the database (required
         * after fork). */

        /* Restore the sigmask that was blanked for pselect. */
        pthread_sigmask (SIG_SETMASK, sigmask_current, NULL);

        reinit_manage_process ();
        manage_session_init (current_credentials.uuid);
        break;

      case -1:
        /* Parent on error. */
        g_warning ("%s: fork failed", __FUNCTION__);
        return -1;

      default:
        /* Parent.  Continue to next task. */
        g_debug ("%s: %i forked %i", __FUNCTION__, getpid (), pid);
        return 0;
    }

  /* Run the callback to fork a child connected to the Manager. */

  pid = fork_connection (&connection, scheduled_task->owner_uuid);
  switch (pid)
    {
      case 0:
        /* Child.  Break, start task, exit. */
        break;

      case -1:
        /* Parent on error. */
        g_warning ("%s: fork_connection failed", __FUNCTION__);
        reschedule_task (scheduled_task->task_uuid);
        scheduled_task_free (scheduled_task);
        exit (EXIT_FAILURE);
        break;

      default:
        {
          int status;

          /* Parent.  Wait for child, to check return. */

          snprintf (title, sizeof (title),
                    "gvmd: scheduler: waiting for %i",
                    pid);
          proctitle_set (title);

          g_debug ("%s: %i fork_connectioned %i",
                   __FUNCTION__, getpid (), pid);

          if (signal (SIGCHLD, SIG_DFL) == SIG_ERR)
            g_warning ("%s: failed to set SIGCHLD", __FUNCTION__);
          while (waitpid (pid, &status, 0) < 0)
            {
              if (errno == ECHILD)
                {
                  g_warning ("%s: Failed to get child exit,"
                             " so task '%s' may not have been scheduled",
                             __FUNCTION__,
                             scheduled_task->task_uuid);
                  scheduled_task_free (scheduled_task);
                  exit (EXIT_FAILURE);
                }
              if (errno == EINTR)
                continue;
              g_warning ("%s: waitpid: %s",
                         __FUNCTION__,
                         strerror (errno));
              g_warning ("%s: As a result, task '%s' may not have been"
                         " scheduled",
                         __FUNCTION__,
                         scheduled_task->task_uuid);
              scheduled_task_free (scheduled_task);
              exit (EXIT_FAILURE);
            }
          if (WIFEXITED (status))
            switch (WEXITSTATUS (status))
              {
                case EXIT_SUCCESS:
                  {
                    schedule_t schedule;
                    int periods;
                    const gchar *task_uuid;

                    /* Child succeeded, so task successfully started. */

                    task_uuid = scheduled_task->task_uuid;
                    schedule = task_schedule_uuid (task_uuid);
                    if (schedule
                        && schedule_period (schedule) == 0
                        && schedule_duration (schedule) == 0
                        /* Check next time too, in case the user changed
                         * the schedule after this task was added to the
                         * "starts" list. */
                        && task_schedule_next_time_uuid (task_uuid) == 0)
                      /* A once-off schedule without a duration, remove
                       * it from the task.  If it has a duration it
                       * will be removed by manage_schedule via
                       * clear_duration_schedules, after the duration. */
                      set_task_schedule_uuid (task_uuid, 0, 0);
                    else if ((periods = task_schedule_periods_uuid
                                         (task_uuid)))
                      {
                        /* A task restricted to a certain number of
                         * scheduled runs. */
                        if (periods > 1)
                          {
                            set_task_schedule_periods (task_uuid,
                                                       periods - 1);
                          }
                        else if (periods == 1
                                 && schedule_duration (schedule) == 0)
                          {
                            /* Last run of a task restricted to a certain
                             * number of scheduled runs. */
                            set_task_schedule_uuid (task_uuid, 0, 1);
                          }
                        else if (periods == 1)
                          /* Flag that the task has started, for
                           * update_duration_schedule_periods. */
                          set_task_schedule_next_time_uuid (task_uuid, 0);
                      }
                  }
                  scheduled_task_free (scheduled_task);
                  exit (EXIT_SUCCESS);

                case EXIT_FAILURE:
                default:
                  break;
              }

          /* Child failed, reset task schedule time and exit. */

          g_warning ("%s: child failed", __FUNCTION__);
          reschedule_task (scheduled_task->task_uuid);
          scheduled_task_free (scheduled_task);
          exit (EXIT_FAILURE);
        }
    }

  /* Start the task. */

  snprintf (title, sizeof (title),
            "gvmd: scheduler: starting %s",
            scheduled_task->task_uuid);
  proctitle_set (title);

  auth_opts = gmp_authenticate_info_opts_defaults;
  auth_opts.username = scheduled_task->owner_name;
  if (gmp_authenticate_info_ext_c (&connection, auth_opts))
    {
      g_warning ("%s: gmp_authenticate failed", __FUNCTION__);
      scheduled_task_free (scheduled_task);
      gvm_connection_free (&connection);
      exit (EXIT_FAILURE);
    }

  if (gmp_resume_task_report_c (&connection,
                                scheduled_task->task_uuid,
                                NULL))
    {
      if (gmp_start_task_report_c (&connection,
                                   scheduled_task->task_uuid,
                                   NULL))
        {
          g_warning ("%s: gmp_start_task and gmp_resume_task failed", __FUNCTION__);
          scheduled_task_free (scheduled_task);
          gvm_connection_free (&connection);
          exit (EXIT_FAILURE);
        }
    }

  scheduled_task_free (scheduled_task);
  gvm_connection_free (&connection);
  exit (EXIT_SUCCESS);
}

/**
 * @brief Stop a task, for the scheduler.
 *
 * @param[in]  scheduled_task   Scheduled task.
 * @param[in]  fork_connection  Function that forks a child which is connected
 *                              to the Manager.  Must return PID in parent, 0
 *                              in child, or -1 on error.
 * @param[in]  sigmask_current  Sigmask to restore in child.
 *
 * @return 0 success, -1 error.  Child does not return.
 */
static int
scheduled_task_stop (scheduled_task_t *scheduled_task,
                     manage_connection_forker_t fork_connection,
                     sigset_t *sigmask_current)
{
  char title[128];
  gvm_connection_t connection;
  gmp_authenticate_info_opts_t auth_opts;

  /* TODO As with starts above, this should retry if the stop failed. */

  /* Run the callback to fork a child connected to the Manager. */

  switch (fork_connection (&connection, scheduled_task->owner_uuid))
    {
      case 0:
        /* Child.  Break, stop task, exit. */
        break;

      case -1:
        /* Parent on error. */
        g_warning ("%s: stop fork failed", __FUNCTION__);
        return -1;

      default:
        /* Parent.  Continue to next task. */
        return 0;
    }

  /* Stop the task. */

  snprintf (title, sizeof (title),
            "gvmd: scheduler: stopping %s",
            scheduled_task->task_uuid);
  proctitle_set (title);

  auth_opts = gmp_authenticate_info_opts_defaults;
  auth_opts.username = scheduled_task->owner_name;
  if (gmp_authenticate_info_ext_c (&connection, auth_opts))
    {
      scheduled_task_free (scheduled_task);
      gvm_connection_free (&connection);
      exit (EXIT_FAILURE);
    }

  if (gmp_stop_task_c (&connection, scheduled_task->task_uuid))
    {
      scheduled_task_free (scheduled_task);
      gvm_connection_free (&connection);
      exit (EXIT_FAILURE);
    }

  scheduled_task_free (scheduled_task);
  gvm_connection_free (&connection);
  exit (EXIT_SUCCESS);
}

/**
 * @brief Perform any syncing that is due.
 *
 * In gvmd, periodically called from the main daemon loop.
 *
 * @param[in]  sigmask_current  Sigmask to restore in child.
 * @param[in]  fork_update_nvt_cache  Function that forks a child that syncs
 *                                    the NVTS.  Child does not return.
 */
void
manage_sync (sigset_t *sigmask_current,
             int (*fork_update_nvt_cache) ())
{
  manage_sync_nvts (fork_update_nvt_cache);
  manage_sync_scap (sigmask_current);
  manage_sync_cert (sigmask_current);
}

/**
 * @brief Schedule any actions that are due.
 *
 * In gvmd, periodically called from the main daemon loop.
 *
 * @param[in]  fork_connection  Function that forks a child which is connected
 *                              to the Manager.  Must return PID in parent, 0
 *                              in child, or -1 on error.
 * @param[in]  run_tasks        Whether to run scheduled tasks.
 * @param[in]  sigmask_current  Sigmask to restore in child.
 *
 * @return 0 success, 1 failed to get lock, -1 error.
 */
int
manage_schedule (manage_connection_forker_t fork_connection,
                 gboolean run_tasks,
                 sigset_t *sigmask_current)
{
  iterator_t schedules;
  GSList *starts, *stops;
  int ret;
  task_t previous_start_task, previous_stop_task;

  starts = NULL;
  stops = NULL;
  previous_start_task = 0;
  previous_stop_task = 0;

  ret = manage_update_nvti_cache ();
  if (ret)
    {
      if (ret == -1)
        {
          g_warning ("%s: manage_update_nvti_cache error"
                     " (Perhaps the db went down?)",
                     __FUNCTION__);
          /* Just ignore, in case the db went down temporarily. */
          return 0;
        }

      return ret;
    }

  if (run_tasks == 0)
    return 0;

  /* Assemble "starts" and "stops" list containing task uuid, owner name and
   * owner UUID for each (scheduled) task to start or stop. */

  ret = init_task_schedule_iterator (&schedules);
  if (ret)
    {
      if (ret == -1)
        {
          g_warning ("%s: iterator init error"
                     " (Perhaps the db went down?)",
                     __FUNCTION__);
          /* Just ignore, in case the db went down temporarily. */
          return 0;
        }

      return ret;
    }
  /* This iterator runs in a transaction. */
  while (next (&schedules))
    if (task_schedule_iterator_start_due (&schedules))
      {
        const char *icalendar, *zone;
        int timed_out;

        /* Check if task schedule is timed out before updating next due time */
        timed_out = task_schedule_iterator_timed_out (&schedules);

        /* Update the task schedule info to prevent multiple schedules. */

        icalendar = task_schedule_iterator_icalendar (&schedules);
        zone = task_schedule_iterator_timezone (&schedules);

        g_debug ("%s: start due for %llu, setting next_time",
                 __FUNCTION__,
                 task_schedule_iterator_task (&schedules));
        set_task_schedule_next_time
         (task_schedule_iterator_task (&schedules),
          icalendar_next_time_from_string (icalendar, zone, 0));

        /* Skip this task if it was already added to the starts list
         * to avoid conflicts between multiple users with permissions. */

        if (previous_start_task == task_schedule_iterator_task (&schedules))
          continue;

        if (timed_out)
          {
            g_message (" %s: Task timed out: %s",
                       __FUNCTION__,
                       task_schedule_iterator_task_uuid (&schedules));
            continue;
          }

        previous_start_task = task_schedule_iterator_task (&schedules);

        /* Add task UUID and owner name and UUID to the list. */

        starts = g_slist_prepend
                  (starts,
                   scheduled_task_new
                    (task_schedule_iterator_task_uuid (&schedules),
                     task_schedule_iterator_owner_uuid (&schedules),
                     task_schedule_iterator_owner_name (&schedules)));
      }
    else if (task_schedule_iterator_stop_due (&schedules))
      {
        /* Skip this task if it was already added to the stops list
         * to avoid conflicts between multiple users with permissions. */

        if (previous_stop_task == task_schedule_iterator_task (&schedules))
          continue;
        previous_stop_task = task_schedule_iterator_task (&schedules);

        /* Add task UUID and owner name and UUID to the list. */

        stops = g_slist_prepend
                 (stops,
                  scheduled_task_new
                   (task_schedule_iterator_task_uuid (&schedules),
                    task_schedule_iterator_owner_uuid (&schedules),
                    task_schedule_iterator_owner_name (&schedules)));
      }
  cleanup_task_schedule_iterator (&schedules);

  /* Start tasks in forked processes, now that the SQL statement is closed. */

  while (starts)
    {
      scheduled_task_t *scheduled_task;
      GSList *head;

      scheduled_task = starts->data;

      head = starts;
      starts = starts->next;
      g_slist_free_1 (head);

      if (scheduled_task_start (scheduled_task,
                                fork_connection,
                                sigmask_current))
        /* Error.  Reschedule and continue to next task. */
        reschedule_task (scheduled_task->task_uuid);
      scheduled_task_free (scheduled_task);
    }

  /* Stop tasks in forked processes, now that the SQL statement is closed. */

  while (stops)
    {
      scheduled_task_t *scheduled_task;
      GSList *head;

      scheduled_task = stops->data;
      head = stops;
      stops = stops->next;
      g_slist_free_1 (head);

      if (scheduled_task_stop (scheduled_task,
                               fork_connection,
                               sigmask_current))
        {
          /* Error.  Exit. */
          scheduled_task_free (scheduled_task);
          while (stops)
            {
              scheduled_task_free (stops->data);
              stops = g_slist_delete_link (stops, stops);
            }
          return -1;
        }
      scheduled_task_free (scheduled_task);
    }

  clear_duration_schedules (0);
  update_duration_schedule_periods (0);

  auto_delete_reports ();

  return 0;
}

/**
 * @brief Get the current schedule timeout.
 *
 * @return The schedule timeout in minutes.
 */
int
get_schedule_timeout ()
{
  return schedule_timeout;
}

/**
 * @brief Set the schedule timeout.
 *
 * @param new_timeout The new schedule timeout in minutes.
 */
void
set_schedule_timeout (int new_timeout)
{
  if (new_timeout < 0)
    schedule_timeout = -1;
  else
    schedule_timeout = new_timeout;
}


/* Report formats. */

/**
 * @brief Get the name of a report format param type.
 *
 * @param[in]  type  Param type.
 *
 * @return The name of the param type.
 */
const char *
report_format_param_type_name (report_format_param_type_t type)
{
  switch (type)
    {
      case REPORT_FORMAT_PARAM_TYPE_BOOLEAN:
        return "boolean";
      case REPORT_FORMAT_PARAM_TYPE_INTEGER:
        return "integer";
      case REPORT_FORMAT_PARAM_TYPE_SELECTION:
        return "selection";
      case REPORT_FORMAT_PARAM_TYPE_STRING:
        return "string";
      case REPORT_FORMAT_PARAM_TYPE_TEXT:
        return "text";
      case REPORT_FORMAT_PARAM_TYPE_REPORT_FORMAT_LIST:
        return "report_format_list";
      default:
        assert (0);
      case REPORT_FORMAT_PARAM_TYPE_ERROR:
        return "ERROR";
    }
}

/**
 * @brief Get a report format param type from a name.
 *
 * @param[in]  name  Param type name.
 *
 * @return The param type.
 */
report_format_param_type_t
report_format_param_type_from_name (const char *name)
{
  if (strcmp (name, "boolean") == 0)
    return REPORT_FORMAT_PARAM_TYPE_BOOLEAN;
  if (strcmp (name, "integer") == 0)
    return REPORT_FORMAT_PARAM_TYPE_INTEGER;
  if (strcmp (name, "selection") == 0)
    return REPORT_FORMAT_PARAM_TYPE_SELECTION;
  if (strcmp (name, "string") == 0)
    return REPORT_FORMAT_PARAM_TYPE_STRING;
  if (strcmp (name, "text") == 0)
    return REPORT_FORMAT_PARAM_TYPE_TEXT;
  if (strcmp (name, "report_format_list") == 0)
    return REPORT_FORMAT_PARAM_TYPE_REPORT_FORMAT_LIST;
  return REPORT_FORMAT_PARAM_TYPE_ERROR;
}

/**
 * @brief Return whether a name is a backup file name.
 *
 * @param[in]  name  Name.
 *
 * @return 0 if normal file name, 1 if backup file name.
 */
static int
backup_file_name (const char *name)
{
  int length = strlen (name);

  if (length && (name[length - 1] == '~'))
    return 1;

  if ((length > 3)
      && (name[length - 4] == '.'))
    return ((name[length - 3] == 'b')
            && (name[length - 2] == 'a')
            && (name[length - 1] == 'k'))
           || ((name[length - 3] == 'B')
               && (name[length - 2] == 'A')
               && (name[length - 1] == 'K'))
           || ((name[length - 3] == 'C')
               && (name[length - 2] == 'K')
               && (name[length - 1] == 'P'));

  return 0;
}

/**
 * @brief Get files associated with a report format.
 *
 * @param[in]   dir_name  Location of files.
 * @param[out]  start     Files on success.
 *
 * @return 0 if successful, -1 otherwise.
 */
static int
get_report_format_files (const char *dir_name, GPtrArray **start)
{
  GPtrArray *files;
  struct dirent **names;
  int n, index;
  char *locale;

  files = g_ptr_array_new ();

  locale = setlocale (LC_ALL, "C");
  n = scandir (dir_name, &names, NULL, alphasort);
  setlocale (LC_ALL, locale);
  if (n < 0)
    {
      g_warning ("%s: failed to open dir %s: %s",
                 __FUNCTION__,
                 dir_name,
                 strerror (errno));
      return -1;
    }

  for (index = 0; index < n; index++)
    {
      if (strcmp (names[index]->d_name, ".")
          && strcmp (names[index]->d_name, "..")
          && (backup_file_name (names[index]->d_name) == 0))
        g_ptr_array_add (files, g_strdup (names[index]->d_name));
      free (names[index]);
    }
  free (names);

  g_ptr_array_add (files, NULL);

  *start = files;
  return 0;
}

/**
 * @brief Get the directory of a report format.
 *
 * @param[in]  uuid  Report format UUID.  NULL to get parent dir.
 *
 * @return Freshly allocated dir name.
 */
gchar *
predefined_report_format_dir (const gchar *uuid)
{
  return g_build_filename (GVMD_DATA_DIR,
                           "report_formats",
                           uuid,
                           NULL);
}

/**
 * @brief Initialise a report format file iterator.
 *
 * @param[in]  iterator       Iterator.
 * @param[in]  report_format  Single report format to iterate over, NULL for
 *                            all.
 *
 * @return 0 on success, -1 on error.
 */
int
init_report_format_file_iterator (file_iterator_t* iterator,
                                  report_format_t report_format)
{
  gchar *dir_name, *uuid;

  uuid = report_format_uuid (report_format);
  if (uuid == NULL)
    return -1;

  if (report_format_predefined (report_format))
    dir_name = predefined_report_format_dir (uuid);
  else
    {
      gchar *owner_uuid;

      owner_uuid = report_format_owner_uuid (report_format);
      if (owner_uuid == NULL)
        return -1;
      dir_name = g_build_filename (GVMD_STATE_DIR,
                                   "report_formats",
                                   owner_uuid,
                                   uuid,
                                   NULL);
      g_free (owner_uuid);
    }

  g_free (uuid);

  if (get_report_format_files (dir_name, &iterator->start))
    {
      g_free (dir_name);
      return -1;
    }

  iterator->current = iterator->start->pdata;
  iterator->current--;
  iterator->dir_name = dir_name;
  return 0;
}

/**
 * @brief Cleanup a report type iterator.
 *
 * @param[in]  iterator  Iterator.
 */
void
cleanup_file_iterator (file_iterator_t* iterator)
{
  array_free (iterator->start);
  g_free (iterator->dir_name);
}

/**
 * @brief Increment a report type iterator.
 *
 * The caller must stop using this after it returns FALSE.
 *
 * @param[in]  iterator  Task iterator.
 *
 * @return TRUE if there was a next item, else FALSE.
 */
gboolean
next_file (file_iterator_t* iterator)
{
  iterator->current++;
  if (*iterator->current == NULL) return FALSE;
  return TRUE;
}

/**
 * @brief Return the name from a file iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return File name.
 */
const char*
file_iterator_name (file_iterator_t* iterator)
{
  return (const char*) *iterator->current;
}

/**
 * @brief Return the file contents from a file iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Freshly allocated file contents, in base64.
 */
gchar*
file_iterator_content_64 (file_iterator_t* iterator)
{
  gchar *path_name, *content;
  GError *error;
  gsize content_size;

  path_name = g_build_filename (iterator->dir_name,
                                (gchar*) *iterator->current,
                                NULL);

  /* Read in the contents. */

  error = NULL;
  if (g_file_get_contents (path_name,
                           &content,
                           &content_size,
                           &error)
      == FALSE)
    {
      if (error)
        {
          g_debug ("%s: failed to read %s: %s",
                   __FUNCTION__, path_name, error->message);
          g_error_free (error);
        }
      g_free (path_name);
      return NULL;
    }

  g_free (path_name);

  /* Base64 encode the contents. */

  if (content && (content_size > 0))
    {
      gchar *base64 = g_base64_encode ((guchar*) content, content_size);
      g_free (content);
      return base64;
    }

  return content;
}


/* Slaves. */

/**
 * @brief Delete a task on a slave.
 *
 * @param[in]  host             Slave host.
 * @param[in]  port             Slave port.
 * @param[in]  username         Slave username.
 * @param[in]  password         Slave password.
 * @param[in]  slave_task_uuid  UUID of task on slave.
 *
 * @return 0 success, -1 error.
 */
int
delete_slave_task (const gchar *host, int port, const gchar *username,
                   const gchar *password, const char *slave_task_uuid)
{
  int socket;
  gnutls_session_t session;
  entity_t get_tasks, get_targets, entity, task, credential, port_list;
  const char *slave_config_uuid, *slave_target_uuid, *slave_port_list_uuid;
  const char *slave_ssh_credential_uuid, *slave_smb_credential_uuid;

  gmp_delete_opts_t del_opts = gmp_delete_opts_ultimate_defaults;

  /* Connect to the slave. */

  g_debug ("   %s: host: %s", __FUNCTION__, host);

  socket = gvm_server_open (&session, host, port);
  if (socket == -1) return -1;

  g_debug ("   %s: connected", __FUNCTION__);

  /* Authenticate using the slave login. */

  if (gmp_authenticate (&session, username, password))
    return -1;

  g_debug ("   %s: authenticated", __FUNCTION__);

  /* Get the UUIDs of the slave resources. */

  if (gmp_get_tasks (&session, slave_task_uuid, 0, 0, &get_tasks))
    goto fail;

  task = entity_child (get_tasks, "task");
  if (task == NULL)
    goto fail_free_task;

  entity = entity_child (task, "config");
  if (entity == NULL)
    goto fail_free_task;
  slave_config_uuid = entity_attribute (entity, "id");

  entity = entity_child (task, "target");
  if (entity == NULL)
    goto fail_free_task;
  slave_target_uuid = entity_attribute (entity, "id");

  if (gmp_get_targets (&session, slave_target_uuid, 0, 0, &get_targets))
    goto fail_free_task;

  entity = entity_child (get_targets, "target");
  if (entity == NULL)
    goto fail_free;

  port_list = entity_child (entity, "port_list");
  if (port_list == NULL)
    goto fail_free;
  slave_port_list_uuid = entity_attribute (port_list, "id");

  credential = entity_child (entity, "ssh_credential");
  if (credential == NULL)
    goto fail_free;
  slave_ssh_credential_uuid = entity_attribute (credential, "id");

  credential = entity_child (entity, "smb_credential");
  if (credential == NULL)
    goto fail_free;
  slave_smb_credential_uuid = entity_attribute (credential, "id");

  /* Remove the slave resources. */

  gmp_stop_task (&session, slave_task_uuid);
  if (gmp_delete_task_ext (&session, slave_task_uuid, del_opts))
    goto fail_config;
  if (gmp_delete_config_ext (&session, slave_config_uuid, del_opts))
    goto fail_target;
  if (gmp_delete_target_ext (&session, slave_target_uuid, del_opts))
    goto fail_credential;
  if (gmp_delete_port_list_ext (&session, slave_port_list_uuid, del_opts))
    goto fail_credential;
  if (gmp_delete_lsc_credential_ext (&session, slave_smb_credential_uuid,
                                     del_opts))
    goto fail;
  if (gmp_delete_lsc_credential_ext (&session, slave_ssh_credential_uuid,
                                     del_opts))
    goto fail;

  /* Cleanup. */

  free_entity (get_targets);
  free_entity (get_tasks);
  gvm_server_close (socket, session);
  return 0;

 fail_config:
  gmp_delete_config_ext (&session, slave_config_uuid, del_opts);
 fail_target:
  gmp_delete_target_ext (&session, slave_target_uuid, del_opts);
  gmp_delete_port_list_ext (&session, slave_port_list_uuid, del_opts);
 fail_credential:
  gmp_delete_lsc_credential_ext (&session, slave_smb_credential_uuid, del_opts);
  gmp_delete_lsc_credential_ext (&session, slave_ssh_credential_uuid, del_opts);
 fail_free:
  free_entity (get_targets);
 fail_free_task:
  free_entity (get_tasks);
 fail:
  gvm_server_close (socket, session);
  return -1;
}

/**
 * @brief Return the path to the CPE dictionary.
 *
 * @return A dynamically allocated string (to be g_free'd) containing the
 *         path to the desired file.
 */
static char *
get_cpe_filename ()
{
  return g_strdup (CPE_DICT_FILENAME);
}

/**
 * @brief Compute the filename where a given CVE can be found.
 *
 * @param[in] item_id   Full CVE identifier ("CVE-YYYY-ZZZZ").
 *
 * @return A dynamically allocated string (to be g_free'd) containing the
 *         path to the desired file or NULL on error.
 */
static char *
get_cve_filename (char *item_id)
{
  int year;

  if (sscanf (item_id, "%*3s-%d-%*d", &year) == 1)
    {
      /* CVEs before 2002 are stored in the 2002 file. */
      if (year <= 2002)
        year = 2002;
      return g_strdup_printf (CVE_FILENAME_FMT, year);
    }
  return NULL;
}

/**
 * @brief Get the filename where a given OVAL definition can be found.
 *
 * @param[in] item_id   Full OVAL identifier with file suffix.
 *
 * @return A dynamically allocated string (to be g_free'd) containing the
 *         path to the desired file or NULL on error.
 */
static char *
get_ovaldef_filename (char *item_id)
{
  char *result, *short_filename;

  result = NULL;
  short_filename = get_ovaldef_short_filename (item_id);

  if (*short_filename)
    {
      result = g_strdup_printf ("%s/%s", GVM_SCAP_DATA_DIR, short_filename);
    }
  free (short_filename);

  return result;
}

/**
 * @brief Compute the filename where a given CERT-Bund Advisory can be found.
 *
 * @param[in] item_id   CERT-Bund identifier without version ("CB-K??/????").
 *
 * @return A dynamically allocated string (to be g_free'd) containing the
 *         path to the desired file or NULL on error.
 */
static char *
get_cert_bund_adv_filename (char *item_id)
{
  int year;

  if (sscanf (item_id, "CB-K%d-%*s", &year) == 1)
    {
      return g_strdup_printf (CERT_BUND_ADV_FILENAME_FMT, year);
    }
  return NULL;
}

/**
 * @brief Compute the filename where a given DFN-CERT Advisory can be found.
 *
 * @param[in] item_id   Full DFN-CERT identifier ("DFN-CERT-YYYY-ZZZZ").
 *
 * @return A dynamically allocated string (to be g_free'd) containing the
 *         path to the desired file or NULL on error.
 */
static char *
get_dfn_cert_adv_filename (char *item_id)
{
  int year;

  if (sscanf (item_id, "DFN-CERT-%d-%*s", &year) == 1)
    {
      return g_strdup_printf (DFN_CERT_ADV_FILENAME_FMT, year);
    }
  return NULL;
}

/**
 * @brief Run xsltproc in an external process.
 *
 * @param[in] stylesheet    XSL stylesheet to use.
 * @param[in] xmlfile       XML file to process.
 * @param[in] param_names   NULL terminated array of stringparam names (can
 *                          be NULL).
 * @param[in] param_values  NULL terminated array of stringparam values (can
 *                          be NULL).
 *
 * @return A dynamically allocated (to be g_free'd) string containing the
 *         result of the operation of NULL on failure.
 */
static gchar *
xsl_transform (gchar *stylesheet, gchar *xmlfile, gchar **param_names,
               gchar **param_values)
{
  int i, param_idx;
  gchar **cmd, *cmd_full;
  gint exit_status;
  gboolean success;
  gchar *standard_out = NULL, *standard_err = NULL;

  param_idx = 0;
  if (param_names && param_values)
    while (param_names[param_idx] && param_values[param_idx])
      param_idx++;

  cmd = (gchar **)g_malloc ((4 + param_idx * 3) * sizeof (gchar *));

  i = 0;
  cmd[i++] = "xsltproc";
  if (param_idx)
    {
      int j;

      for (j = 0; j < param_idx; j++)
        {
          cmd[i++] = "--stringparam";
          cmd[i++] = param_names[j];
          cmd[i++] = param_values[j];
        }
    }
  cmd[i++] = stylesheet;
  cmd[i++] = xmlfile;
  cmd[i] = NULL;


  /* DEBUG: display the final command line. */
  cmd_full = g_strjoinv (" ", cmd);
  g_debug ("%s: Spawning in parent dir: %s",
           __FUNCTION__, cmd_full);
  g_free (cmd_full);
  /* --- */

  if ((g_spawn_sync (NULL,
                     cmd,
                     NULL,                  /* Environment. */
                     G_SPAWN_SEARCH_PATH,
                     NULL,                  /* Setup function. */
                     NULL,
                     &standard_out,
                     &standard_err,
                     &exit_status,
                     NULL)
       == FALSE)
      || (WIFEXITED (exit_status) == 0)
      || WEXITSTATUS (exit_status))
    {
      g_debug ("%s: failed to transform the xml: %d (WIF %i, WEX %i)",
               __FUNCTION__,
               exit_status,
               WIFEXITED (exit_status),
               WEXITSTATUS (exit_status));
      g_debug ("%s: stderr: %s", __FUNCTION__, standard_err);
      g_debug ("%s: stdout: %s", __FUNCTION__, standard_out);
      success = FALSE;
    }
  else if (strlen (standard_out) == 0)
    success = FALSE; /* execution succeeded but nothing was found */
  else
    success = TRUE; /* execution succeeded and we have a result */

  /* Cleanup. */
  g_free (cmd);
  g_free (standard_err);

  if (success)
    return standard_out;

  g_free (standard_out);
  return NULL;
}

/**
 * @brief Define a code snippet for get_nvti_xml.
 *
 * @param  x  Prefix for names in snippet.
 */
#define DEF(x)                                                    \
      const char* x = nvt_iterator_ ## x (nvts);                  \
      gchar* x ## _text = x                                       \
                          ? g_markup_escape_text (x, -1)          \
                          : g_strdup ("");

/**
 * @brief Create and return XML description for an NVT.
 *
 * @param[in]  nvts        The NVT.
 * @param[in]  details     If true, detailed XML, else simple XML.
 * @param[in]  pref_count  Preference count.  Used if details is true.
 * @param[in]  preferences If true, included preferences.
 * @param[in]  timeout     Timeout.  Used if details is true.
 * @param[in]  config      Config, used if preferences is true.
 * @param[in]  close_tag   Whether to close the NVT tag or not.
 *
 * @return A dynamically allocated string containing the XML description.
 */
gchar *
get_nvti_xml (iterator_t *nvts, int details, int pref_count,
              int preferences, const char *timeout, config_t config,
              int close_tag)
{
  const char* oid = nvt_iterator_oid (nvts);
  const char* name = nvt_iterator_name (nvts);
  gchar *msg, *name_text;

  name_text = name
               ? g_markup_escape_text (name, strlen (name))
               : g_strdup ("");
  if (details)
    {
      int tag_count;
      GString *refs_str, *tags_str, *buffer, *nvt_tags;
      iterator_t cert_refs_iterator, tags;
      gchar *tag_name_esc, *tag_value_esc, *tag_comment_esc;
      char *default_timeout = nvt_default_timeout (oid);

      DEF (family);
      DEF (tag);

#undef DEF

      nvt_tags = g_string_new (tag_text);
      g_free (tag_text);

      /* Add the elements that are expected as part of the pipe-separated tag list
       * via API although internally already explicitly stored. Once the API is
       * extended to have these elements explicitly, they do not need to be
       * added to this tag string anymore. */
      if (nvt_iterator_summary (nvts) && nvt_iterator_summary (nvts)[0])
        {
          if (nvt_tags->str)
            xml_string_append (nvt_tags, "|summary=%s",
                               nvt_iterator_summary (nvts));
          else
            xml_string_append (nvt_tags, "summary=%s",
                               nvt_iterator_summary (nvts));
        }
      if (nvt_iterator_insight (nvts) && nvt_iterator_insight (nvts)[0])
        {
          if (nvt_tags->str)
            xml_string_append (nvt_tags, "|insight=%s",
                               nvt_iterator_insight (nvts));
          else
            xml_string_append (nvt_tags, "insight=%s",
                               nvt_iterator_insight (nvts));
        }
      if (nvt_iterator_affected (nvts) && nvt_iterator_affected (nvts)[0])
        {
          if (nvt_tags->str)
            xml_string_append (nvt_tags, "|affected=%s",
                               nvt_iterator_affected (nvts));
          else
            xml_string_append (nvt_tags, "affected=%s",
                               nvt_iterator_affected (nvts));
        }
      if (nvt_iterator_impact (nvts) && nvt_iterator_impact (nvts)[0])
        {
          if (nvt_tags->str)
            xml_string_append (nvt_tags, "|impact=%s",
                               nvt_iterator_impact (nvts));
          else
            xml_string_append (nvt_tags, "impact=%s",
                               nvt_iterator_impact (nvts));
        }
      if (nvt_iterator_solution (nvts) && nvt_iterator_solution (nvts)[0])
        {
          if (nvt_tags->str)
            xml_string_append (nvt_tags, "|solution=%s",
                                nvt_iterator_solution (nvts));
          else
            xml_string_append (nvt_tags, "solution=%s",
                               nvt_iterator_solution (nvts));
        }
      if (nvt_iterator_solution_type (nvts)
          && nvt_iterator_solution_type (nvts)[0])
        {
          if (nvt_tags->str)
            xml_string_append (nvt_tags, "|solution_type=%s",
                               nvt_iterator_solution_type (nvts));
          else
            xml_string_append (nvt_tags, "solution_type=%s",
                               nvt_iterator_solution_type (nvts));
        }
      if (nvt_iterator_detection (nvts) && nvt_iterator_detection (nvts)[0])
        {
          if (nvt_tags->str)
            xml_string_append (nvt_tags, "|vuldetect=%s",
                               nvt_iterator_detection (nvts));
          else
            xml_string_append (nvt_tags, "vuldetect=%s",
                               nvt_iterator_detection (nvts));
        }

      refs_str = g_string_new ("");

      if (manage_cert_loaded())
        {
          init_nvt_cert_bund_adv_iterator (&cert_refs_iterator, oid, 0, 0);
          while (next (&cert_refs_iterator))
            {
              xml_string_append (refs_str,
                                 "<ref type=\"cert-bund\" id=\"%s\"/>",
                                 get_iterator_name (&cert_refs_iterator));
          }
          cleanup_iterator (&cert_refs_iterator);

          init_nvt_dfn_cert_adv_iterator (&cert_refs_iterator, oid, 0, 0);
          while (next (&cert_refs_iterator))
            {
              xml_string_append (refs_str,
                                 "<ref type=\"dfn-cert\" id=\"%s\"/>",
                                 get_iterator_name (&cert_refs_iterator));
          }
          cleanup_iterator (&cert_refs_iterator);
        }
      else
        {
          g_string_append (refs_str,
                           "<warning>database not available</warning>");
        }

      nvti_refs_append_xml (refs_str, oid, NULL);

      tags_str = g_string_new ("");
      tag_count = resource_tag_count ("nvt",
                                      get_iterator_resource (nvts),
                                      1);

      if (tag_count)
        {
          g_string_append_printf (tags_str,
                                  "<user_tags>"
                                  "<count>%i</count>",
                                  tag_count);

          init_resource_tag_iterator (&tags, "nvt",
                                      get_iterator_resource (nvts),
                                      1, NULL, 1);
          while (next (&tags))
            {
              tag_name_esc = g_markup_escape_text (resource_tag_iterator_name
                                                    (&tags),
                                                  -1);
              tag_value_esc = g_markup_escape_text (resource_tag_iterator_value
                                                      (&tags),
                                                    -1);
              tag_comment_esc = g_markup_escape_text (resource_tag_iterator_comment
                                                        (&tags),
                                                      -1);
              g_string_append_printf (tags_str,
                                      "<tag id=\"%s\">"
                                      "<name>%s</name>"
                                      "<value>%s</value>"
                                      "<comment>%s</comment>"
                                      "</tag>",
                                      resource_tag_iterator_uuid (&tags),
                                      tag_name_esc,
                                      tag_value_esc,
                                      tag_comment_esc);
              g_free (tag_name_esc);
              g_free (tag_value_esc);
              g_free (tag_comment_esc);
            }
          cleanup_iterator (&tags);
          g_string_append_printf (tags_str,
                                  "</user_tags>");
        }

      buffer = g_string_new ("");

      g_string_append_printf (buffer,
                              "<nvt oid=\"%s\">"
                              "<name>%s</name>"
                              "<creation_time>%s</creation_time>"
                              "<modification_time>%s</modification_time>"
                              "%s" // user_tags
                              "<category>%d</category>"
                              "<family>%s</family>"
                              "<cvss_base>%s</cvss_base>"
                              "<qod>"
                              "<value>%s</value>"
                              "<type>%s</type>"
                              "</qod>"
                              "<refs>%s</refs>"
                              "<tags>%s</tags>"
                              "<preference_count>%i</preference_count>"
                              "<timeout>%s</timeout>"
                              "<default_timeout>%s</default_timeout>",
                              oid,
                              name_text,
                              get_iterator_creation_time (nvts)
                               ? get_iterator_creation_time (nvts)
                               : "",
                              get_iterator_modification_time (nvts)
                               ? get_iterator_modification_time (nvts)
                               : "",
                              tags_str->str,
                              nvt_iterator_category (nvts),
                              family_text,
                              nvt_iterator_cvss_base (nvts)
                               ? nvt_iterator_cvss_base (nvts)
                               : "",
                              nvt_iterator_qod (nvts),
                              nvt_iterator_qod_type (nvts),
                              refs_str->str,
                              nvt_tags->str,
                              pref_count,
                              timeout ? timeout : "",
                              default_timeout ? default_timeout : "");
      g_free (family_text);
      g_string_free(nvt_tags, 1);
      g_string_free(refs_str, 1);
      g_string_free(tags_str, 1);

      if (preferences)
        {
          iterator_t prefs;
          const char *nvt_oid = nvt_iterator_oid (nvts);

          /* Send the preferences for the NVT. */

          xml_string_append (buffer,
                             "<preferences>"
                             "<timeout>%s</timeout>"
                             "<default_timeout>%s</default_timeout>",
                             timeout ? timeout : "",
                             default_timeout ? default_timeout : "");

          init_nvt_preference_iterator (&prefs, nvt_oid);
          while (next (&prefs))
            buffer_config_preference_xml (buffer, &prefs, config, 1);
          cleanup_iterator (&prefs);

          xml_string_append (buffer, "</preferences>");
        }

      xml_string_append (buffer, close_tag ? "</nvt>" : "");
      msg = g_string_free (buffer, FALSE);
      free (default_timeout);
    }
  else
    {
      int tag_count;
      tag_count = resource_tag_count ("nvt",
                                      get_iterator_resource (nvts),
                                      1);

      if (tag_count)
        {
          msg = g_strdup_printf
                 ("<nvt oid=\"%s\"><name>%s</name>"
                  "<user_tags><count>%i</count></user_tags>%s",
                  oid, name_text,
                  tag_count,
                  close_tag ? "</nvt>" : "");
        }
      else
        {
          msg = g_strdup_printf
                 ("<nvt oid=\"%s\"><name>%s</name>%s",
                  oid, name_text,
                  close_tag ? "</nvt>" : "");
        }
    }
  g_free (name_text);
  return msg;
}

/**
 * @brief GET SCAP update time, as a string.
 *
 * @return Last update time as a static string, or "" on error.
 */
const char *
manage_scap_update_time ()
{
  gchar *content;
  GError *error;
  gsize content_size;
  struct tm update_time;

  /* Read in the contents. */

  error = NULL;
  if (g_file_get_contents (SCAP_TIMESTAMP_FILENAME,
                           &content,
                           &content_size,
                           &error)
      == FALSE)
    {
      if (error)
        {
          g_debug ("%s: failed to read %s: %s",
                   __FUNCTION__, SCAP_TIMESTAMP_FILENAME, error->message);
          g_error_free (error);
        }
      return "";
    }

  memset (&update_time, 0, sizeof (struct tm));
  if (strptime (content, "%Y%m%d%H%M", &update_time))
    {
      static char time_string[100];
      strftime (time_string, 99, "%FT%T.000%z", &update_time);
      return time_string;
    }
  return "";
}

/**
 * @brief Read raw information.
 *
 * @param[in]   type    Type of the requested information.
 * @param[in]   uid     Unique identifier of the requested information
 * @param[in]   name    Name or identifier of the requested information.
 * @param[out]  result  Pointer to the read information location. Will point
 *                      to NULL on error.
 *
 * @return 1 success, -1 error.
 */
int
manage_read_info (gchar *type, gchar *uid, gchar *name, gchar **result)
{
  gchar *fname;
  gchar *pnames[2] = { "refname", NULL };
  gchar *pvalues[2] = { name, NULL };

  assert (result != NULL);
  *result = NULL;

  if (g_ascii_strcasecmp ("CPE", type) == 0)
    {
      fname = get_cpe_filename ();
      if (fname)
        {
          gchar *cpe;
          cpe = xsl_transform (CPE_GETBYNAME_XSL, fname, pnames, pvalues);
          g_free (fname);
          if (cpe)
            *result = cpe;
        }
    }
  else if (g_ascii_strcasecmp ("CVE", type) == 0)
    {
      fname = get_cve_filename (uid);
      if (fname)
        {
          gchar *cve;
          cve = xsl_transform (CVE_GETBYNAME_XSL, fname, pnames, pvalues);
          g_free (fname);
          if (cve)
            *result = cve;
        }
    }
  else if (g_ascii_strcasecmp ("NVT", type) == 0)
    {
      iterator_t nvts;
      nvt_t nvt;

      if (!find_nvt (uid ? uid : name, &nvt) && nvt)
        {
          init_nvt_iterator (&nvts, nvt, 0, NULL, NULL, 0, NULL);

          if (next (&nvts))
            *result = get_nvti_xml (&nvts,
                                    1,    /* Include details. */
                                    0,    /* Preference count. */
                                    1,    /* Include preferences. */
                                    NULL, /* Timeout. */
                                    0,    /* Config. */
                                    1);   /* Close tag. */

          cleanup_iterator (&nvts);
        }
    }
  else if (g_ascii_strcasecmp ("OVALDEF", type) == 0)
    {
      fname = get_ovaldef_filename (uid);
      if (fname)
        {
          gchar *ovaldef;
          ovaldef = xsl_transform (OVALDEF_GETBYNAME_XSL, fname,
                                   pnames, pvalues);
          g_free (fname);
          if (ovaldef)
            *result = ovaldef;
        }
    }
  else if (g_ascii_strcasecmp ("CERT_BUND_ADV", type) == 0)
    {
      fname = get_cert_bund_adv_filename (uid);
      if (fname)
        {
          gchar *adv;
          adv = xsl_transform (CERT_BUND_ADV_GETBYNAME_XSL, fname,
                               pnames, pvalues);
          g_free (fname);
          if (adv)
            *result = adv;
        }
    }
  else if (g_ascii_strcasecmp ("DFN_CERT_ADV", type) == 0)
    {
      fname = get_dfn_cert_adv_filename (uid);
      if (fname)
        {
          gchar *adv;
          adv = xsl_transform (DFN_CERT_ADV_GETBYNAME_XSL, fname,
                               pnames, pvalues);
          g_free (fname);
          if (adv)
            *result = adv;
        }
    }

  if (*result == NULL)
    return -1;

  return 1;
}


/* Users. */

/**
 *
 * @brief Validates a username.
 *
 * @param[in]  name  The name.
 *
 * @return 0 if the username is valid, 1 if not.
 */
int
validate_username (const gchar * name)
{
  if (g_regex_match_simple ("^[[:alnum:]-_.]+$", name, 0, 0))
    return 0;
  else
    return 1;
}


/* Resource aggregates. */

/**
 * @brief Free a sort_data_t struct and its related resources.
 *
 * @param[in] sort_data  The sort_data struct to free.
 */
void
sort_data_free (sort_data_t *sort_data)
{
  g_free (sort_data->field);
  g_free (sort_data->stat);
  g_free (sort_data);
}


/* Feeds. */

/**
 * @brief Request a feed synchronization script selftest.
 *
 * Ask a feed synchronization script to perform a selftest and report
 * the results.
 *
 * @param[in]   sync_script  The file name of the synchronization script.
 * @param[out]  result       Return location for selftest errors, or NULL.
 *
 * @return TRUE if the selftest was successful, or FALSE if an error occurred.
 */
gboolean
gvm_sync_script_perform_selftest (const gchar * sync_script,
                                  gchar ** result)
{
  g_assert (sync_script);
  g_assert_cmpstr (*result, ==, NULL);

  gchar *script_working_dir = g_path_get_dirname (sync_script);

  gchar **argv = (gchar **) g_malloc (3 * sizeof (gchar *));
  argv[0] = g_strdup (sync_script);
  argv[1] = g_strdup ("--selftest");
  argv[2] = NULL;

  gchar *script_out;
  gchar *script_err;
  gint script_exit;
  GError *error = NULL;

  if (!g_spawn_sync
      (script_working_dir, argv, NULL, 0, NULL, NULL, &script_out, &script_err,
       &script_exit, &error))
    {
      if (*result != NULL)
        {
          *result =
            g_strdup_printf ("Failed to execute synchronization " "script: %s",
                             error->message);
        }

      g_free (script_working_dir);
      g_strfreev (argv);
      g_free (script_out);
      g_free (script_err);
      g_error_free (error);

      return FALSE;
    }

  if (script_exit != 0)
    {
      if (script_err != NULL)
        {
          *result = g_strdup_printf ("%s", script_err);
        }

      g_free (script_working_dir);
      g_strfreev (argv);
      g_free (script_out);
      g_free (script_err);

      return FALSE;
    }

  g_free (script_working_dir);
  g_strfreev (argv);
  g_free (script_out);
  g_free (script_err);

  return TRUE;
}

/**
 * @brief Retrieves the ID string of a feed sync script, with basic validation.
 *
 * @param[in]   sync_script     The file name of the synchronization script.
 * @param[out]  identification  Return location of the identification string.
 * @param[in]   feed_type       Could be NVT_FEED, SCAP_FEED or CERT_FEED.
 *
 * @return TRUE if the identification string was retrieved, or FALSE if an
 *         error occurred.
 */
gboolean
gvm_get_sync_script_identification (const gchar * sync_script,
                                    gchar ** identification,
                                    int feed_type)
{
  g_assert (sync_script);
  if (identification)
    g_assert_cmpstr (*identification, ==, NULL);

  gchar *script_working_dir = g_path_get_dirname (sync_script);

  gchar **argv = (gchar **) g_malloc (3 * sizeof (gchar *));
  argv[0] = g_strdup (sync_script);
  argv[1] = g_strdup ("--identify");
  argv[2] = NULL;

  gchar *script_out;
  gchar *script_err;
  gint script_exit;
  GError *error = NULL;

  gchar **script_identification;

  if (!g_spawn_sync
        (script_working_dir, argv, NULL, 0, NULL, NULL, &script_out, &script_err,
         &script_exit, &error))
    {
      g_warning ("Failed to execute %s: %s", sync_script, error->message);

      g_free (script_working_dir);
      g_strfreev (argv);
      g_free (script_out);
      g_free (script_err);
      g_error_free (error);

      return FALSE;
    }

  if (script_exit != 0)
    {
      g_warning ("%s returned a non-zero exit code.", sync_script);

      g_free (script_working_dir);
      g_strfreev (argv);
      g_free (script_out);
      g_free (script_err);

      return FALSE;
    }

  script_identification = g_strsplit (script_out, "|", 6);

  if ((script_identification[0] == NULL)
      || (feed_type == NVT_FEED
          && g_ascii_strncasecmp (script_identification[0], "NVTSYNC", 7))
      || (feed_type == SCAP_FEED
          && g_ascii_strncasecmp (script_identification[0], "SCAPSYNC", 7))
      || (feed_type == CERT_FEED
          && g_ascii_strncasecmp (script_identification[0], "CERTSYNC", 7))
      || g_ascii_strncasecmp (script_identification[0], script_identification[5], 7))
    {
      g_warning ("%s is not a feed synchronization script", sync_script);

      g_free (script_working_dir);
      g_strfreev (argv);
      g_free (script_out);
      g_free (script_err);

      g_strfreev (script_identification);

      return FALSE;
    }

  if (identification)
    *identification = g_strdup (script_out);

  g_free (script_working_dir);
  g_strfreev (argv);
  g_free (script_out);
  g_free (script_err);

  g_strfreev (script_identification);

  return TRUE;
}

/**
 * @brief Retrieves description of a feed sync script, with basic validation.
 *
 * @param[in]   sync_script  The file name of the synchronization script.
 * @param[out]  description  Return location of the description string.
 *
 * @return TRUE if the description was retrieved, or FALSE if an error
 *         occurred.
 */
gboolean
gvm_get_sync_script_description (const gchar * sync_script,
                                 gchar ** description)
{
  g_assert (sync_script);
  g_assert_cmpstr (*description, ==, NULL);

  gchar *script_working_dir = g_path_get_dirname (sync_script);

  gchar **argv = (gchar **) g_malloc (3 * sizeof (gchar *));
  argv[0] = g_strdup (sync_script);
  argv[1] = g_strdup ("--describe");
  argv[2] = NULL;

  gchar *script_out;
  gchar *script_err;
  gint script_exit;
  GError *error = NULL;

  if (!g_spawn_sync
      (script_working_dir, argv, NULL, 0, NULL, NULL, &script_out, &script_err,
       &script_exit, &error))
    {
      g_warning ("Failed to execute %s: %s", sync_script, error->message);

      g_free (script_working_dir);
      g_strfreev (argv);
      g_free (script_out);
      g_free (script_err);
      g_error_free (error);

      return FALSE;
    }

  if (script_exit != 0)
    {
      g_warning ("%s returned a non-zero exit code.", sync_script);

      g_free (script_working_dir);
      g_strfreev (argv);
      g_free (script_out);
      g_free (script_err);

      return FALSE;
    }

  *description = g_strdup (script_out);

  g_free (script_working_dir);
  g_strfreev (argv);
  g_free (script_out);
  g_free (script_err);

  return TRUE;
}

/**
 * @brief Retrieves the version of a feed handled by the sync, with basic
 * validation.
 *
 * @param[in]   sync_script  The file name of the synchronization script.
 * @param[out]  feed_version  Return location of the feed version string.
 *
 * @return TRUE if the feed version was retrieved, or FALSE if an error
 *         occurred.
 */
gboolean
gvm_get_sync_script_feed_version (const gchar * sync_script,
                                  gchar ** feed_version)
{
  g_assert (sync_script);
  g_assert_cmpstr (*feed_version, ==, NULL);

  gchar *script_working_dir = g_path_get_dirname (sync_script);

  gchar **argv = (gchar **) g_malloc (3 * sizeof (gchar *));
  argv[0] = g_strdup (sync_script);
  argv[1] = g_strdup ("--feedversion");
  argv[2] = NULL;

  gchar *script_out;
  gchar *script_err;
  gint script_exit;
  GError *error = NULL;

  if (!g_spawn_sync
        (script_working_dir, argv, NULL, 0, NULL, NULL, &script_out, &script_err,
         &script_exit, &error))
    {
      g_warning ("Failed to execute %s: %s", sync_script, error->message);

      g_free (script_working_dir);
      g_strfreev (argv);
      g_free (script_out);
      g_free (script_err);
      g_error_free (error);

      return FALSE;
    }

  if (script_exit != 0)
    {
      g_warning ("%s returned a non-zero exit code.", sync_script);

      g_free (script_working_dir);
      g_strfreev (argv);
      g_free (script_out);
      g_free (script_err);

      return FALSE;
    }

  *feed_version = g_strdup (script_out);

  g_free (script_working_dir);
  g_strfreev (argv);
  g_free (script_out);
  g_free (script_err);

  return TRUE;
}

/**
 * @brief Migrates SCAP or CERT database, waiting until migration terminates.
 *
 * Calls a sync script to migrate the SCAP or CERT database.
 *
 * @param[in]  feed_type     Could be SCAP_FEED or CERT_FEED.
 *
 * @return 0 sync complete, 1 sync already in progress, -1 error
 */
int
gvm_migrate_secinfo (int feed_type)
{
  int lockfile, ret;
  gchar *lockfile_name;
  const gchar *lockfile_basename;

  if (feed_type == SCAP_FEED)
    lockfile_basename = "gvm-sync-scap";
  else if (feed_type == CERT_FEED)
    lockfile_basename = "gvm-sync-cert";
  else
    {
      g_warning ("%s: unsupported feed_type", __FUNCTION__);
      return -1;
    }

  /* Open the lock file. */

  lockfile_name = g_build_filename (g_get_tmp_dir (), lockfile_basename, NULL);

  lockfile = open (lockfile_name, O_RDWR | O_CREAT | O_APPEND,
                   /* "-rw-r--r--" */
                   S_IWUSR | S_IRUSR | S_IROTH | S_IRGRP);
  if (lockfile == -1)
    {
      g_warning ("Failed to open lock file '%s': %s", lockfile_name,
                 strerror (errno));
      g_free (lockfile_name);
      return -1;
    }

  if (flock (lockfile, LOCK_EX | LOCK_NB))  /* Exclusive, Non blocking. */
    {
      if (errno == EWOULDBLOCK)
        {
          if (close (lockfile))
            g_warning ("%s: failed to close lockfile: %s",
                       __FUNCTION__,
                       strerror (errno));
          g_free (lockfile_name);
          return 1;
        }
      g_debug ("%s: flock: %s", __FUNCTION__, strerror (errno));
      if (close (lockfile))
        g_warning ("%s: failed to close lockfile: %s",
                   __FUNCTION__,
                   strerror (errno));
      g_free (lockfile_name);
      return -1;
    }

  if (feed_type == SCAP_FEED)
    ret = check_scap_db_version ();
  else
    ret = check_cert_db_version ();

  /* Close the lock file. */

  if (close (lockfile))
    {
      g_free (lockfile_name);
      g_warning ("Failed to close lock file: %s", strerror (errno));
      return -1;
    }

  g_free (lockfile_name);

  return ret;
}

/**
 * @brief Update NVT cache using OSP.
 *
 * @param[in]  update_socket  Socket to use to contact ospd-openvas scanner.
 *
 * @return 0 success, -1 error, 2 scanner still loading.
 */
int
manage_update_nvts_osp (const gchar *update_socket)
{
  return manage_update_nvt_cache_osp (update_socket);
}


/* Wizards. */

/**
 * @brief Run a wizard.
 *
 * @param[in]  wizard_name       Wizard name.
 * @param[in]  run_command       Function to run GMP command.
 * @param[in]  run_command_data  Argument for run_command.
 * @param[in]  params            Wizard params.  Array of name_value_t.
 * @param[in]  read_only         Whether to only allow wizards marked as
 *                               read only.
 * @param[in]  mode              Name of the mode to run the wizard in.
 * @param[out] command_error     Either NULL or an address for an error message
 *                               when return is 0, 4 or 6.
 * @param[out] command_error_code  Either NULL or an address for a status code
 *                                 from the failed command when return is 0
 *                                 or 4.
 * @param[out] ret_response      Address for response string of last command.
 *
 * @return 0 success,
 *         1 name error,
 *         4 command in wizard failed,
 *         5 wizard not read only,
 *         6 Parameter validation failed,
 *         -1 internal error,
 *         99 permission denied.
 */
int
manage_run_wizard (const gchar *wizard_name,
                   int (*run_command) (void*, gchar*, gchar**),
                   void *run_command_data,
                   array_t *params,
                   int read_only,
                   const char *mode,
                   gchar **command_error,
                   gchar **command_error_code,
                   gchar **ret_response)
{
  GString *params_xml;
  gchar *file, *file_name, *response, *extra, *extra_wrapped, *wizard;
  gsize wizard_len;
  GError *get_error;
  entity_t entity, mode_entity, params_entity, read_only_entity;
  entity_t param_def, step;
  entities_t modes, steps, param_defs;
  int ret;
  const gchar *point;

  if (acl_user_may ("run_wizard") == 0)
    return 99;

  if (command_error)
    *command_error = NULL;

  if (command_error_code)
    *command_error_code = NULL;

  if (ret_response)
    *ret_response = NULL;

  point = wizard_name;
  while (*point && (isalnum (*point) || *point == '_')) point++;
  if (*point)
    return 1;

  /* Read wizard from file. */

  file_name = g_strdup_printf ("%s.xml", wizard_name);
  file = g_build_filename (GVMD_DATA_DIR,
                           "wizards",
                           file_name,
                           NULL);
  g_free (file_name);

  get_error = NULL;
  g_file_get_contents (file,
                       &wizard,
                       &wizard_len,
                       &get_error);
  g_free (file);
  if (get_error)
    {
      g_warning ("%s: Failed to read wizard: %s",
                 __FUNCTION__,
                 get_error->message);
      g_error_free (get_error);
      return -1;
    }

  /* Parse wizard. */

  entity = NULL;
  if (parse_entity (wizard, &entity))
    {
      g_warning ("%s: Failed to parse wizard", __FUNCTION__);
      g_free (wizard);
      return -1;
    }
  g_free (wizard);

  /* Select mode */
  if (mode && strcmp (mode, ""))
    {
      modes = entity->entities;
      int mode_found = 0;
      while (mode_found == 0 && (mode_entity = first_entity (modes)))
        {
          if (strcasecmp (entity_name (mode_entity), "mode") == 0)
            {
              entity_t name_entity;
              name_entity = entity_child (mode_entity, "name");

              if (strcmp (entity_text (name_entity), mode) == 0)
                mode_found = 1;
            }
          modes = next_entities (modes);
        }

      if (mode_found == 0)
        {
          free_entity (entity);
          if (ret_response)
            *ret_response = g_strdup ("");

          return 0;
        }
    }
  else
    {
      mode_entity = entity;
    }

  /* If needed, check if wizard is marked as read only.
   * This does not check the actual commands.
   */
  if (read_only)
    {
      read_only_entity = entity_child (mode_entity, "read_only");
      if (read_only_entity == NULL)
        {
          free_entity (entity);
          return 5;
        }
    }

  /* Check params */
  params_xml = g_string_new ("");
  params_entity = entity_child (mode_entity, "params");
  if (params_entity)
    param_defs = params_entity->entities;

  while (params_entity && (param_def = first_entity (param_defs)))
    {
      if (strcasecmp (entity_name (param_def), "param") == 0)
        {
          entity_t name_entity, regex_entity, optional_entity;
          const char *name, *regex;
          int optional;
          int param_found = 0;

          name_entity = entity_child (param_def, "name");
          if ((name_entity == NULL)
              || (strcmp (entity_text (name_entity), "") == 0))
            {
              g_warning ("%s: Wizard PARAM missing NAME",
                         __FUNCTION__);
              free_entity (entity);
              return -1;
            }
          else
            name = entity_text (name_entity);

          regex_entity = entity_child (param_def, "regex");
          if ((regex_entity == NULL)
              || (strcmp (entity_text (regex_entity), "") == 0))
            {
              g_warning ("%s: Wizard PARAM missing REGEX",
                         __FUNCTION__);
              free_entity (entity);
              return -1;
            }
          else
            regex = entity_text (regex_entity);

          optional_entity = entity_child (param_def, "optional");
          optional = (optional_entity
                      && strcmp (entity_text (optional_entity), "")
                      && strcmp (entity_text (optional_entity), "0"));

          if (params)
            {
              guint index = params->len;
              while (index--)
                {
                  name_value_t *pair;

                  pair = (name_value_t*) g_ptr_array_index (params, index);

                  if (pair == NULL)
                    continue;

                  if ((pair->name)
                      && (pair->value)
                      && (strcmp (pair->name, name) == 0))
                    {
                      index = 0; // end loop;
                      param_found = 1;

                      if (g_regex_match_simple (regex, pair->value, 0, 0) == 0)
                        {
                          *command_error
                            = g_strdup_printf ("Value '%s' is not valid for"
                                              " parameter '%s'.",
                                              pair->value, name);
                          free_entity (entity);
                          g_string_free (params_xml, TRUE);
                          return 6;
                        }
                    }
                }
            }

          if (optional == 0 && param_found == 0)
            {
              *command_error = g_strdup_printf ("Mandatory wizard param '%s'"
                                                " missing",
                                                name);
              free_entity (entity);
              return 6;
            }


        }
      param_defs = next_entities (param_defs);
    }

  /* Buffer params */
  if (params)
    {
      guint index = params->len;
      while (index--)
        {
          name_value_t *pair;

          pair = (name_value_t*) g_ptr_array_index (params, index);
          xml_string_append (params_xml,
                             "<param>"
                             "<name>%s</name>"
                             "<value>%s</value>"
                             "</param>",
                             pair->name ? pair->name : "",
                             pair->value ? pair->value : "");
        }
    }

  /* Run each step of the wizard. */

  response = NULL;
  extra = NULL;
  steps = mode_entity->entities;
  while ((step = first_entity (steps)))
    {
      if (strcasecmp (entity_name (step), "step") == 0)
        {
          entity_t command, extra_xsl;
          gchar *gmp;
          int xsl_fd, xml_fd;
          char xsl_file_name[] = "/tmp/gvmd-xsl-XXXXXX";
          FILE *xsl_file, *xml_file;
          char xml_file_name[] = "/tmp/gvmd-xml-XXXXXX";
          char extra_xsl_file_name[] = "/tmp/gvmd-extra-xsl-XXXXXX";
          char extra_xml_file_name[] = "/tmp/gvmd-extra-xml-XXXXXX";

          /* Get the command element. */

          command = entity_child (step, "command");
          if (command == NULL)
            {
              g_warning ("%s: Wizard STEP missing COMMAND",
                         __FUNCTION__);
              free_entity (entity);
              g_free (response);
              g_free (extra);
              g_string_free (params_xml, TRUE);
              return -1;
            }

          /* Save the command XSL from the element to a file. */

          xsl_fd = mkstemp (xsl_file_name);
          if (xsl_fd == -1)
            {
              g_warning ("%s: Wizard XSL file create failed",
                         __FUNCTION__);
              free_entity (entity);
              g_free (response);
              g_free (extra);
              g_string_free (params_xml, TRUE);
              return -1;
            }

          xsl_file = fdopen (xsl_fd, "w");
          if (xsl_file == NULL)
            {
              g_warning ("%s: Wizard XSL file open failed",
                         __FUNCTION__);
              close (xsl_fd);
              free_entity (entity);
              g_free (response);
              g_free (extra);
              g_string_free (params_xml, TRUE);
              return -1;
            }

          if (first_entity (command->entities))
            print_entity (xsl_file, first_entity (command->entities));

          /* Write the params as XML to a file. */

          xml_fd = mkstemp (xml_file_name);
          if (xml_fd == -1)
            {
              g_warning ("%s: Wizard XML file create failed",
                         __FUNCTION__);
              fclose (xsl_file);
              unlink (xsl_file_name);
              free_entity (entity);
              g_free (response);
              g_free (extra);
              g_string_free (params_xml, TRUE);
              return -1;
            }

          xml_file = fdopen (xml_fd, "w");
          if (xml_file == NULL)
            {
              g_warning ("%s: Wizard XML file open failed",
                         __FUNCTION__);
              fclose (xsl_file);
              unlink (xsl_file_name);
              close (xml_fd);
              free_entity (entity);
              g_free (response);
              g_free (extra);
              g_string_free (params_xml, TRUE);
              return -1;
            }

          if (fprintf (xml_file,
                       "<wizard>"
                       "<params>%s</params>"
                       "<previous>"
                       "<response>%s</response>"
                       "<extra_data>%s</extra_data>"
                       "</previous>"
                       "</wizard>\n",
                       params_xml->str ? params_xml->str : "",
                       response ? response : "",
                       extra ? extra : "")
              < 0)
            {
              fclose (xsl_file);
              unlink (xsl_file_name);
              fclose (xml_file);
              unlink (xml_file_name);
              free_entity (entity);
              g_warning ("%s: Wizard failed to write XML",
                         __FUNCTION__);
              g_free (response);
              g_free (extra);
              g_string_free (params_xml, TRUE);
              return -1;
            }

          fflush (xml_file);

          /* Combine XSL and XML to get the GMP command. */

          gmp = xsl_transform (xsl_file_name, xml_file_name, NULL,
                               NULL);
          fclose (xsl_file);
          unlink (xsl_file_name);
          fclose (xml_file);
          unlink (xml_file_name);
          if (gmp == NULL)
            {
              g_warning ("%s: Wizard XSL transform failed",
                         __FUNCTION__);
              free_entity (entity);
              g_free (response);
              g_free (extra);
              g_string_free (params_xml, TRUE);
              return -1;
            }

          /* Run the GMP command. */

          g_free (response);
          response = NULL;
          ret = run_command (run_command_data, gmp, &response);
          if (ret == 0)
            {
              /* Command succeeded. */
            }
          else
            {
              free_entity (entity);
              g_free (response);
              g_free (extra);
              g_string_free (params_xml, TRUE);
              return -1;
            }

          /* Exit if the command failed. */

          if (response)
            {
              const char *status;
              entity_t response_entity;

              response_entity = NULL;
              if (parse_entity (response, &response_entity))
                {
                  g_warning ("%s: Wizard failed to parse response",
                             __FUNCTION__);
                  free_entity (entity);
                  g_free (response);
                  g_free (extra);
                  g_string_free (params_xml, TRUE);
                  return -1;
                }

              status = entity_attribute (response_entity, "status");
              if ((status == NULL)
                  || (strlen (status) == 0)
                  || (status[0] != '2'))
                {
                  g_debug ("response was %s", response);
                  if (command_error)
                    {
                      const char *text;
                      text = entity_attribute (response_entity, "status_text");
                      if (text)
                        *command_error = g_strdup (text);
                    }
                  if (command_error_code)
                    {
                      *command_error_code = g_strdup (status);
                    }
                  free_entity (response_entity);
                  free_entity (entity);
                  g_free (response);
                  g_free (extra);
                  g_string_free (params_xml, TRUE);
                  return 4;
                }

              free_entity (response_entity);
            }

          /* Get the extra_data element. */

          extra_xsl = entity_child (step, "extra_data");
          if (extra_xsl)
            {
              /* Save the extra_data XSL from the element to a file. */

              xsl_fd = mkstemp (extra_xsl_file_name);
              if (xsl_fd == -1)
                {
                  g_warning ("%s: Wizard extra_data XSL file create failed",
                            __FUNCTION__);
                  free_entity (entity);
                  g_free (response);
                  g_free (extra);
                  g_string_free (params_xml, TRUE);
                  return -1;
                }

              xsl_file = fdopen (xsl_fd, "w");
              if (xsl_file == NULL)
                {
                  g_warning ("%s: Wizard extra_data XSL file open failed",
                            __FUNCTION__);
                  close (xsl_fd);
                  free_entity (entity);
                  g_free (response);
                  g_free (extra);
                  g_string_free (params_xml, TRUE);
                  return -1;
                }

              if (first_entity (extra_xsl->entities))
                print_entity (xsl_file, first_entity (extra_xsl->entities));

              /* Write the params as XML to a file. */

              xml_fd = mkstemp (extra_xml_file_name);
              if (xml_fd == -1)
                {
                  g_warning ("%s: Wizard XML file create failed",
                            __FUNCTION__);
                  fclose (xsl_file);
                  unlink (xsl_file_name);
                  free_entity (entity);
                  g_free (response);
                  g_free (extra);
                  g_string_free (params_xml, TRUE);
                  return -1;
                }

              xml_file = fdopen (xml_fd, "w");
              if (xml_file == NULL)
                {
                  g_warning ("%s: Wizard XML file open failed",
                            __FUNCTION__);
                  fclose (xsl_file);
                  unlink (xsl_file_name);
                  close (xml_fd);
                  free_entity (entity);
                  g_free (response);
                  g_free (extra);
                  g_string_free (params_xml, TRUE);
                  return -1;
                }

              if (fprintf (xml_file,
                           "<wizard>"
                           "<params>%s</params>"
                           "<current>"
                           "<response>%s</response>"
                           "</current>"
                           "<previous>"
                           "<extra_data>%s</extra_data>"
                           "</previous>"
                           "</wizard>\n",
                           params_xml->str ? params_xml->str : "",
                           response ? response : "",
                           extra ? extra : "")
                  < 0)
                {
                  fclose (xsl_file);
                  unlink (extra_xsl_file_name);
                  fclose (xml_file);
                  unlink (extra_xml_file_name);
                  free_entity (entity);
                  g_warning ("%s: Wizard failed to write XML",
                            __FUNCTION__);
                  g_free (response);
                  g_free (extra);
                  g_string_free (params_xml, TRUE);
                  return -1;
                }

              fflush (xml_file);

              g_free (extra);
              extra = xsl_transform (extra_xsl_file_name, extra_xml_file_name,
                                     NULL, NULL);
              fclose (xsl_file);
              unlink (extra_xsl_file_name);
              fclose (xml_file);
              unlink (extra_xml_file_name);
            }
        }
      steps = next_entities (steps);
    }

  if (extra)
    extra_wrapped = g_strdup_printf ("<extra_data>%s</extra_data>",
                                     extra);
  else
    extra_wrapped = NULL;
  g_free (extra);

  if (ret_response)
    *ret_response = response;

  if (extra_wrapped)
    {
      entity_t extra_entity, status_entity, status_text_entity;
      ret = parse_entity (extra_wrapped, &extra_entity);
      if (ret == 0)
        {
          status_entity = entity_child (extra_entity, "status");
          status_text_entity = entity_child (extra_entity, "status_text");

          if (status_text_entity && command_error)
            {
              *command_error = g_strdup (entity_text (status_text_entity));
            }

          if (status_entity && command_error_code)
            {
              *command_error_code = g_strdup (entity_text (status_entity));
            }
          free_entity (extra_entity);
        }
      else
        {
          g_warning ("%s: failed to parse extra data", __FUNCTION__);
          free_entity (entity);
          g_string_free (params_xml, TRUE);
          return -1;
        }
    }

  free_entity (entity);
  g_string_free (params_xml, TRUE);

  /* All the steps succeeded. */

  return 0;
}

/**
 * @brief Gets the last termination signal or 0.
 *
 * @return The last termination signal or 0 if there was none.
 */
int
get_termination_signal ()
{
  return termination_signal;
}


/* Resources. */

/**
 * @brief Delete a resource.
 *
 * @param[in]  type         Type of resource.
 * @param[in]  resource_id  UUID of resource.
 * @param[in]  ultimate     Whether to remove entirely, or to trashcan.
 *
 * @return 0 success, 1 resource in use, 2 failed to find resource,
 *         3 predefined resource, 99 permission denied, -1 error.
 */
int
delete_resource (const char *type, const char *resource_id, int ultimate)
{
  if (strcasecmp (type, "ticket") == 0)
    return delete_ticket (resource_id, ultimate);
  if (strcasecmp (type, "tls_certificate") == 0)
    return delete_tls_certificate (resource_id, ultimate);
  assert (0);
  return -1;
}