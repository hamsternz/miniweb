# miniweb
A small, lightweight web server, more aimed for embedded, low memory applications.

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

#Reference for functions

###struct miniweb\_session;
The opaque datatype for the session status

## Setup and configuration

###int miniweb\_set\_port(int portno);
Sets the port number that the server will listen on

###int miniweb\_register\_page(char \*method, char \*url, void (\*callback)(struct miniweb_session \*));
Adds a handler for a web page / URL. A '*' in the URL is a wildcard, and can be of of any length. 
Note - currently the wildcard can include slashes, but this is likely to change in future.

###int miniweb\_listen\_header(char \*header);
Informs miniweb of request headers that should be captured.

## Request processing functions

###char \*miniweb\_get\_header(struct miniweb\_session \*session, char \*header);
Retrieves the value of a request header. Note that miniweb must be forst told to listen for a header by 
calling miniweb\_listen\_header().

###int miniweb\_add\_header(struct miniweb\_session \*session, char \*header, char \*value);
Adds a additional header to the reply, or updates any header already present.

###size\_t miniweb\_write(struct miniweb\_session \*session, void \*data, size\_t len);
Adds a block of data to the reply body.

###int miniweb\_response(struct miniweb\_session \*session, int response);
Sets the HTTP response code for this session. Can be called multiple times, with the last call winning.

###char \*miniweb\_get\_wildcard(struct miniweb\_session \*session);
Returns a pointer to any wildcard that was in the URL.

## Processing / admin functions

###int miniweb\_run(int timeout\_ms);
Run the web server for at most timout\_ms. Note: It may run longer than timeout\_ms if a page handler blocks.

###void miniweb\_stats(void);
Prints out a table of registered URLs, the number of calls, and the total time processing the request.

###void miniweb\_tidyup(void);
Releases all the resources in use by miniweb. Closes all sessions in progress.

## Error and status functions

###int miniweb\_set\_debug\_level(int level);
Set the level of debugging. Options are MINIWEB\_DEBUG\_NONE, MINIWEB\_DEBUG\_ERRORS, MINIWEB\_DEBUG\_DATA or MINIWEB\_DEBUG\_ALL 

###int miniweb\_log\_callback(void (\*callback)(char \*url, int response\_code, unsigned us\_taken));
Set a callback that can be used to log requests.

###int miniweb\_error\_callback(void (\*callback)(int error, char \*text));
Set a callback that can be used to display internal errors. The error code 'error' can be converted to text with miniweb\_error\_text().
The second parameter, text, can be NULL.

###char \*miniweb\_error\_text(int error);
Convert an internal error number into a text description.

#TODO list
[ ] Add client IP address to the logging callback.
[ ] Make the output of the response non-blocking.
[ ] Add support to query GET variables.
[ ] Add support for basic authentication.
[ ] Add TLS support for https.
