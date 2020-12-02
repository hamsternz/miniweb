# miniweb
A small, lightweight web server, more aimed for embedded, low memory applications.

## What is it?
It is a single C source and a header file will allow you to quicky hack up a web server.

## A minimal example
Here's a minimal example, that serves a single document on port 8080
```
/////////////////////////////////////////////////////////
// minimal.c : A minimal example
//
// A web server to serve single, fixed document
//
// (c) 2020 Mike Field <hamster@snap.net.nz>
/////////////////////////////////////////////////////////
#include <stdio.h>
#include "miniweb.h"

static char contents[] = "<HTML><BODY><H1>Welcome to Miniweb</H1></BODY></HTML>";

void page_GET_index_html(struct miniweb_session *session) {
    miniweb_response(session, 200);
    miniweb_write(session, contents, sizeof(contents)-1);
}

int main(int argc, char *argv[]) {
    // Change the port number from the default
    miniweb_set_port(8080);

    // Register the web pages
    miniweb_register_page("GET", "/",             page_GET_index_html);
    miniweb_register_page("GET", "/index.html",   page_GET_index_html);

    // Start the web server
    while(1) {
        miniweb_run(1000);
    }
    // Never gets here
    miniweb_tidyup();
    return 0;
}
```

# Performance
Running on a Raspberry Pi 400, it gets around 1000 HTTP/1.0 requests per second.

Running on my low-end i3-3110m laptop, it can perform over 6,000 requests per second from a single core:
```Server Software:        Miniweb/0.0.1
Server Hostname:        localhost
Server Port:            8080

Document Path:          /index.html
Document Length:        585 bytes

Concurrency Level:      50
Time taken for tests:   16.091 seconds
Complete requests:      100000
Failed requests:        0
Total transferred:      68100000 bytes
HTML transferred:       58500000 bytes
Requests per second:    6214.58 [#/sec] (mean)
Time per request:       8.046 [ms] (mean)
Time per request:       0.161 [ms] (mean, across all concurrent requests)
Transfer rate:          4132.94 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    0   0.1      0       3
Processing:     1    8   1.6      7      17
Waiting:        1    8   1.6      7      17
Total:          3    8   1.6      7      17

Percentage of the requests served within a certain time (ms)
  50%      7
  66%      8
  75%      8
  80%      8
  90%     11
  95%     12
  98%     12
  99%     12
 100%     17 (longest request)
```

# Licensing
See include LICENSE file.

# Want to say thanks
If you use this in a project, consider dropping me a line to say thanks, or send my a coffee via PayPal (see email address in source)

# Reference for functions and structures in miniweb.h

    struct miniweb_session;
The opaque datatype for the session status

## Setup and configuration

    int miniweb_set_port(int portno);
Sets the port number that the server will listen on

    int miniweb_register_page(char *method, char *url, void (*callback)(struct miniweb_session *));
Adds a handler for a web page / URL. A '*' in the URL is a wildcard, and can be of of any length. 
Note - currently the wildcard can include slashes, but this is likely to change in future.

    int miniweb_listen_header(char *header);
Informs miniweb of request headers that should be captured.

## Request processing functions

    char *miniweb_get_header(struct miniweb_session *session, char *header);
Retrieves the value of a request header. Note that miniweb must be forst told to listen for a header by 
calling miniweb\_listen\_header().

    int miniweb_add_header(struct miniweb_session *session, char *header, char *value);
Adds a additional header to the reply, or updates any header already present.

    size_t miniweb_write(struct miniweb_session *session, void *data, size_t len);
Adds a block of data to the reply body.

    int miniweb_response(struct miniweb_session *session, int response);
Sets the HTTP response code for this session. Can be called multiple times, with the last call winning.

    char *miniweb_get_wildcard(struct miniweb_session *session);
Returns a pointer to any wildcard that was in the URL.

## Processing / admin functions

    int miniweb_run(int timeout_ms);
Run the web server for at most timout\_ms. Note: It may run longer than timeout\_ms if a page handler blocks.

    void miniweb_stats(void);
Prints out a table of registered URLs, the number of calls, and the total time processing the request.

    void miniweb_tidyup(void);
Releases all the resources in use by miniweb. Closes all sessions in progress.

## Error and status functions

    int miniweb_set_debug_level(int level);
Set the level of debugging. Options are MINIWEB\_DEBUG\_NONE, MINIWEB\_DEBUG\_ERRORS, MINIWEB\_DEBUG\_DATA or MINIWEB\_DEBUG\_ALL 

    int miniweb_log_callback(void (*callback)(char *url, int response_code, unsigned us_taken));
Set a callback that can be used to log requests.

    int miniweb_error_callback(void (*callback)(int error, char *text));
Set a callback that can be used to display internal errors. The error code 'error' can be converted to text with miniweb\_error\_text().
The second parameter, text, can be NULL.

    char *miniweb_error_text(int error);
Convert an internal error number into a text description.

# TODO list
* Add client IP address to the logging callback.
* Add support to query GET variables.
* Add support to POST data .
* Add support for basic authentication.
* Add TLS support for https.
