/////////////////////////////////////////////////////////////
// main.c : Example miniweb web server process
//
// Demonstrations the more advanced features such as logging,
// error handling and printing statistics to the consle.
//
// (c) 2020 Mike Field <hamster@snap.net.nz>
/////////////////////////////////////////////////////////////
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include "miniweb.h"
#include "time.h"

#define ALLOW_EXIT_URL 0

#ifdef ALLOW_EXIT_URL
void page_GET_exit(struct miniweb_session *session) {
    // Allow the user to cause a clean tidyup for valgrind testing.
    miniweb_tidyup();
    exit(1);
}
#endif

static char *index_html_buffer;
static size_t index_html_size;

void page_GET_index_html(struct miniweb_session *session) {
    if(index_html_buffer == NULL) { 
        FILE *f = fopen("index.html","rb");
        if(f == NULL) {
            miniweb_response(session, 404);
            miniweb_write(session, "File not found\n",15);
        } else {
            // Work out how big the file is, and read it into the buffer
            fseek(f, 0,SEEK_END);
            size_t file_size = ftell(f);
            fseek(f, 0,SEEK_SET);
            // Still malloc something for a zero-length file
            if(file_size == 0) 
               file_size++;
printf("Allocate file\n");
            index_html_buffer = malloc(file_size);
            if(index_html_buffer == NULL) {
                miniweb_response(session, 404);
                miniweb_write(session, "Out of memory\n",14);
            } else {
                index_html_size  = fread(index_html_buffer,1,file_size,f);
            }
            fclose(f);
        }
    }
    miniweb_response(session, 200);
    miniweb_shared_data_buffer(session, index_html_buffer, index_html_size);
}

void page_GET_favicon_ico(struct miniweb_session *session) {
     FILE *f = fopen("favicon.ico","rb");
     if(f == NULL) {
         miniweb_response(session, 404);
         miniweb_write(session, "File not found\n",15);
     } else {
         char buffer[1024];
         int n;
         miniweb_response(session, 200);
         miniweb_add_header(session, "Content-Type", "image/x-icon");
         n = fread(buffer,1,1024,f);
         while(n > 0) {
             miniweb_write(session, buffer,n);
             n = fread(buffer,1,1024,f);
         }
         fclose(f);
     }
}

void page_GET_README_md(struct miniweb_session *session) {
     FILE *f = fopen("README.md","rb");
     if(f == NULL) {
         miniweb_response(session, 404);
         miniweb_write(session, "File not found\n",15);
     } else {
         char buffer[1024];
         int n;
         miniweb_response(session, 200);
         n = fread(buffer,1,1024,f);
         while(n > 0) {
             miniweb_write(session, buffer,n);
             n = fread(buffer,1,1024,f);
         }
         fclose(f);
     }
}

void write_log(char *url, int response_code, unsigned us_taken) {
//    printf("Page access: %s %i %i.%06i\n", url, response_code, us_taken/1000000, us_taken%1000000);
}

void show_error(int error, char *message) {
    if(message != NULL) 
        printf("Error %i at %s: %s\n",error,message,miniweb_error_text(error));
    else
        printf("Error %i: %s\n",error,miniweb_error_text(error));
}

int main(int argc, char *argv[]) {
    time_t stats_time = 0; 
    // Change the port number from the default
    miniweb_set_port(8080);

    // Set the debug level
    miniweb_set_debug_level(MINIWEB_DEBUG_NONE);
    
    // Set what will log requests and errors
    miniweb_log_callback(write_log);
    miniweb_error_callback(show_error);

    // Which headers are we interested in?
    miniweb_listen_header("Host");

    // Register the web pages
    miniweb_register_page("GET", "/",             page_GET_index_html);
    miniweb_register_page("GET", "/index.html",   page_GET_index_html);
    miniweb_register_page("GET", "/favicon.ico",  page_GET_favicon_ico);
    miniweb_register_page("GET", "/README.md",    page_GET_README_md);
    miniweb_register_page("GET", "/*/index.html", page_GET_index_html);
#ifdef ALLOW_EXIT_URL
    miniweb_register_page("GET", "/exit",         page_GET_exit);
#endif

    // Start the web server
    while(1) {
        miniweb_run(4000);
        time_t now = time(NULL);
        if(now > stats_time) {
           miniweb_stats();
           stats_time = now + 10;
        }
    }
    miniweb_tidyup();
    if(index_html_buffer) {
        free(index_html_buffer);
    }
}
