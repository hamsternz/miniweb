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
static char post_contents[] = "<HTML><BODY><H1>Post contents</H1></BODY></HTML>";

void page_POST_post_html(struct miniweb_session *session) {
    miniweb_response(session, 200);
    printf("User supplied '%s'\n", miniweb_content(session));
    miniweb_write(session, post_contents, sizeof(post_contents)-1);
}

void page_GET_index_html(struct miniweb_session *session) {
    miniweb_response(session, 200);
    miniweb_write(session, contents, sizeof(contents)-1);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    // Change the port number from the default
    miniweb_set_port(8080);

    // Register the web pages
    miniweb_register_page("GET",  "/",            page_GET_index_html);
    miniweb_register_page("GET",  "/index.html",  page_GET_index_html);
    miniweb_register_page("POST", "/post.html",   page_POST_post_html);

    // Start the web server
    while(1) {
        miniweb_run(1000);
    }
    // Never gets here
    miniweb_tidyup();
    return 0;
}
