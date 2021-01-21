#ifndef MINIWEB_H
#define MINIWEB_H

/* Error codes */
#define MINIWEB_ERR_NOMEM    (-1)
#define MINIWEB_ERR_ACCEPT   (-2)
#define MINIWEB_ERR_LISTEN   (-3)
#define MINIWEB_ERR_SOCKET   (-4)
#define MINIWEB_ERR_BIND     (-5)
#define MINIWEB_ERR_CLOSE    (-6)
#define MINIWEB_ERR_HDRTOBIG (-7)
#define MINIWEB_ERR_SELECT   (-8)
#define MINIWEB_ERR_WRITE    (-9)

/* Debug level settings */
#define MINIWEB_DEBUG_NONE   (0)
#define MINIWEB_DEBUG_ERRORS (1)
#define MINIWEB_DEBUG_DATA   (2)
#define MINIWEB_DEBUG_ALL    (3)

/* Opaque data type */
struct miniweb_session;

/* Setup functions */
int    miniweb_set_port(int portno);
int    miniweb_register_page(char *method, char *url, void (*callback)(struct miniweb_session *));
int    miniweb_listen_header(char *header);

/* Request processing functions */
char  *miniweb_get_header(struct miniweb_session *session, char *header);
int    miniweb_add_header(struct miniweb_session *session, char *header, char *value);
size_t miniweb_write(struct miniweb_session *session, void *data, size_t len);
size_t miniweb_shared_data_buffer(struct miniweb_session *session, void *data, size_t len);
int    miniweb_response(struct miniweb_session *session, int response);
char  *miniweb_get_wildcard(struct miniweb_session *session);
int    miniweb_content_length(struct miniweb_session *session);
int    miniweb_content_read(struct miniweb_session *session, void *data, int len);

/* Process / admin */
int   miniweb_run(int timeout_ms);
void  miniweb_stats(void);
void  miniweb_tidyup(void);

/* Error and status functions */
int   miniweb_set_debug_level(int level);
int   miniweb_log_callback(void (*callback)(char *url, int response_code, unsigned us_taken));
int   miniweb_error_callback(void (*callback)(int error, char *text));
char *miniweb_error_text(int error);
#endif
