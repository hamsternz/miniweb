//////////////////////////////////////////////////////////////
// miniweb.c - A minimal web server that support multiple 
//             connections and low resource usage.
//
// (c) 2020 Mike Field <hamster@snap.net.nz>
//////////////////////////////////////////////////////////////
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <time.h>
#include <memory.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include "miniweb.h"

#define MAX_HEADER_SIZE 10240
#define DEBUG_FSM 0
static int debug_level = MINIWEB_DEBUG_NONE;
static int port_no = 80;
static int listen_socket = -1;
static int max_sessions = 500;      // Allow upto this many concurrent session (must be < 1000)
static int timeout_secs = 5;        // Close sessions after 5 secs
static int free_timeout_secs = 15;  // Close sessions after 5 secs

// What headers we will take note of
struct listen_header {
   struct listen_header *next;
   size_t len;
   char *header;
};
static struct listen_header *first_listen_header;

// Headers in the request (but only those we are listening for)
struct request_header {
   struct request_header *next;
   char *header;
   char *value;
};

// Headers queued to send in the reply
struct reply_header {
   struct reply_header *next;
   char *header;
   char *value;
};

void (*log_callback)(char *url, int response_code, unsigned ms_taken);
void (*error_callback)(int error, char *message);

enum parser_state_e { p_method, p_url,   p_protocol, p_lf, 
                      p_start_header, p_header, p_header_sp, p_value,
                      p_end_lf,
                      p_content,
                      p_error};
enum io_state_e { io_reading, io_writing_headers, io_writing_data, io_writing_shared_data};


// The https session state
struct miniweb_session {
   struct miniweb_session *next;
   enum parser_state_e parser_state;
   enum io_state_e     io_state;

   int socket;
   int response_code;
   struct url_reg *url;
   struct timespec start_time;
   time_t last_action;
   struct listen_header *current_header;

   // Lists holding the headers
   struct reply_header *first_reply_header;
   struct request_header *first_request_header;

   // For reading the incoming headers
   char *in_buffer;
   int  in_buffer_size;
   int  in_buffer_used;

   // For buffering the data before sending
   char   *header_data;
   size_t header_data_size;
   char   *data;
   size_t data_size;
   size_t data_used;
   char   *shared_data; 
   size_t shared_data_size;
   char   last_line_term;
   size_t write_pointer;

   // Details of the request
   char *method;
   char *full_url;
   char *protocol;
   char *wildcard;

   char *content;
   int  content_length;
   int  content_read;
};
static struct miniweb_session *first_session;
static int session_count;
static int sessions_timed_out;

// URL Registrations
struct url_reg { 
   struct url_reg *next;
   char *method;
   char *pattern_start; 
   char *pattern_end; 
   size_t pattern_start_len; 
   size_t pattern_end_len; 
   unsigned data_sent_metric;
   unsigned request_count_metric;
   unsigned request_count;
   struct timespec request_time;
   void (*callback)(struct miniweb_session *s);
};
static struct url_reg *first_url_reg;

struct resp_code {
   int number;
   char *text;
} resp_codes[] = {
   {200, " 200 OK\r\n"},
   {400, " 400 Bad Request\r\n"},
   {401, " 401 Not Authorized\\rn"},
   {404, " 404 Not Found\r\n"},
   {500, " 500 Server Error\r\n"}
};
 
/****************************************************************************************/
static void debug_fsm(int pos, int c, char *msg) {
   printf("%3i ",pos);
   switch(c) {
       case '\r': printf("CR  %s\n",msg);  break;
       case '\n': printf("LF  %s\n",msg);  break;
       default:
           if(c<32 || c >126) {
              c = '?';
           }
           printf("'%c' %s\n", c, msg);
           break;
   }
}

/****************************************************************************************/
static int isMethodChar(int c) {
   return c > ' ' && c < 128;
}

/****************************************************************************************/
static int isUrlChar(int c) {
   return c > ' ' && c < 128;
}

/****************************************************************************************/
static int isProtocolChar(int c) {
   return c > ' ' && c < 128;
}

/****************************************************************************************/
static int isHeaderChar(int c) {
   return c > ' ' && c < 128;
}

/****************************************************************************************/
static int isValueChar(int c) {
   return c >= ' ' && c < 128;
}
/****************************************************************************************/
static int lock_url(void) {
   // For if I get around to multi-threading 
   return 1;
}

static void unlock_url(void) {
   // For if I get around to multi-threading 
}

/****************************************************************************************/
int miniweb_log_callback(void (*callback)(char *url, int response_code, unsigned ms_taken)) {
   log_callback = callback;
   return 1;
}

/****************************************************************************************/
int miniweb_set_debug_level(int level) {
    int t = debug_level;
    debug_level = level;
    return t;
}

/****************************************************************************************/
int miniweb_error_callback(void (*callback)(int error, char *message)) {
   error_callback = callback;
   return 1;
}

/****************************************************************************************/
char *miniweb_error_text(int error) {
  switch(error) {
    case MINIWEB_ERR_NOMEM:    return "Out of memory";
    case MINIWEB_ERR_ACCEPT:   return "accept() error";
    case MINIWEB_ERR_LISTEN:   return "listen() error";
    case MINIWEB_ERR_SOCKET:   return "socket error";
    case MINIWEB_ERR_BIND:     return "bind() error";
    case MINIWEB_ERR_CLOSE:    return "close() error";
    case MINIWEB_ERR_HDRTOBIG: return "header too big";
    case MINIWEB_ERR_SELECT:   return "select() too big";
    case MINIWEB_ERR_WRITE:    return "write() too big";
    default:                   return "Unknown error";
  }
}

/****************************************************************************************/
static int miniweb_log_error(int error_code) {
    if(error_callback != NULL) 
       error_callback(error_code, NULL);

    if(debug_level >= MINIWEB_DEBUG_ERRORS) {
        fprintf(stderr,"Miniweb error %i\n", error_code);
    }
    return 0;
}

/****************************************************************************************/
static struct listen_header *header_find(char *data, size_t len) {
    struct listen_header *lh = first_listen_header;
    while(lh != NULL) {
       if(lh->len == len) {
           if(memcmp(data,lh->header,len)==0)
             return lh;
       }
       lh = lh->next;
    }
    if(debug_level >= MINIWEB_DEBUG_ALL) {
        printf("Not listening for '");
        for(size_t i = 0; i < len; i++) {
            putchar(data[i]);
        }
        putchar('\'');
        putchar('\n');
    }
    return NULL;
}
/****************************************************************************************/
static void session_update_metrics(struct miniweb_session *session) {
    struct timespec end_time;
    struct timespec duration;
    int time_us;
    if(session->url == NULL)  // This for 404 pages
        return;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    if(!lock_url()) {
        usleep(100);
        if(!lock_url()) {
            return;
        }
    }
    // Update total time spent
    if(end_time.tv_nsec >= session->start_time.tv_nsec) {
       duration.tv_nsec = end_time.tv_nsec - session->start_time.tv_nsec;
       duration.tv_sec  = end_time.tv_sec  - session->start_time.tv_sec;
    } else {
       duration.tv_nsec = end_time.tv_nsec - session->start_time.tv_nsec+1000000000;
       duration.tv_sec  = end_time.tv_sec  - session->start_time.tv_sec-1;
    }

    session->url->request_time.tv_nsec += duration.tv_nsec;
    session->url->request_time.tv_sec  += duration.tv_sec; 
    if(session->url->request_time.tv_nsec > 1000000000) {
        session->url->request_time.tv_nsec -= 1000000000;
        session->url->request_time.tv_sec  += 1; 
    }

    time_us = duration.tv_nsec / 1000 + duration.tv_sec * 1000000;

    if(session->url->request_time.tv_nsec >= 1000000000) {
       session->url->request_time.tv_nsec -= 1000000000;
       session->url->request_time.tv_sec  += 1;
    }  

    session->url->request_count++;
    session->url->request_count_metric++;
    session->url->data_sent_metric += session->data_used;
    if(session->url->request_count_metric > 0x40000000 || session->url->data_sent_metric > 0x40000000) {
        session->url->request_count_metric >>= 1;
        session->url->data_sent_metric     >>= 1;
    }
    unlock_url();
    if(log_callback != NULL) {
       log_callback(session->full_url, session->response_code, time_us);
    }
}

/****************************************************************************************/
static struct miniweb_session *session_new(int socket) {
   struct miniweb_session *session;
   if(socket == -1)
       return NULL; 
  
   // Can we reused an existing session object? 
   session = first_session;
   while(session != NULL && session->socket != -1) {
       session = session->next;
   }

   // Do we need a new session object
   if(session == NULL) {
       session = malloc(sizeof(struct miniweb_session));
       if(session == NULL) {
           miniweb_log_error(MINIWEB_ERR_NOMEM);
           return NULL;
       }
       session->next = first_session;
       first_session = session;
       session_count++;
   }
   session->io_state     = io_reading;
   session->parser_state = p_method;
   session->current_header = NULL;
   session->socket = socket;
   session->response_code = 500;
   session->url = NULL;
   session->first_request_header = NULL;
   session->first_reply_header = NULL;

   session->header_data = NULL;
   session->header_data_size = 0;
   session->data = NULL;
   session->data_size = 0;
   session->data_used = 0;
   session->shared_data = NULL;
   session->shared_data_size = 0;
   session->write_pointer = 0;

   session->in_buffer = NULL;
   session->in_buffer_size = 0;
   session->in_buffer_used = 0;

   session->last_line_term = 0;
   session->method = NULL;
   session->protocol = NULL;
   session->full_url = NULL;
   session->wildcard = NULL;
 
   session->content_length = -1;
   session->content = NULL;

   return session;
}


/****************************************************************************************/
char *miniweb_get_wildcard(struct miniweb_session *session) {
   if(!session)
       return NULL;
   return session->wildcard;
}
/****************************************************************************************/
static int session_request_header_add(struct miniweb_session *session,char *header, char *value) {
    struct request_header *rh;

    if(debug_level >= MINIWEB_DEBUG_DATA) {
       fprintf(stderr, "Adding header %s: %s\n", header, value);
    }
    rh = malloc(sizeof(struct request_header));
    if(rh == NULL) 
        return 0;

    rh->header = malloc(strlen(header)+1);
    if(rh->header == NULL) {
        free(rh);
        return 0;
    }

    rh->value = malloc(strlen(value)+1);
    if(rh->value == NULL) {
        free(rh->header);
        free(rh);
        return 0;
    }

    strcpy(rh->header, header);
    strcpy(rh->value,  value);

    rh->next = session->first_request_header;
    session->first_request_header = rh;

    return 1;
}

/****************************************************************************************/
static int check_url_match(struct miniweb_session *session, struct url_reg *ur) {
    // Find the ? that starts the get vars
    size_t url_end, len = strlen(session->full_url);
    for(url_end = 0; url_end < len; url_end++) {
        if(session->full_url[url_end] == '?')
            break;
    }

    // If there is no wildcard then check the full URL (excluding vars!)
    if(ur->pattern_end == NULL) {
        if(len != ur->pattern_start_len)
            return 0;
        if(memcmp(session->full_url, ur->pattern_start, ur->pattern_start_len) != 0)
            return 0;
        return 1;
    }

    // Not long enough to have the start and end patterns, and some wildcard chars?
    if(url_end < ur->pattern_start_len+1+ur->pattern_end_len)
        return 0;

    if(memcmp(session->full_url, ur->pattern_start,  ur->pattern_start_len) != 0)
        return 0;

    if(memcmp(session->full_url+len-ur->pattern_end_len, ur->pattern_end,  ur->pattern_end_len) != 0)
        return 0;

    int wildcard_len = len - ur->pattern_start_len - ur->pattern_end_len;
    session->wildcard = malloc(wildcard_len+1);
    if(session->wildcard == NULL)
        return miniweb_log_error(MINIWEB_ERR_NOMEM);

    memcpy(session->wildcard, session->full_url+ur->pattern_start_len,  wildcard_len);
    session->wildcard[wildcard_len] = '\0';

    return 1;
}
/****************************************************************************************/
static int session_find_target_url(struct miniweb_session *session) {
    struct url_reg *ur = first_url_reg;
    if(debug_level == MINIWEB_DEBUG_ALL)
        printf("Looking for %s %s %s\n", session->method, session->full_url, session->protocol);
    while(ur) {
        if(strcmp(session->protocol, "HTTP/1.1") == 0 || strcmp(session->protocol, "HTTP/1.0") == 0) {
            if(strcmp(session->method,   ur->method) == 0) {
                if(check_url_match(session,ur))
                    break;
            }
        }
        ur = ur->next;
    }
    if(debug_level == MINIWEB_DEBUG_ALL) {
        if(ur == NULL) {
            printf("Not found\n");
        } else {
            printf("Found\n");
        }
    }
    session->url = ur;
    return ur ? 1 : 0;
}

/****************************************************************************************/
static void session_empty(struct miniweb_session *session) {
    // Clean up any POST content
    session->content_length = -1;
    if(session->content) {
       free(session->content);
       session->content = NULL;
    }

    // Clean up method, full_url and protocol
    session->parser_state = p_method;
    if(session->method) {
       free(session->method);
       session->method = NULL;
    }
    if(session->full_url) {
       free(session->full_url);
       session->full_url = NULL;
    }
    if(session->protocol) {
       free(session->protocol);
       session->protocol = NULL;
    }
    if(session->wildcard) {
       free(session->wildcard);
       session->wildcard = NULL;
    }
   
    if(session->header_data != NULL) {
        free(session->header_data);
        session->header_data = NULL;
    }
    session->header_data_size = 0;
    // Stop using shared data
    session->shared_data = NULL;
    session->shared_data_size = 0;

    // Clean up reply data
    if(session->data) {
       free(session->data);
       session->data = NULL;
    }

    // Clean up header_data
    if(session->in_buffer) {
       free(session->in_buffer);
       session->in_buffer = NULL; 
    }

    // Clean up reply header
    while(session->first_reply_header) {
       struct reply_header *rh = session->first_reply_header;
       session->first_reply_header = rh->next;
       if(rh->header)
         free(rh->header);
       if(rh->value)
         free(rh->value);
       free(rh);  
    }

    // Clean up request header
    while(session->first_request_header) {
       struct request_header *rh = session->first_request_header;
       session->first_request_header = rh->next;
       if(rh->header)
         free(rh->header);
       if(rh->value)
         free(rh->value);
       free(rh);  
    }
}

/****************************************************************************************/
static void session_end(struct miniweb_session *session) {
    if(session->socket != -1) {
        while(close(session->socket) < 0 && errno == EINTR) {
            miniweb_log_error(MINIWEB_ERR_CLOSE);
        }
        if(debug_level >= MINIWEB_DEBUG_ALL) 
            fprintf(stderr,"SOCKET CLOSE\n");
        session->socket = -1;
    }
    session_empty(session);
}

/****************************************************************************************/
static void build_header_data(struct miniweb_session *s) {
    struct reply_header *rh;
    int rc_index;
    char rc_str[100];
    size_t header_len;
    //TODO: This is really bad code - esp the malloc.

    // Send HTTP response code
 
    for(rc_index = sizeof(resp_codes)/sizeof(struct resp_code)-1; rc_index > 0;  rc_index--) {
        if(resp_codes[rc_index].number == s->response_code) {
           break;
        }
    }
    header_len = strlen(s->protocol);
    if(rc_index < 0) {
       sprintf(rc_str, " %i Unknown\r\n", s->response_code); 
       header_len += strlen(rc_str);
    } else {
       header_len += strlen(resp_codes[rc_index].text);
    }

    rh = s->first_reply_header;
    while(rh != NULL) {
        header_len += strlen(rh->header);
        header_len += 2;
        header_len += strlen(rh->value);
        header_len += 2;
        rh = rh->next;
    }
    header_len += 2;
    header_len += 1; // For the termating null

    // Allocate the space
    s->header_data = malloc(header_len);
    if(s->header_data == NULL) {
        miniweb_log_error(MINIWEB_ERR_NOMEM);
        session_end(s);
        return;
    }
    s->header_data[0] = '\0';

    // Now assemble the headers
    strcpy(s->header_data, s->protocol);
    if(rc_index < 0) {
       strcat(s->header_data, rc_str);
    } else {
       strcat(s->header_data, resp_codes[rc_index].text);
    }

    rh = s->first_reply_header;
    while(rh != NULL) {
        strcat(s->header_data, rh->header);
        strcat(s->header_data, ": ");
        strcat(s->header_data, rh->value);
        strcat(s->header_data, "\r\n");
        rh = rh->next;
    }
    strcat(s->header_data, "\r\n");
    s->header_data_size = strlen(s->header_data);
    s->write_pointer = 0;
}

/****************************************************************************************/
static void session_send_reply(struct miniweb_session *session) {
    // Set the default headers (can be overwritten)
    miniweb_add_header(session, "Server","Miniweb/0.0.1 (Linux)");
    miniweb_add_header(session, "Content-Type","text/html");
    if(strcmp(session->protocol,"HTTP/1.1")==0)
       miniweb_add_header(session, "Keep-Alive", "timeout=10, max=1000");

    // Now process the request
    if(session->url) {
        session->response_code = 500;      // Default response code

        // TODO Add date header 
        // Date: Mon, 27 Jul 2009 12:28:53 GMT

        // Do the user portion of the request
        if(session->url->callback) {
            session->url->callback(session);
        }
    } else {
        session->response_code = 404;
        miniweb_write(session,"Page not found\n",14);
    }

    // Add the content length header - overwrite any already queued to send
    char buffer[11];
    sprintf(buffer,"%zi",session->data_used + session->shared_data_size);
    miniweb_add_header(session, "Content-Length",buffer);

    build_header_data(session);
    session->io_state = io_writing_headers;
}

/****************************************************************************************/
int miniweb_register_page(char *method, char *url, void (*callback)(struct miniweb_session *)) {
   struct url_reg *new_url;
   int url_len = strlen(url);

   // We need the Content-Length header to process POST commands
   if(strcmp(method,"POST")==0) {
      if(miniweb_listen_header("Content-Length")) {
         return 0;
      }
   }

   // Allocate new
   new_url = malloc(sizeof(struct url_reg));
   if(new_url == NULL) {
      return miniweb_log_error(MINIWEB_ERR_NOMEM);
   }

   // Populate fields   
   new_url->next = NULL;
   new_url->method = malloc(strlen(method)+1);
   if(new_url->method == NULL) {
      free(new_url);
      return miniweb_log_error(MINIWEB_ERR_NOMEM);
   }
   strcpy(new_url->method, method);

   int start;
   for(start = 0; start < url_len; start++) {
     if(url[start] == '*')
        break;
   }

   new_url->pattern_start = malloc(start+1);
   if(new_url->pattern_start == NULL) {
      free(new_url->method);
      free(new_url);
      return miniweb_log_error(MINIWEB_ERR_NOMEM);
   }
   if(start > 0) 
     memcpy(new_url->pattern_start, url, start);
   new_url->pattern_start[start] = '\0';
   new_url->pattern_start_len = strlen(new_url->pattern_start);

   if(start == url_len) {
      new_url->pattern_end = NULL;
      new_url->pattern_end_len = 0;
   } else {
      start++; // Skip the '*' before having the end pattern
      new_url->pattern_end = malloc(url_len-start+1);
      if(new_url->pattern_end==NULL)
         return miniweb_log_error(MINIWEB_ERR_NOMEM);

      if(url_len-start > 0) 
         memcpy(new_url->pattern_end, url+start, url_len-start);
      new_url->pattern_end[url_len-start] = '\0';
      new_url->pattern_end_len = strlen(new_url->pattern_end);
   }
   
   new_url->data_sent_metric = 0;
   new_url->request_count_metric = 0;
   new_url->request_count = 0;
   new_url->callback = callback;
   new_url->request_time.tv_nsec = 0;
   new_url->request_time.tv_sec = 0;

   // TODO Add in correct order for filtering
   new_url->next = first_url_reg;
   first_url_reg = new_url;
   return 1;
}
/****************************************************************************************/
size_t miniweb_shared_data_buffer(struct miniweb_session *session, void *data, size_t len) {
    // Overwrite any existing shared data with this one
    session->shared_data      = data;
    session->shared_data_size = len;
    return len;
}

/****************************************************************************************/
size_t miniweb_write(struct miniweb_session *session, void *data, size_t len) {
    if(len == 0)
       return 0;

    if(session->data == NULL) {
        // Create new data buffer if one isn't there
        size_t buff_size = 0;
        if(session->url) {
            if(session->url->request_count_metric > 0) {
                buff_size = session->url->data_sent_metric/session->url->request_count_metric+64;
            }
        }
        if(buff_size < 256) buff_size = 256;
        if(buff_size < len) buff_size = len;

        if(debug_level >= MINIWEB_DEBUG_ALL) {
            fprintf(stderr,"Allocating %zi for data\n",buff_size);
        }
        session->data = malloc(buff_size);
        if(session->data == NULL) {
            return miniweb_log_error(MINIWEB_ERR_NOMEM);
        }
        session->data_size = buff_size;
        session->data_used = 0;
    } else {
        if(session->data_used+len > session->data_size) {
            // Resize if needed
            size_t new_size = session->data_used+len+64;
            char *new_data;
            new_data = realloc(session->data, new_size);
            if(new_data == NULL) {
                return miniweb_log_error(MINIWEB_ERR_NOMEM);
            }
            session->data = new_data;
            if(debug_level >= MINIWEB_DEBUG_ALL) {
                fprintf(stderr, "Updating data buffer %zi to %zi\n", session->data_size, new_size);
            }
            session->data_size = new_size;
        }
    }
    memcpy(session->data+session->data_used, data, len);
    session->data_used += len;
    return len;
}

/****************************************************************************************/
int miniweb_add_header(struct miniweb_session *session, char *header, char *value) {
    struct reply_header *rh;
    rh = session->first_reply_header;
    while(rh != NULL) {
        if(strcmp(rh->header, header) == 0) {
            // No change needed?
            if(strcmp(rh->value,value)==0) 
                return 1;
 
            // Replace value
            char *v = malloc(strlen(value)+1);
            if(v == NULL) 
                return miniweb_log_error(MINIWEB_ERR_NOMEM);
            free(rh->value);
            rh->value = v; 
            return 1;
        }
        rh = rh->next;
    }

    // Allocate the structure
    rh = malloc(sizeof(struct reply_header));
    if(rh == NULL) {
        return miniweb_log_error(MINIWEB_ERR_NOMEM);
    }

    // Populate the structure
    rh->next = NULL; 
    rh->header = malloc(strlen(header)+1);
    if(rh->header == NULL) {
        free(rh);
        return miniweb_log_error(MINIWEB_ERR_NOMEM);
    }
    rh->value = malloc(strlen(value)+1);
    if(rh->value == NULL) {
        free(rh->header);
        free(rh);
        return miniweb_log_error(MINIWEB_ERR_NOMEM);
    }
    strcpy(rh->header,header);
    strcpy(rh->value,value);

    // Adding the first header? If so, add at state
    if(session->first_reply_header == NULL) {
        session->first_reply_header = rh;
    } else {
        // Add at the end of the list (so they go out in order)
        struct reply_header *h;
        h = session->first_reply_header;
        while(h->next != NULL) {
            h = h->next;
        }
        h->next = rh;
    }
    return 1;
}

/****************************************************************************************/
char *miniweb_get_header(struct miniweb_session *session, char *header) {
    struct request_header *h = session->first_request_header;
    while(h != NULL) {
      if(strcmp(header,h->header)==0) {
         return h->value;
      }
      h = h->next;
    }
    return NULL;
}

/****************************************************************************************/
int miniweb_content_length(struct miniweb_session *session) {
   if(session->content_length == -1) {
      char *length_string = miniweb_get_header(session, "Content-Length");
      if(length_string == NULL) {
         fprintf(stderr, "No content length header\n");
      } else {
         session->content_length = atoi(length_string);
         if(session->content_length < 0) {
           fprintf(stderr, "Negative content length mapped to zero");
           session->content_length = 0;
         }
      }
   }
   return session->content_length;
}

/****************************************************************************************/
char *miniweb_content(struct miniweb_session *session) {
   return session->content;
}
/****************************************************************************************/
int miniweb_listen_header(char *header) {
   struct listen_header *lh;
   char *h;

   // First check if we already have the header in the list?
   lh = first_listen_header;
   while(lh != NULL) {
      if(strcmp(header,lh->header)==0)
          return 1;
      lh = lh->next;
   }

   // Nope - we need to add it.
   int len = strlen(header);
   h = malloc(len+1);
   if(h == NULL)
     return miniweb_log_error(MINIWEB_ERR_NOMEM);

   lh = malloc(sizeof(struct listen_header));
   if(lh == NULL) {
     free(h);
     return miniweb_log_error(MINIWEB_ERR_NOMEM);
   }
   strcpy(h,header);
   lh->header = h;
   lh->len = len;
   lh->next = first_listen_header;
   first_listen_header = lh; 
   return 1;
}

/****************************************************************************************/
int miniweb_response(struct miniweb_session *session, int response) {
   session->response_code = response;
   return 1;
}

/****************************************************************************************/
void  miniweb_tidyup(void) {
   while(first_session != NULL) {
      struct miniweb_session *next = first_session->next;
      session_end(first_session);
      free(first_session);
      first_session = next;
   }
   while(first_listen_header != NULL) {
      struct listen_header *lh = first_listen_header;
      first_listen_header = lh->next;
      if(lh->header)
        free(lh->header);
      free(lh);
   }

   while(first_url_reg != NULL) {
      struct url_reg *url = first_url_reg;
      first_url_reg = url->next;
      if(url->method)
        free(url->method);
      if(url->pattern_start)
        free(url->pattern_start);
      if(url->pattern_end)
        free(url->pattern_end);
      free(url);
   }

   if(listen_socket != -1) {
     close(listen_socket);
     listen_socket = -1;
   }
}
/****************************************************************************************/
void miniweb_stats(void) {
   struct url_reg *url = first_url_reg;
   printf("%i active session, %i timed out\n", session_count, sessions_timed_out);
   printf("Count   Time    URL\n");
   while(url != NULL) {
      printf("%6i ", url->request_count);
      printf("%6i.%09i ", (int)url->request_time.tv_sec, (int)url->request_time.tv_nsec); 
      if(url->pattern_end == NULL) {
         printf("%s %s\n", url->method, url->pattern_start);
      } else {
         printf("%s %s*%s\n", url->method, url->pattern_start, url->pattern_end);
      }
      url = url->next;
   } 
   putchar('\n');
}

/****************************************************************************************/
static void write_more_headers(struct miniweb_session *s) {
    if(s->header_data) {
        while(s->write_pointer != s->header_data_size) {
            int n = write(s->socket, s->header_data+s->write_pointer, s->header_data_size-s->write_pointer);
            if(n >= 0) {
                s->write_pointer += n;
            } else if(n == -1) {
                switch(errno) {
                    case EINTR:
                       break;
                    case EWOULDBLOCK:
                       return; // Go away and come back later
                    default: 
                       miniweb_log_error(MINIWEB_ERR_WRITE);
                       return; // Go away and come back later
                }
            }
        }
    }
    s->write_pointer = 0;
    if(s->data != NULL) {
        s->io_state = io_writing_data;   
    } else  if(s->shared_data != NULL) {
        s->io_state = io_writing_shared_data;   
    } else {
        // Close older 1.0 (non-persistent) connections
        if(strcmp(s->protocol, "HTTP/1.1") != 0)
            session_end(s);
        else
            s->io_state = io_reading;   
    }
}

/****************************************************************************************/
static void write_more_data(struct miniweb_session *s) {
    if(s->data) {
        while(s->write_pointer != s->data_size) {
            int n = write(s->socket, s->data+s->write_pointer, s->data_size-s->write_pointer);
            if(n >= 0) {
                s->write_pointer += n;
            } else if(n == -1) {
                switch(errno) {
                    case EINTR:
                       break;
                    case EWOULDBLOCK:
                       return; // Go away and come back later
                    default: 
                       miniweb_log_error(MINIWEB_ERR_WRITE);
                       return; // Go away and come back later
                }
            }
        }
    }
    s->write_pointer = 0;
    if(s->shared_data != NULL) {
        s->io_state = io_writing_shared_data;   
    } else {
        // Close older 1.0 (non-persistent) connections
        if(strcmp(s->protocol, "HTTP/1.1") != 0)
            session_end(s);
        else
            s->io_state = io_reading;   
    }
}
/****************************************************************************************/
static void write_more_shared_data(struct miniweb_session *s) {
    if(s->shared_data) {
        while(s->write_pointer != s->shared_data_size) {
            int n = write(s->socket, s->shared_data+s->write_pointer, s->shared_data_size-s->write_pointer);
            if(n >= 0) {
                s->write_pointer += n;
            } else if(n == -1) {
                switch(errno) {
                    case EINTR:
                       break;
                    case EWOULDBLOCK:
                       return; // Go away and come back later
                    default: 
                       miniweb_log_error(MINIWEB_ERR_WRITE);
                       return; // Go away and come back later
                }
            }
        }
    }

    session_update_metrics(s);
    s->write_pointer = 0;
    // Close older 1.0 (non-persistent) connections
    if(strcmp(s->protocol, "HTTP/1.1") != 0)
        session_end(s);
    else
        s->io_state = io_reading;   
}
/****************************************************************************************/
static int session_read(struct miniweb_session *session) {
    int n;
    if(session->socket == -1) 
       return 0;
    /* If connection is established then start communicating */
    if(session->in_buffer == NULL) {
        // Need to allocate the buffer?
        size_t new_size = 128; 
        session->in_buffer = malloc(new_size);
        if(session->in_buffer == NULL) {
            return miniweb_log_error(MINIWEB_ERR_NOMEM);
        }
        session->in_buffer_size = new_size;
        session->in_buffer_used = 0;
    } else if(session->in_buffer_size == session->in_buffer_used) {
        // Need to grow the buffer?
        if(session->in_buffer_size == MAX_HEADER_SIZE) {
            session_end(session);
            return miniweb_log_error(MINIWEB_ERR_HDRTOBIG);
        } else {
            size_t new_size = session->in_buffer_size*3/2+1;
            if(new_size > MAX_HEADER_SIZE)
                new_size = MAX_HEADER_SIZE;
            
            char *buffer = realloc(session->in_buffer, new_size);
            if(buffer == NULL) {
                session_end(session);
                return miniweb_log_error(MINIWEB_ERR_NOMEM);
            } 
            session->in_buffer_size = new_size;
            session->in_buffer      = buffer;
        }
    }

    n = read( session->socket,session->in_buffer+session->in_buffer_used,session->in_buffer_size-session->in_buffer_used);
    if (n < 1) {
        session_end(session);
        return 0;
    }

    int scan_pos = session->in_buffer_used;
    session->in_buffer_used += n;
    int consumed = 0;
    while(scan_pos != session->in_buffer_used) {
        int c = session->in_buffer[scan_pos];
        scan_pos++;
        switch(session->parser_state) {
            case p_method:
                if(DEBUG_FSM) debug_fsm(scan_pos-1,c,"p_method");
                // Start recording transaction time from now
                if(scan_pos == 1)
                    clock_gettime(CLOCK_MONOTONIC, &(session->start_time));
                if(c == ' ') {
                    int len = scan_pos-consumed-1;
                    session->method = malloc(len+1);
                    if(session->method == NULL) {
                        session->parser_state = p_error;
                    } else {
                        memcpy(session->method, session->in_buffer+consumed, len);
                        session->method[len] = '\0';
                        consumed = scan_pos;
                        session->parser_state = p_url;
                    }
                } else if(!isMethodChar(c)) {
                    session->parser_state = p_error;
                }
                break;
            case p_url:
                if(DEBUG_FSM) debug_fsm(scan_pos-1, c,"p_url");
                if(c == ' ') {
                   int len = scan_pos-consumed-1;
                   session->full_url = malloc(len+1);
                   if(session->full_url == NULL) {
                       session->parser_state = p_error;
                   } else {
                       memcpy(session->full_url, session->in_buffer+consumed, len);
                       session->full_url[len] = '\0';
                       consumed = scan_pos;
                       session->parser_state = p_protocol;
                   }
                } else if(!isUrlChar(c)) {
                   session->parser_state = p_error;
                }
                break;
            case p_protocol:
                if(DEBUG_FSM) debug_fsm(scan_pos-1, c,"p_protocol");
                if(c == '\r') {
                   int len = scan_pos-consumed-1;
                   session->protocol = malloc(len+1);
                   if(session->protocol == NULL) {
                       session->parser_state = p_error;
                   } else {
                       memcpy(session->protocol, session->in_buffer+consumed, len);
                       session->protocol[len] = '\0';
                       consumed = scan_pos;
                       session->parser_state = p_lf;
                   }
                } else if(!isProtocolChar(c)) {
                   session->parser_state = p_error;
                }
                break;
            case p_lf:
                if(DEBUG_FSM) debug_fsm(scan_pos-1, c,"p_lf");
                if(c == '\n') {
                   consumed = scan_pos;
                   session->parser_state = p_start_header;
                } else {
                   session->parser_state = p_error;
                }
                break;
            case p_start_header:
                if(DEBUG_FSM) debug_fsm(scan_pos-1, c,"p_start_header");
                if(c == '\r') {
                   session->parser_state = p_end_lf;
                } else if(!isHeaderChar(c)) {
                   session->parser_state = p_error;
                } else {
                   session->parser_state = p_header;
                }
                break;
            case p_header:
                if(DEBUG_FSM) debug_fsm(scan_pos-1, c,"p_header");
                if(c == ':') {
                   // See if the header is one we are listening to
                   session->current_header = header_find(session->in_buffer+consumed, scan_pos-1-consumed);
                   consumed = scan_pos;
                   session->parser_state = p_header_sp;
                } else if(!isHeaderChar(c)) {
                   session->parser_state = p_error;
                }
                break;
            case p_header_sp:
                if(DEBUG_FSM) debug_fsm(scan_pos-1, c,"p_header_sp");
                if(c == ' ') {
                   consumed = scan_pos;
                   session->parser_state = p_value;
                } else {
                   session->parser_state = p_error;
                }
                break;
            case p_value:
                if(DEBUG_FSM) debug_fsm(scan_pos-1, c,"p_value");
                if(c == '\r') {
                   if(session->current_header) {
                       int t = session->in_buffer[scan_pos-1];
                       session->in_buffer[scan_pos-1] = '\0';
                       char *v = session->in_buffer+consumed;
                       if(!session_request_header_add(session, session->current_header->header, v)) {
                          session->parser_state = p_error;
                       }
                       session->in_buffer[scan_pos-1] = t;
                   }
                   consumed = scan_pos;
                   session->parser_state = p_lf;
                   session->current_header = NULL;
                } else if(!isValueChar(c)) {
                   session->parser_state = p_error;
                   session->current_header = NULL;
                }
                break;
            case p_end_lf:
                if(DEBUG_FSM) debug_fsm(scan_pos-1, c,"p_end_lf");
                if(c == '\n') {
                    if(debug_level >= MINIWEB_DEBUG_ALL)
                        printf("Ready to run a query\n"); 

                    consumed = scan_pos;
                    miniweb_content_length(session); // Pull content lenght from headers
                    // TODO - Add limit to content lenght
                    if(strcmp(session->method,"POST")==0 && session->content_length > 0) {
                        session->content = malloc(session->content_length);
                        if(session->content != NULL) {
                           printf("Attempting to read %i of content\n", session->content_length);
                           session->content_read = 0;
                           session->parser_state = p_content;
                        } else {
                           printf("Unable to allocate content_memory\n"); 
                           session->parser_state = p_error;
                        }
                    } else {
                        session->parser_state = p_method;
                        // Exec request
                        session_find_target_url(session);
                        session_send_reply(session); 
                    }     
                } else {
                    session->parser_state = p_error;
                }
                break;
            case p_content:
                if(DEBUG_FSM) debug_fsm(scan_pos-1, c,"p_content");

                consumed = scan_pos;
                if(consumed != session->in_buffer_used) {
                    // Work out how much content we have left to read
                    int data_to_copy = session->in_buffer_used-consumed;
                    if(data_to_copy > session->content_length-session->content_read)
                       data_to_copy = session->content_length-session->content_read;
                    // move over what is in the buffer
                    memcpy(session->content+session->content_read, 
                           session->in_buffer+consumed,
                           data_to_copy);
                    consumed += data_to_copy;
                    session->content_read += data_to_copy;
                    printf("Added %i bytes of content\n", data_to_copy);
                }
                // If we have all the content
                if(session->content_read == session->content_length) {
                        session->parser_state = p_method;
                        // Exec request
                        session_find_target_url(session);
                        session_send_reply(session); 
                }     
                break;

            case p_error:
                if(DEBUG_FSM) debug_fsm(scan_pos-1, c,"p_error");
            default:
                break;
        }
    }
    if(consumed) {
        // Throw away the data 
        if(consumed != session->in_buffer_used) {
            // Move remaining data to the front of buffer
            memmove(session->in_buffer, session->in_buffer+consumed, session->in_buffer_used-consumed); 
        }
        session->in_buffer_used -= consumed;
    }
    return 1;
}

/****************************************************************************************/
int miniweb_set_port(int port) {
   port_no = port;
   return 1;
}

/****************************************************************************************/
int miniweb_run(int timeout_ms) {
     static time_t listen_retry_time = 0;
     static time_t last_now = 0;
     time_t now = time(NULL);

     if(listen_socket < 0 && listen_retry_time <= now) {
         listen_retry_time = now+3;
         struct sockaddr_in serv_addr;

         if(debug_level >= MINIWEB_DEBUG_ALL) {
            fprintf(stderr, "Attempting to set up listening socket\n");
         }

         listen_socket = socket(AF_INET, SOCK_STREAM, 0);
         if(listen_socket < 0) {
             miniweb_log_error(MINIWEB_ERR_SOCKET);
             return 0;
         }
     
         /* Initialize socket structure */
         bzero((char *) &serv_addr, sizeof(serv_addr));
   
         serv_addr.sin_family      = AF_INET;
         serv_addr.sin_addr.s_addr = INADDR_ANY;
         serv_addr.sin_port        = htons(port_no);
   
         /* Now bind the host address using bind() call.*/
         if (bind(listen_socket, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
             close(listen_socket);
             listen_socket = -1;
             miniweb_log_error(MINIWEB_ERR_BIND);
             return 0;
         }
         if(debug_level >= MINIWEB_DEBUG_ALL) {
            fprintf(stderr, "Listening socket opened\n");
         }
         int fileflags;
         if((fileflags = fcntl(listen_socket, F_GETFL, 0)) == -1) {
             perror("fcntl F_GETFL");
         }
         if((fcntl(listen_socket, F_SETFL, fileflags | O_NONBLOCK)) == -1) {
             perror("fcntl F_SETFL, O_NONBLOCK");
         }
         if(listen(listen_socket,100) == -1 ) {
             close(listen_socket);
             listen_socket = -1;
             miniweb_log_error(MINIWEB_ERR_LISTEN);
             return 0;
         }
     }
 
     fd_set rfds, wfds, efds;
     struct timeval tv;
     int retval;
     int max_fd = 0;
     FD_ZERO(&rfds);
     FD_ZERO(&wfds);
     FD_ZERO(&efds);
     if(listen_socket >= 0 && session_count < max_sessions) {
         FD_SET(listen_socket, &rfds);
         FD_SET(listen_socket, &efds);
         max_fd = listen_socket+1;
     }

     // Remove the head of the list, if it is stale
     if(first_session != NULL) {
         if(first_session->last_action + free_timeout_secs < now) {
             struct miniweb_session *next = first_session->next;
             session_empty(first_session);
             free(first_session);
             session_count--;
             first_session = next;
         }
     }

     struct miniweb_session *s = first_session;
     while(s != NULL) {
         if(s->socket >= 0) {
             switch(s->io_state) { 
                 case io_reading:
                    FD_SET(s->socket, &rfds);
                    break;
                 case io_writing_headers:
                 case io_writing_data:
                 case io_writing_shared_data:
                    FD_SET(s->socket, &wfds);
                    break;
             };
             if(max_fd < s->socket+1) 
                max_fd = s->socket+1;
         }
         // Remove and free any stale sessions at the end of the list
         if(s->next != NULL && s->next->next == NULL) {
             struct miniweb_session *tail = s->next;
             if(tail->last_action + free_timeout_secs < now) {
                session_empty(tail);
                free(tail);
                session_count--;
                s->next = NULL;
             }
         }

         s = s->next;
     } 
     tv.tv_sec  = (timeout_ms/1000);
     tv.tv_usec = (timeout_ms%1000)*1000;

     retval = select(max_fd, &rfds, &wfds, &efds, &tv);
     if (retval == -1) {
         if(errno != EINTR)
           miniweb_log_error(MINIWEB_ERR_SELECT);
         return 0;
     }
     else if (!retval) {
         // Nothing to do 
         return 0;
     }

     // Process the session sockets first 
     s = first_session;
     while(s != NULL) {
         if(s->socket >= 0 && FD_ISSET(s->socket, &rfds)) {
            session_read(s);
            s->last_action = now;
         }
         if(s->socket >= 0 && FD_ISSET(s->socket, &wfds)) {
             switch(s->io_state) { 
                 case io_reading:
                    break; 
                 case io_writing_headers:
                    write_more_headers(s);
                    break;
                 case io_writing_data:
                    write_more_data(s);
                    break;
                 case io_writing_shared_data:
                    write_more_shared_data(s);
                    break;
             };
         }
         if(s->socket >= 0 && FD_ISSET(s->socket, &efds)) {
            session_end(s);
         }
         s = s->next;
     } 
    
     if(last_now != now) { 
         s = first_session;
         while(s != NULL) {
             if(s->socket != -1 && s->last_action+timeout_secs < now) {
                 session_end(s);
                 sessions_timed_out++;
             }
             s = s->next;
         }
         last_now = now;
     }

     // Accept any new connections
     if(listen_socket >= 0 && FD_ISSET(listen_socket, &rfds)) {
         int newsockfd; 
         struct sockaddr_in cli_addr;
         socklen_t clilen;

         clilen = sizeof(cli_addr);
     
         /* Accept actual connection from the client */
         newsockfd = accept(listen_socket, (struct sockaddr *)&cli_addr, &clilen);
         if (newsockfd < 0) {
             miniweb_log_error(MINIWEB_ERR_ACCEPT);
             perror("Accept");
             return 0;
         }
         if(debug_level >= MINIWEB_DEBUG_ALL) {
             fprintf(stderr, "SOCKET ACCPTED\n");
         }
         int fileflags;
         if((fileflags = fcntl(newsockfd, F_GETFL, 0)) == -1) {
             perror("fcntl F_GETFL");
         }
         if((fcntl(newsockfd, F_SETFL, fileflags | O_NONBLOCK)) == -1) {
             perror("fcntl F_SETFL, O_NONBLOCK");
         }
         
         struct miniweb_session *session = session_new(newsockfd);
         if(session == NULL) {
            close(newsockfd);
         }
         session->last_action = now;
     }
     return 0;
}
/****************************************************************************************/
/*  End of file                                                                         */
/****************************************************************************************/
