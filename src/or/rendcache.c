/* Copyright (c) 2015, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file rendcache.c
 * \brief Hidden service desriptor cache.
 **/

#include "rendcache.h"

#include "config.h"
#include "rendcommon.h"
#include "rephist.h"
#include "routerlist.h"
#include "routerparse.h"

/** Map from service id (as generated by rend_get_service_id) to
 * rend_cache_entry_t. */
static strmap_t *rend_cache = NULL;

/** Map from descriptor id to rend_cache_entry_t; only for hidden service
 * directories. */
static digestmap_t *rend_cache_v2_dir = NULL;

/** DOCDOC */
static size_t rend_cache_total_allocation = 0;

/** Initializes the service descriptor cache.
*/
void
rend_cache_init(void)
{
  rend_cache = strmap_new();
  rend_cache_v2_dir = digestmap_new();
}

/** Return the approximate number of bytes needed to hold <b>e</b>. */
static size_t
rend_cache_entry_allocation(const rend_cache_entry_t *e)
{
  if (!e)
    return 0;

  /* This doesn't count intro_nodes or key size */
  return sizeof(*e) + e->len + sizeof(*e->parsed);
}

/** DOCDOC */
size_t
rend_cache_get_total_allocation(void)
{
  return rend_cache_total_allocation;
}

/** Decrement the total bytes attributed to the rendezvous cache by n. */
static void
rend_cache_decrement_allocation(size_t n)
{
  static int have_underflowed = 0;

  if (rend_cache_total_allocation >= n) {
    rend_cache_total_allocation -= n;
  } else {
    rend_cache_total_allocation = 0;
    if (! have_underflowed) {
      have_underflowed = 1;
      log_warn(LD_BUG, "Underflow in rend_cache_decrement_allocation");
    }
  }
}

/** Increase the total bytes attributed to the rendezvous cache by n. */
static void
rend_cache_increment_allocation(size_t n)
{
  static int have_overflowed = 0;
  if (rend_cache_total_allocation <= SIZE_MAX - n) {
    rend_cache_total_allocation += n;
  } else {
    rend_cache_total_allocation = SIZE_MAX;
    if (! have_overflowed) {
      have_overflowed = 1;
      log_warn(LD_BUG, "Overflow in rend_cache_increment_allocation");
    }
  }
}

/** Helper: free storage held by a single service descriptor cache entry. */
static void
rend_cache_entry_free(rend_cache_entry_t *e)
{
  if (!e)
    return;
  rend_cache_decrement_allocation(rend_cache_entry_allocation(e));
  rend_service_descriptor_free(e->parsed);
  tor_free(e->desc);
  tor_free(e);
}

/** Helper: deallocate a rend_cache_entry_t.  (Used with strmap_free(), which
 * requires a function pointer whose argument is void*). */
static void
rend_cache_entry_free_(void *p)
{
  rend_cache_entry_free(p);
}

/** Free all storage held by the service descriptor cache. */
void
rend_cache_free_all(void)
{
  strmap_free(rend_cache, rend_cache_entry_free_);
  digestmap_free(rend_cache_v2_dir, rend_cache_entry_free_);
  rend_cache = NULL;
  rend_cache_v2_dir = NULL;
  rend_cache_total_allocation = 0;
}

/** Removes all old entries from the service descriptor cache.
*/
void
rend_cache_clean(time_t now)
{
  strmap_iter_t *iter;
  const char *key;
  void *val;
  rend_cache_entry_t *ent;
  time_t cutoff = now - REND_CACHE_MAX_AGE - REND_CACHE_MAX_SKEW;
  for (iter = strmap_iter_init(rend_cache); !strmap_iter_done(iter); ) {
    strmap_iter_get(iter, &key, &val);
    ent = (rend_cache_entry_t*)val;
    if (ent->parsed->timestamp < cutoff) {
      iter = strmap_iter_next_rmv(rend_cache, iter);
      rend_cache_entry_free(ent);
    } else {
      iter = strmap_iter_next(rend_cache, iter);
    }
  }
}

/** Remove ALL entries from the rendezvous service descriptor cache.
*/
void
rend_cache_purge(void)
{
  if (rend_cache) {
    log_info(LD_REND, "Purging HS descriptor cache");
    strmap_free(rend_cache, rend_cache_entry_free_);
  }
  rend_cache = strmap_new();
}

/** Remove all old v2 descriptors and those for which this hidden service
 * directory is not responsible for any more.
 *
 * If at all possible, remove at least <b>force_remove</b> bytes of data.
 */
void
rend_cache_clean_v2_descs_as_dir(time_t now, size_t force_remove)
{
  digestmap_iter_t *iter;
  time_t cutoff = now - REND_CACHE_MAX_AGE - REND_CACHE_MAX_SKEW;
  const int LAST_SERVED_CUTOFF_STEP = 1800;
  time_t last_served_cutoff = cutoff;
  size_t bytes_removed = 0;
  do {
    for (iter = digestmap_iter_init(rend_cache_v2_dir);
         !digestmap_iter_done(iter); ) {
      const char *key;
      void *val;
      rend_cache_entry_t *ent;
      digestmap_iter_get(iter, &key, &val);
      ent = val;
      if (ent->parsed->timestamp < cutoff ||
          ent->last_served < last_served_cutoff ||
          !hid_serv_responsible_for_desc_id(key)) {
        char key_base32[REND_DESC_ID_V2_LEN_BASE32 + 1];
        base32_encode(key_base32, sizeof(key_base32), key, DIGEST_LEN);
        log_info(LD_REND, "Removing descriptor with ID '%s' from cache",
                 safe_str_client(key_base32));
        bytes_removed += rend_cache_entry_allocation(ent);
        iter = digestmap_iter_next_rmv(rend_cache_v2_dir, iter);
        rend_cache_entry_free(ent);
      } else {
        iter = digestmap_iter_next(rend_cache_v2_dir, iter);
      }
    }

    /* In case we didn't remove enough bytes, advance the cutoff a little. */
    last_served_cutoff += LAST_SERVED_CUTOFF_STEP;
    if (last_served_cutoff > now)
      break;
  } while (bytes_removed < force_remove);
}

/** Lookup in the client cache the given service ID <b>query</b> for
 * <b>version</b>.
 *
 * Return 0 if found and if <b>e</b> is non NULL, set it with the entry
 * found. Else, a negative value is returned and <b>e</b> is untouched.
 * -EINVAL means that <b>query</b> is not a valid service id.
 * -ENOENT means that no entry in the cache was found. */
int
rend_cache_lookup_entry(const char *query, int version, rend_cache_entry_t **e)
{
  int ret = 0;
  char key[REND_SERVICE_ID_LEN_BASE32 + 2]; /* <version><query>\0 */
  rend_cache_entry_t *entry = NULL;
  static const int default_version = 2;

  tor_assert(rend_cache);
  tor_assert(query);

  if (!rend_valid_service_id(query)) {
    ret = -EINVAL;
    goto end;
  }

  switch (version) {
    case 0:
      log_warn(LD_REND, "Cache lookup of a v0 renddesc is deprecated.");
      break;
    case 2:
      /* Default is version 2. */
    default:
      tor_snprintf(key, sizeof(key), "%d%s", default_version, query);
      entry = strmap_get_lc(rend_cache, key);
      break;
  }
  if (!entry) {
    ret = -ENOENT;
    goto end;
  }
  tor_assert(entry->parsed && entry->parsed->intro_nodes);

  if (e) {
    *e = entry;
  }

end:
  return ret;
}

/** Lookup the v2 service descriptor with base32-encoded <b>desc_id</b> and
 * copy the pointer to it to *<b>desc</b>.  Return 1 on success, 0 on
 * well-formed-but-not-found, and -1 on failure.
 */
int
rend_cache_lookup_v2_desc_as_dir(const char *desc_id, const char **desc)
{
  rend_cache_entry_t *e;
  char desc_id_digest[DIGEST_LEN];
  tor_assert(rend_cache_v2_dir);
  if (base32_decode(desc_id_digest, DIGEST_LEN,
                    desc_id, REND_DESC_ID_V2_LEN_BASE32) < 0) {
    log_fn(LOG_PROTOCOL_WARN, LD_REND,
           "Rejecting v2 rendezvous descriptor request -- descriptor ID "
           "contains illegal characters: %s",
           safe_str(desc_id));
    return -1;
  }
  /* Lookup descriptor and return. */
  e = digestmap_get(rend_cache_v2_dir, desc_id_digest);
  if (e) {
    *desc = e->desc;
    e->last_served = approx_time();
    return 1;
  }
  return 0;
}

/** Parse the v2 service descriptor(s) in <b>desc</b> and store it/them to the
 * local rend cache. Don't attempt to decrypt the included list of introduction
 * points (as we don't have a descriptor cookie for it).
 *
 * If we have a newer descriptor with the same ID, ignore this one.
 * If we have an older descriptor with the same ID, replace it.
 *
 * Return an appropriate rend_cache_store_status_t.
 */
rend_cache_store_status_t
rend_cache_store_v2_desc_as_dir(const char *desc)
{
  const or_options_t *options = get_options();
  rend_service_descriptor_t *parsed;
  char desc_id[DIGEST_LEN];
  char *intro_content;
  size_t intro_size;
  size_t encoded_size;
  char desc_id_base32[REND_DESC_ID_V2_LEN_BASE32 + 1];
  int number_parsed = 0, number_stored = 0;
  const char *current_desc = desc;
  const char *next_desc;
  rend_cache_entry_t *e;
  time_t now = time(NULL);
  tor_assert(rend_cache_v2_dir);
  tor_assert(desc);
  if (!hid_serv_acting_as_directory()) {
    /* Cannot store descs, because we are (currently) not acting as
     * hidden service directory. */
    log_info(LD_REND, "Cannot store descs: Not acting as hs dir");
    return RCS_NOTDIR;
  }
  while (rend_parse_v2_service_descriptor(&parsed, desc_id, &intro_content,
                                          &intro_size, &encoded_size,
                                          &next_desc, current_desc, 1) >= 0) {
    number_parsed++;
    /* We don't care about the introduction points. */
    tor_free(intro_content);
    /* For pretty log statements. */
    base32_encode(desc_id_base32, sizeof(desc_id_base32),
                  desc_id, DIGEST_LEN);
    /* Is desc ID in the range that we are (directly or indirectly) responsible
     * for? */
    if (!hid_serv_responsible_for_desc_id(desc_id)) {
      log_info(LD_REND, "Service descriptor with desc ID %s is not in "
               "interval that we are responsible for.",
               safe_str_client(desc_id_base32));
      goto skip;
    }
    /* Is descriptor too old? */
    if (parsed->timestamp < now - REND_CACHE_MAX_AGE-REND_CACHE_MAX_SKEW) {
      log_info(LD_REND, "Service descriptor with desc ID %s is too old.",
               safe_str(desc_id_base32));
      goto skip;
    }
    /* Is descriptor too far in the future? */
    if (parsed->timestamp > now + REND_CACHE_MAX_SKEW) {
      log_info(LD_REND, "Service descriptor with desc ID %s is too far in the "
               "future.",
               safe_str(desc_id_base32));
      goto skip;
    }
    /* Do we already have a newer descriptor? */
    e = digestmap_get(rend_cache_v2_dir, desc_id);
    if (e && e->parsed->timestamp > parsed->timestamp) {
      log_info(LD_REND, "We already have a newer service descriptor with the "
               "same desc ID %s and version.",
               safe_str(desc_id_base32));
      goto skip;
    }
    /* Do we already have this descriptor? */
    if (e && !strcmp(desc, e->desc)) {
      log_info(LD_REND, "We already have this service descriptor with desc "
               "ID %s.", safe_str(desc_id_base32));
      goto skip;
    }
    /* Store received descriptor. */
    if (!e) {
      e = tor_malloc_zero(sizeof(rend_cache_entry_t));
      digestmap_set(rend_cache_v2_dir, desc_id, e);
      /* Treat something just uploaded as having been served a little
       * while ago, so that flooding with new descriptors doesn't help
       * too much.
       */
      e->last_served = approx_time() - 3600;
    } else {
      rend_cache_decrement_allocation(rend_cache_entry_allocation(e));
      rend_service_descriptor_free(e->parsed);
      tor_free(e->desc);
    }
    e->parsed = parsed;
    e->desc = tor_strndup(current_desc, encoded_size);
    e->len = encoded_size;
    rend_cache_increment_allocation(rend_cache_entry_allocation(e));
    log_info(LD_REND, "Successfully stored service descriptor with desc ID "
             "'%s' and len %d.",
             safe_str(desc_id_base32), (int)encoded_size);

    /* Statistics: Note down this potentially new HS. */
    if (options->HiddenServiceStatistics) {
      rep_hist_stored_maybe_new_hs(e->parsed->pk);
    }

    number_stored++;
    goto advance;
skip:
    rend_service_descriptor_free(parsed);
advance:
    /* advance to next descriptor, if available. */
    current_desc = next_desc;
    /* check if there is a next descriptor. */
    if (!current_desc ||
        strcmpstart(current_desc, "rendezvous-service-descriptor "))
      break;
  }
  if (!number_parsed) {
    log_info(LD_REND, "Could not parse any descriptor.");
    return RCS_BADDESC;
  }
  log_info(LD_REND, "Parsed %d and added %d descriptor%s.",
           number_parsed, number_stored, number_stored != 1 ? "s" : "");
  return RCS_OKAY;
}

/** Parse the v2 service descriptor in <b>desc</b>, decrypt the included list
 * of introduction points with <b>descriptor_cookie</b> (which may also be
 * <b>NULL</b> if decryption is not necessary), and store the descriptor to
 * the local cache under its version and service id.
 *
 * If we have a newer v2 descriptor with the same ID, ignore this one.
 * If we have an older descriptor with the same ID, replace it.
 * If the descriptor's service ID does not match
 * <b>rend_query</b>-\>onion_address, reject it.
 *
 * If the descriptor's descriptor ID doesn't match <b>desc_id_base32</b>,
 * reject it.
 *
 * Return an appropriate rend_cache_store_status_t. If entry is not NULL,
 * set it with the cache entry pointer of the descriptor.
 */
rend_cache_store_status_t
rend_cache_store_v2_desc_as_client(const char *desc,
                                   const char *desc_id_base32,
                                   const rend_data_t *rend_query,
                                   rend_cache_entry_t **entry)
{
  /*XXXX this seems to have a bit of duplicate code with
   * rend_cache_store_v2_desc_as_dir().  Fix that. */
  /* Though having similar elements, both functions were separated on
   * purpose:
   * - dirs don't care about encoded/encrypted introduction points, clients
   *   do.
   * - dirs store descriptors in a separate cache by descriptor ID, whereas
   *   clients store them by service ID; both caches are different data
   *   structures and have different access methods.
   * - dirs store a descriptor only if they are responsible for its ID,
   *   clients do so in every way (because they have requested it before).
   * - dirs can process multiple concatenated descriptors which is required
   *   for replication, whereas clients only accept a single descriptor.
   * Thus, combining both methods would result in a lot of if statements
   * which probably would not improve, but worsen code readability. -KL */
  rend_service_descriptor_t *parsed = NULL;
  char desc_id[DIGEST_LEN];
  char *intro_content = NULL;
  size_t intro_size;
  size_t encoded_size;
  const char *next_desc;
  time_t now = time(NULL);
  char key[REND_SERVICE_ID_LEN_BASE32+2];
  char service_id[REND_SERVICE_ID_LEN_BASE32+1];
  char want_desc_id[DIGEST_LEN];
  rend_cache_entry_t *e;
  rend_cache_store_status_t retval = RCS_BADDESC;
  tor_assert(rend_cache);
  tor_assert(desc);
  tor_assert(desc_id_base32);
  memset(want_desc_id, 0, sizeof(want_desc_id));
  if (entry) {
    *entry = NULL;
  }
  if (base32_decode(want_desc_id, sizeof(want_desc_id),
                    desc_id_base32, strlen(desc_id_base32)) != 0) {
    log_warn(LD_BUG, "Couldn't decode base32 %s for descriptor id.",
             escaped_safe_str_client(desc_id_base32));
    goto err;
  }
  /* Parse the descriptor. */
  if (rend_parse_v2_service_descriptor(&parsed, desc_id, &intro_content,
                                       &intro_size, &encoded_size,
                                       &next_desc, desc, 0) < 0) {
    log_warn(LD_REND, "Could not parse descriptor.");
    goto err;
  }
  /* Compute service ID from public key. */
  if (rend_get_service_id(parsed->pk, service_id)<0) {
    log_warn(LD_REND, "Couldn't compute service ID.");
    goto err;
  }
  if (rend_query->onion_address[0] != '\0' &&
      strcmp(rend_query->onion_address, service_id)) {
    log_warn(LD_REND, "Received service descriptor for service ID %s; "
             "expected descriptor for service ID %s.",
             service_id, safe_str(rend_query->onion_address));
    goto err;
  }
  if (tor_memneq(desc_id, want_desc_id, DIGEST_LEN)) {
    log_warn(LD_REND, "Received service descriptor for %s with incorrect "
             "descriptor ID.", service_id);
    goto err;
  }

  /* Decode/decrypt introduction points. */
  if (intro_content && intro_size > 0) {
    int n_intro_points;
    if (rend_query->auth_type != REND_NO_AUTH &&
        !tor_mem_is_zero(rend_query->descriptor_cookie,
                         sizeof(rend_query->descriptor_cookie))) {
      char *ipos_decrypted = NULL;
      size_t ipos_decrypted_size;
      if (rend_decrypt_introduction_points(&ipos_decrypted,
                                           &ipos_decrypted_size,
                                           rend_query->descriptor_cookie,
                                           intro_content,
                                           intro_size) < 0) {
        log_warn(LD_REND, "Failed to decrypt introduction points. We are "
                 "probably unable to parse the encoded introduction points.");
      } else {
        /* Replace encrypted with decrypted introduction points. */
        log_info(LD_REND, "Successfully decrypted introduction points.");
        tor_free(intro_content);
        intro_content = ipos_decrypted;
        intro_size = ipos_decrypted_size;
      }
    }
    n_intro_points = rend_parse_introduction_points(parsed, intro_content,
                                                    intro_size);
    if (n_intro_points <= 0) {
      log_warn(LD_REND, "Failed to parse introduction points. Either the "
               "service has published a corrupt descriptor or you have "
               "provided invalid authorization data.");
      goto err;
    } else if (n_intro_points > MAX_INTRO_POINTS) {
      log_warn(LD_REND, "Found too many introduction points on a hidden "
               "service descriptor for %s. This is probably a (misguided) "
               "attempt to improve reliability, but it could also be an "
               "attempt to do a guard enumeration attack. Rejecting.",
               safe_str_client(service_id));

      goto err;
    }
  } else {
    log_info(LD_REND, "Descriptor does not contain any introduction points.");
    parsed->intro_nodes = smartlist_new();
  }
  /* We don't need the encoded/encrypted introduction points any longer. */
  tor_free(intro_content);
  /* Is descriptor too old? */
  if (parsed->timestamp < now - REND_CACHE_MAX_AGE-REND_CACHE_MAX_SKEW) {
    log_warn(LD_REND, "Service descriptor with service ID %s is too old.",
             safe_str_client(service_id));
    goto err;
  }
  /* Is descriptor too far in the future? */
  if (parsed->timestamp > now + REND_CACHE_MAX_SKEW) {
    log_warn(LD_REND, "Service descriptor with service ID %s is too far in "
             "the future.", safe_str_client(service_id));
    goto err;
  }
  /* Do we already have a newer descriptor? */
  tor_snprintf(key, sizeof(key), "2%s", service_id);
  e = (rend_cache_entry_t*) strmap_get_lc(rend_cache, key);
  if (e && e->parsed->timestamp >= parsed->timestamp) {
    log_info(LD_REND, "We already have a new enough service descriptor for "
             "service ID %s with the same desc ID and version.",
             safe_str_client(service_id));
    goto okay;
  }
  if (!e) {
    e = tor_malloc_zero(sizeof(rend_cache_entry_t));
    strmap_set_lc(rend_cache, key, e);
  } else {
    rend_cache_decrement_allocation(rend_cache_entry_allocation(e));
    rend_service_descriptor_free(e->parsed);
    tor_free(e->desc);
  }
  e->parsed = parsed;
  e->desc = tor_malloc_zero(encoded_size + 1);
  strlcpy(e->desc, desc, encoded_size + 1);
  e->len = encoded_size;
  rend_cache_increment_allocation(rend_cache_entry_allocation(e));
  log_debug(LD_REND,"Successfully stored rend desc '%s', len %d.",
            safe_str_client(service_id), (int)encoded_size);
  if (entry) {
    *entry = e;
  }
  return RCS_OKAY;

okay:
  if (entry) {
    *entry = e;
  }
  retval = RCS_OKAY;

err:
  rend_service_descriptor_free(parsed);
  tor_free(intro_content);
  return retval;
}
