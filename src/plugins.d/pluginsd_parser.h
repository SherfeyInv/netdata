// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_PLUGINSD_PARSER_H
#define NETDATA_PLUGINSD_PARSER_H

#include "daemon/common.h"

#define WORKER_PARSER_FIRST_JOB 34

// this has to be in-sync with the same at stream-thread.c
#define WORKER_RECEIVER_JOB_REPLICATION_COMPLETION (WORKER_PARSER_FIRST_JOB - 9)

// this controls the max response size of a function
#define PLUGINSD_MAX_DEFERRED_SIZE (100 * 1024 * 1024)

#define PLUGINSD_MIN_RRDSET_POINTERS_CACHE 1024

#define HOST_LABEL_IS_EPHEMERAL "_is_ephemeral"
// PARSER return codes
typedef enum __attribute__ ((__packed__)) parser_rc {
    PARSER_RC_OK,       // Callback was successful, go on
    PARSER_RC_STOP,     // Callback says STOP
    PARSER_RC_ERROR     // Callback failed (abort rest of callbacks)
} PARSER_RC;

typedef enum __attribute__ ((__packed__)) parser_input_type {
    PARSER_INPUT_SPLIT          = (1 << 1),
    PARSER_DEFER_UNTIL_KEYWORD  = (1 << 2),
} PARSER_INPUT_TYPE;

typedef enum __attribute__ ((__packed__)) {
    PARSER_INIT_PLUGINSD        = (1 << 1),
    PARSER_INIT_STREAMING       = (1 << 2),
    PARSER_REP_METADATA         = (1 << 3),
} PARSER_REPERTOIRE;

struct parser;
typedef PARSER_RC (*keyword_function)(char **words, size_t num_words, struct parser *parser);

typedef struct parser_keyword {
    char *keyword;
    size_t id;
    PARSER_REPERTOIRE repertoire;
    size_t worker_job_id;
} PARSER_KEYWORD;

typedef struct parser_user_object {
    bool cleanup_slots;
    RRDSET *st;
    RRDHOST *host;
    void    *opaque;
    struct plugind *cd;
    int trust_durations;
    RRDLABELS *new_host_labels;
    RRDLABELS *chart_rrdlabels_linked_temporarily;
    size_t data_collections_count;
    int enabled;

#ifdef NETDATA_LOG_STREAM_RECEIVE
    FILE *stream_log_fp;
    PARSER_REPERTOIRE stream_log_repertoire;
#endif

    STREAM_CAPABILITIES capabilities; // receiver capabilities

    struct {
        bool parsing_host;
        nd_uuid_t machine_guid;
        char machine_guid_str[UUID_STR_LEN];
        STRING *hostname;
        RRDLABELS *rrdlabels;
    } host_define;

    struct parser_user_object_replay {
        time_t start_time;
        time_t end_time;

        usec_t start_time_ut;
        usec_t end_time_ut;

        time_t wall_clock_time;

        bool rset_enabled;
    } replay;

    struct parser_user_object_v2 {
        bool locked_data_collection;
        RRDSET_STREAM_BUFFER stream_buffer; // sender capabilities in this
        time_t update_every;
        time_t end_time;
        time_t wall_clock_time;
        bool ml_locked;
    } v2;
} PARSER_USER_OBJECT;

typedef void (*parser_deferred_action_t)(struct parser *parser, void *action_data);

struct parser {
    uint8_t version;                // Parser version
    PARSER_REPERTOIRE repertoire;
    uint32_t flags;
    int fd_input;
    int fd_output;
    ND_SOCK *sock;

#ifdef ENABLE_H2O
    void *h2o_ctx;                  // if set we use h2o_stream functions to send data
#endif

    PARSER_USER_OBJECT user;        // User defined structure to hold extra state between calls

    struct buffered_reader reader;
    struct line_splitter line;
    const PARSER_KEYWORD *keyword;

    struct {
        const char *end_keyword;
        BUFFER *response;
        parser_deferred_action_t action;
        void *action_data;
    } defer;

    struct {
        DICTIONARY *functions;
        usec_t smaller_monotonic_timeout_ut;
    } inflight;

    struct {
        SPINLOCK spinlock;
    } writer;
};

typedef struct parser PARSER;

PARSER *parser_init(struct parser_user_object *user, int fd_input, int fd_output, PARSER_INPUT_TYPE flags, ND_SOCK *sock);
void parser_init_repertoire(PARSER *parser, PARSER_REPERTOIRE repertoire);
void parser_destroy(PARSER *working_parser);
void pluginsd_cleanup_v2(PARSER *parser);
void pluginsd_keywords_init(PARSER *parser, PARSER_REPERTOIRE repertoire);
PARSER_RC parser_execute(PARSER *parser, const PARSER_KEYWORD *keyword, char **words, size_t num_words);

static inline int find_first_keyword(const char *src, char *dst, int dst_size, bool *isspace_map) {
    const char *s = src, *keyword_start;

    while (unlikely(isspace_map[(uint8_t)*s])) s++;
    keyword_start = s;

    while (likely(*s && !isspace_map[(uint8_t)*s]) && dst_size > 1) {
        *dst++ = *s++;
        dst_size--;
    }
    *dst = '\0';
    return dst_size == 0 ? 0 : (int) (s - keyword_start);
}

const PARSER_KEYWORD *gperf_lookup_keyword(register const char *str, register size_t len);

static inline const PARSER_KEYWORD *parser_find_keyword(PARSER *parser, const char *command) {
    const PARSER_KEYWORD *t = gperf_lookup_keyword(command, strlen(command));
    if(t && (t->repertoire & parser->repertoire))
        return t;

    return NULL;
}

bool parser_reconstruct_node(BUFFER *wb, void *ptr);
bool parser_reconstruct_instance(BUFFER *wb, void *ptr);
bool parser_reconstruct_context(BUFFER *wb, void *ptr);

static inline int parser_action(PARSER *parser, char *input) {
#ifdef NETDATA_LOG_STREAM_RECEIVE
    static __thread char line[PLUGINSD_LINE_MAX + 1];
    strncpyz(line, input, sizeof(line) - 1);
#endif

    parser->line.count++;

    if(unlikely(parser->flags & PARSER_DEFER_UNTIL_KEYWORD)) {
        char command[100 + 1];
        bool has_keyword = find_first_keyword(input, command, 100, isspace_map_pluginsd);

        if(!has_keyword || strcmp(command, parser->defer.end_keyword) != 0) {
            if(parser->defer.response) {
                buffer_strcat(parser->defer.response, input);
                if(buffer_strlen(parser->defer.response) > PLUGINSD_MAX_DEFERRED_SIZE) {
                    // more than PLUGINSD_MAX_DEFERRED_SIZE of data,
                    // or a bad plugin that did not send the end_keyword
                    nd_log(NDLS_DAEMON, NDLP_ERR, "PLUGINSD: deferred response is too big (%zu bytes). Stopping this plugin.", buffer_strlen(parser->defer.response));
                    return 1;
                }
            }
            return 0;
        }
        else {
            // call the action
            parser->defer.action(parser, parser->defer.action_data);

            // empty everything
            parser->defer.action = NULL;
            parser->defer.action_data = NULL;
            parser->defer.end_keyword = NULL;
            parser->defer.response = NULL;
            parser->flags &= ~PARSER_DEFER_UNTIL_KEYWORD;
        }
        return 0;
    }

    parser->line.num_words = quoted_strings_splitter_pluginsd(input, parser->line.words, PLUGINSD_MAX_WORDS);
    const char *command = get_word(parser->line.words, parser->line.num_words, 0);

    if(unlikely(!command)) {
        line_splitter_reset(&parser->line);
        return 0;
    }

    PARSER_RC rc;
    parser->keyword = parser_find_keyword(parser, command);
    if(likely(parser->keyword)) {
        worker_is_busy(parser->keyword->worker_job_id);

#ifdef NETDATA_LOG_STREAM_RECEIVE
        if(parser->user.stream_log_fp && parser->keyword->repertoire & parser->user.stream_log_repertoire)
            fprintf(parser->user.stream_log_fp, "%s", line);
#endif

        rc = parser_execute(parser, parser->keyword, parser->line.words, parser->line.num_words);
        // rc = (*t->func)(words, num_words, parser);
        worker_is_idle();
    }
    else
        rc = PARSER_RC_ERROR;

    if(rc == PARSER_RC_ERROR) {
        CLEAN_BUFFER *wb = buffer_create(1024, NULL);
        line_splitter_reconstruct_line(wb, &parser->line);
        netdata_log_error("PLUGINSD: parser_action('%s') failed on line %zu: { %s } (quotes added to show parsing)",
                command, parser->line.count, buffer_tostring(wb));
    }

    line_splitter_reset(&parser->line);
    return (rc == PARSER_RC_ERROR || rc == PARSER_RC_STOP);
}

#endif //NETDATA_PLUGINSD_PARSER_H
