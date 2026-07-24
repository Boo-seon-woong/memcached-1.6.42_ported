#ifndef STORAGE_H
#define STORAGE_H

void storage_delete(void *e, item *it);
#ifdef EXTSTORE
#define STORAGE_delete(e, it) \
    do { \
        storage_delete(e, it); \
    } while (0)
#else
#define STORAGE_delete(...)
#endif

// API.
void storage_stats(ADD_STAT add_stats, void *c);
void process_extstore_stats(ADD_STAT add_stats, void *c);
void storage_prof_reset(void);   // D6: clear in-server span histograms
bool storage_validate_item(void *e, item *it);
#ifdef EXTSTORE
int storage_get_item(LIBEVENT_THREAD *t, item *it, mc_resp *resp);
// Commit one value remotely and return an unlinked ITEM_HDR. Caller holds
// item_lock(hv); no local value is published on failure.
int storage_store_item(void *e, item *it, item **hdr_it, uint32_t hv);
#else
#define storage_get_item NULL
#endif

// callback for the IO queue subsystem.
void storage_submit_cb(io_queue_t *q);

// Init functions.
struct extstore_conf_file *storage_conf_parse(char *arg);
void *storage_init_config(struct settings *s);
int storage_read_config(void *conf, char **subopt);
int storage_check_config(void *conf);
void *storage_init(void *conf);

// Ignore pointers and header bits from the CRC
#define STORE_OFFSET offsetof(item, nbytes)

#endif
