#ifndef _PROJ3_H
#define _PROJ3_H

#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>

int setupSocket();
void send_msg(const char * target, const char * port, const char * message);
int setup_udp();
void print_view_status();
int connectTCP(const char * host);
void *heartbeat_send_thread(void *arg);
void *heartbeat_receive_thread(void *arg);
void *track_heartbeat_thread(void *arg);
void add_handler(int join_id);
void delete_handler(int peer_id);
void *crash_thread(void *arg);
void *leader_crash_thread(void *arg);


#endif