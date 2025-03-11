#include "proj3.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>


int delay = 0;
char * hostfile_location;
char *hostNames[100];
int host_num = 0;
int processID;
char hostName[100];
int membership[100];
int membershipCount = 0;
int view_id = 1;
int upd_socket = 0;
int request_id = 1;
time_t lastHeartbeat[100];
int crash_delay = 0;
int crash = 0;
pthread_mutex_t heartbeat_mutex = PTHREAD_MUTEX_INITIALIZER;
int crashed[100];
int listen_sock;
int stored_req_id = -1;
int stored_curr_view = -1;
int stored_peer = -1;
int leader = 1;
int leader_crash_protocol = 0;
int in_list = 1;
int HEARTBEAT_TIME = 2;

typedef enum {
    REQ,
    OK,
    NEWVIEW,
    NEWLEADER,
    JOIN,
    CRASH,
    LEAD,
    RESPONSE,
    LEADER_CRASH,
    KICKED
} MessageType;

typedef enum {
    NONE,
    ADD,
    DEL,
    PENDING
} ActionType;

ActionType stored_action = NONE;
ActionType pending_action = NONE;


typedef struct {
    MessageType msgType;
    int process_id;      
    int req_id;          
    int curr_view_id; 
    int view_id;
    int member_count;  
    char member_list[100];
    ActionType actionType; 
    int peer_id; 
} message_t;

// sets up a socket (TCP)
int setupSocket() {

    int socket_desc = socket(AF_INET, SOCK_STREAM, 0);
    
    if(socket_desc < 0){
        printf("Error while creating socket\n");
        return -1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(2000);
    // needed to set to INADDR_ANY, otherwise program would stall
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY); 

    if(bind(socket_desc, (struct sockaddr*)&server_addr, sizeof(server_addr))<0){
        return -1;
    }

    if (listen(socket_desc, 100) < 0) {
        printf("Error while listening\n");
        return -1;
    }

    return socket_desc;
}

// sends a message to another peer
// sourced from my beloved project1
void send_msg(const char * target, const char * port, const char * message) {
    struct addrinfo hints;
    struct addrinfo *result;

    //from example man pages for getaddrinfo
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;  
    hints.ai_socktype = SOCK_DGRAM; 
    hints.ai_flags = AI_PASSIVE; 
    hints.ai_protocol = 0;
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;
    int s = getaddrinfo(target, port, &hints, &result);

    // return if the address didn't exist
    // this preserves cases where we send heartbeats to crashed messages
    // as we will not get anything in return, and should simply skip them
    if (s != 0) {
        return;
    }

    //prepare message
    char msg[100];
    strncpy(msg, message, sizeof(msg));
    // thanks to the linux man pages again for clearing this part up
    sendto(upd_socket, msg, strlen(msg), 0, result->ai_addr, result->ai_addrlen);
    // just in case of memory issues, free the addrinfo
    // freeaddrinfo(result);
    freeaddrinfo(result);
}

// sets up the socket (udp)
int setup_udp() {
    int socket_desc = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(2000);
    // needed to set to INADDR_ANY, otherwise program would stall
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY); 
    
    // binding the socket
    if(bind(socket_desc, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        return -1;
    }

    return socket_desc;
}

void print_view_status() {
    char member_list[256] = "";
    for(int i = 0; i < membershipCount; i ++) {
        char buff[100];
        snprintf(buff, sizeof(buff), "%d", membership[i]);
        strcat(member_list, buff);
        if(i < membershipCount - 1) {
            strcat(member_list, ",");
        }
    }

    fprintf(stderr, "{peer_id:%d, view_id: %d, leader: %d, memb_list: [%s]}\n", processID, view_id, leader, member_list);
}

// connects to a TCP socket for message sending
// dont this way because we do not have a set number of peers at the start
// therefore, we cannot set up TCP connections ahead of time
// instead, we can connect at the time needed, using the membership list
// to get ids, and match them with our given hostlist, to see what
// what peers are currently active
int connectTCP(const char * host) {

    int socket_desc = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr;
    struct hostent *h;
    
    h = gethostbyname(host);
    if (h == NULL) {
        // fprintf(stderr, "No such host: %s\n", host);
        return -1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(2000);
    memcpy(&server_addr.sin_addr.s_addr, h->h_addr, h->h_length);

    if(connect(socket_desc, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0){
        printf("Unable to connect\n");
        close(socket_desc);
        return -1;
    }
    
    return socket_desc;
}

// thread for sending heartbeats to other peers
void *heartbeat_send_thread(void *arg) {
    while(1) {
        sleep(HEARTBEAT_TIME);
        char msg[100];
        snprintf(msg, sizeof(msg), "HEARTBEAT %d\n", processID);

        for(int i = 0; i < membershipCount; i ++) {

            int process_id = membership[i];
            if(process_id != processID) {
                send_msg(hostNames[process_id - 1], "2000", msg);
            }
        }
    }

}

// thread for receiving and updating heartbeats from other peers
void *heartbeat_receive_thread(void *arg) {

    while(1) {
        struct sockaddr_in client_addr;
        int client_addr_len = sizeof(client_addr);
        char msg[100];

        // receives the next message from the socket
        recvfrom(upd_socket, msg, sizeof(msg) - 1, 0,
        (struct sockaddr*)&client_addr, &client_addr_len);

        int peer_id;
        sscanf(msg, "HEARTBEAT %d\n", &peer_id);
        pthread_mutex_lock(&heartbeat_mutex);
        lastHeartbeat[peer_id] = time(NULL);
        pthread_mutex_unlock(&heartbeat_mutex);
    }
}

// thread logic for checking if a process has crashed, and dealing with it as needed
void *track_heartbeat_thread(void *arg) {

    while(1) {
    
        if(in_list == 1) {
            time_t curr_time = time(NULL);

            pthread_mutex_lock(&heartbeat_mutex);
            for(int i = 0; i < membershipCount; i ++) {
                int peer_id = membership[i];

                // debug check
                if(peer_id == -1) {
                  //  fprintf(stderr, "checking peer 0???\n");
                }

                if(peer_id != processID) {

                    // initialization, for the first this process recognizes a peer in the member list
                    if(lastHeartbeat[peer_id] == -1) {
                        lastHeartbeat[peer_id] = time(NULL);
                    } else if(curr_time - lastHeartbeat[peer_id] > HEARTBEAT_TIME * 2) {

                        if(crashed[peer_id] == 0) {
                        
                            if(processID == leader) {
                                message_t crash_msg;
                                crash_msg.msgType = CRASH;
                                crash_msg.peer_id = peer_id;
                                int self_socket = connectTCP(hostNames[processID - 1]);
                                send(self_socket, &crash_msg, sizeof(crash_msg), 0);
                                close(self_socket);
                            }

                            if(peer_id == leader) {
                                fprintf(stderr, "{peer_id:%d, view_id: %d, leader: %d, message:\"peer %d (leader) unreachable\"}\n", processID, view_id, leader, peer_id);

                                // find the new leader (smallest remaining id in the list)
                                int min = 100;
                                for(int j = 0; j < membershipCount; j ++) {
                                    if(membership[j] < min && membership[j] != leader) {
                                        min = membership[j];
                                    }
                                }

                                if(processID == min) {
                                    leader = processID;
                                    message_t new_leader_msg;
                                    new_leader_msg.msgType = LEAD;
                                    int self_socket = connectTCP(hostNames[processID - 1]);
                                    send(self_socket, &new_leader_msg, sizeof(new_leader_msg), 0);
                                    close(self_socket);
                                }

                            } else {
                                fprintf(stderr, "{peer_id:%d, view_id: %d, leader: %d, message:\"peer %d unreachable\"}\n", processID, view_id, leader, peer_id);
                            }
                        }

                        crashed[peer_id] = 1;

                    }
                }
            }
            pthread_mutex_unlock(&heartbeat_mutex);
        }

    }
}

// handles the addition of a member to the membership list
void add_handler(int join_id) {
    // need to gather accepts. first choose a suitable request_id
    request_id += 1;
    message_t request_msg;
    request_msg.msgType = REQ;
    request_msg.req_id = request_id;
    request_msg.actionType = ADD;
    request_msg.view_id = view_id;
    request_msg.peer_id = join_id;

    for(int i = 0; i < membershipCount; i ++) {
        int member_id = membership[i];
        if(member_id != leader) {
            int req_socket_desc = connectTCP(hostNames[member_id - 1]);
            send(req_socket_desc, &request_msg, sizeof(request_msg), 0);
            close(req_socket_desc);
        }
    }
    
    int curr_count = 0;
    int count_needed = membershipCount - 1;
    
    // wait for an OK from every other process
    while(curr_count < count_needed) {

        // in the loop, accepts incoming messages, looking for an "OK"
        struct sockaddr_in client_addr_ok;
        socklen_t addr_len_ok = sizeof(client_addr_ok);
        int connect_ok = accept(listen_sock, (struct sockaddr *)&client_addr_ok, &addr_len_ok);
        if(connect_ok < 0) {
            continue;
        }

        message_t buff_ok;
        if (recv(connect_ok, &buff_ok, sizeof(buff_ok), 0) < 0){
            continue;
        }

        // detects an OK, checks that it is valid (same request id and view id)
        if(buff_ok.msgType == OK) {
            int req_ok = buff_ok.req_id;
            int view_ok = buff_ok.view_id;
            if(req_ok == request_id && view_ok == view_id) {
                // increments the count
                // no further checks should be needed, as each process can send at max
                // only one "OK" with the specific req id + view id
                // therefore, once the target is reached, that indicates all peers gave the OK
                curr_count += 1;
            }
        }
    }

    // OK round is done, so send out the NEWVIEW to every other peer
    view_id += 1;
    membership[membershipCount] = join_id;
    membershipCount += 1;
    char member_list[256] = "";

    for(int i = 0; i < membershipCount; i ++) {
        char buff[100];
        snprintf(buff, sizeof(buff), "%d", membership[i]);
        strcat(member_list, buff);
        if(i < membershipCount - 1) {
            strcat(member_list, ",");
        }
    }

    message_t new_view_msg;
    new_view_msg.msgType = NEWVIEW;
    new_view_msg.view_id = view_id;
    new_view_msg.member_count = membershipCount;
    strcpy(new_view_msg.member_list, member_list);
    
    for(int i = 0; i < membershipCount; i++) {
        int id = membership[i];
        if(id != leader) {
            int new_socket_desc = connectTCP(hostNames[id - 1]);
            send(new_socket_desc, &new_view_msg, sizeof(new_view_msg), 0);
            close(new_socket_desc);
        }
    }

    // leader does not need to send the NEWVIEW to itself, so it does the status print manually
    print_view_status();
}

// handles the deletion of a peer from the membership list
void delete_handler(int peer_id) {

    // need to gather accepts. first choose a suitable request_id
    request_id += 1;
    message_t request_msg;
    request_msg.msgType = REQ;
    request_msg.req_id = request_id;
    request_msg.view_id = view_id;
    request_msg.peer_id = peer_id;
    request_msg.actionType = DEL;

    for(int i = 0; i < membershipCount; i ++) {
        int member_id = membership[i];
        if(member_id != leader) {
            int req_socket_desc = connectTCP(hostNames[member_id - 1]);
            send(req_socket_desc, &request_msg, sizeof(request_msg), 0);
            close(req_socket_desc);
        }
    }
    
    int curr_count = 0;
    int count_needed = membershipCount - 2;
    
    // wait for an OK from every other process
    while(curr_count < count_needed) {
        // in the loop, accepts incoming messages, looking for an "OK"
        struct sockaddr_in client_addr_ok;
        socklen_t addr_len_ok = sizeof(client_addr_ok);
        int connect_ok = accept(listen_sock, (struct sockaddr *)&client_addr_ok, &addr_len_ok);
        if(connect_ok < 0) {
            continue;
        }

        message_t buff_ok;
        if (recv(connect_ok, &buff_ok, sizeof(buff_ok), 0) < 0){
            continue;
        }

        // detects an OK, checks that it is valid (same request id and view id)
        if(buff_ok.msgType == OK) {
            int req_ok = buff_ok.req_id;
            int view_ok = buff_ok.view_id;
            if(req_ok == request_id && view_ok == view_id) {
                // increments the count
                // no further checks should be needed, as each process can send at max
                // only one "OK" with the specific req id + view id
                // therefore, once the target is reached, that indicates all peers gave the OK
                curr_count += 1;
            }
        }

    }

    // OK round is done, so send out the NEWVIEW to every other peer
    view_id += 1;

    pthread_mutex_lock(&heartbeat_mutex);
    membershipCount -= 1;
    char member_list[256] = "";

    for(int i = 0; i < membershipCount; i ++) {
        if(membership[i] != peer_id) {
            char buff[100];
            snprintf(buff, sizeof(buff), "%d", membership[i]);
            strcat(member_list, buff);
            if(i < membershipCount - 1) {
                strcat(member_list, ",");
            }
        }
    }
    
    char *tokens = strtok(member_list, ",");

    int i = 0;

    // parse the list back into a membership array
    while(tokens && i < membershipCount){
        membership[i] = atoi(tokens);
        i += 1;
        tokens = strtok(NULL, ",");
    }
    pthread_mutex_unlock(&heartbeat_mutex);

    message_t new_view_msg;
    new_view_msg.msgType = NEWVIEW;
    new_view_msg.view_id = view_id;
    new_view_msg.member_count = membershipCount;
    strcpy(new_view_msg.member_list, member_list);

    message_t kicked_msg;
    kicked_msg.msgType = KICKED;
    int new_socket_desc = connectTCP(hostNames[peer_id - 1]);
    send(new_socket_desc, &kicked_msg, sizeof(kicked_msg), 0);
    close(new_socket_desc);
    
    for(int i = 0; i < membershipCount; i++) {
        int id = membership[i];
        if(id != leader) {
            int new_socket_desc = connectTCP(hostNames[id - 1]);
            send(new_socket_desc, &new_view_msg, sizeof(new_view_msg), 0);
            close(new_socket_desc);
        }

    }

    // leader does not need to send the NEWVIEW to itself, so it does the status print manually
    print_view_status();
}

// crashes the program after a specified delay
void *crash_thread(void *arg) {
    sleep(crash_delay);
    fprintf(stderr, "{peer_id:%d, view_id: %d, leader: %d, message:\"crashing\"}\n", processID, view_id, leader);
    exit(0);
}

// crashes the leader after all peers join, and after the leader sends a delete message
void *leader_crash_thread(void *arg) {
    while(1) {

        if(membershipCount == host_num) {
            message_t crash_msg;
            crash_msg.msgType = LEADER_CRASH;
            int self_socket = connectTCP(hostNames[processID - 1]);
            send(self_socket, &crash_msg, sizeof(crash_msg), 0);
            close(self_socket);
        }
       
    }
}

// main logic
int main(int argc, char const *argv[]) {
    memset(membership, -1, sizeof(membership));
    // first, we take a trip down proj1 memory lane   
    // like proj1, but we parse a few more args
    for(int i = 0; i < argc; i++) {
        if(strcmp(argv[i], "-h") == 0) {
            hostfile_location = argv[i + 1];
        } else if(strcmp(argv[i], "-d") == 0) {
            delay = atof(argv[i + 1]);
        } else if(strcmp(argv[i], "-c") == 0) {
            crash_delay = atof(argv[i + 1]);
            crash = 1;
        } else if(strcmp(argv[i], "-t") == 0) {
            leader_crash_protocol = 1;
        } 
        
    }

    // open the hostfile
    FILE * hostfile = fopen(hostfile_location, "r");
    if (hostfile == NULL) {
       perror("Error opening file");
       return -1;
    }

    // store a list of all host names
    char line[100];

    // handy way to get the hostname we will use to reference!
    // this will allow us to send a personalized message to other processes
    gethostname(hostName, sizeof(hostName));

    // limits to 100 hosts, should be fine?
    while (fgets(line, sizeof(line), hostfile) && host_num < 100) {
        // line breaks messing up the names...
        // makes sense since each host is on a new line
        line[strcspn(line, "\n")] = '\0';
        char * lineptr = (char*) malloc(strlen(line) + 1);
        strcpy(lineptr, line);
        hostNames[host_num] = lineptr;
        host_num++;
    }

    // grab our id
    for (int i = 0; i < host_num; i++) {
        if (strcmp(hostName, hostNames[i]) == 0) {
            processID = i + 1;
            break;
        }
    }

    // We've stored all the hostnames, so we're good!
    fclose(hostfile);

    sleep(delay);

    listen_sock = setupSocket();
    upd_socket = setup_udp();

    if(processID != leader) {
        // set up a connection
        int socket_desc = connectTCP(hostNames[leader - 1]);
        
        // attempt to join by letting the host know what's up (JOIN)
        message_t joinMsg;
        joinMsg.msgType = JOIN;
        joinMsg.process_id = processID;
        if (send(socket_desc, &joinMsg, sizeof(joinMsg), 0) < 0){
            printf("Can't send\n");
            return -1;
        }

        close(socket_desc);
        
        if(crash == 1) {
            pthread_t crashThread;
            pthread_create(&crashThread, NULL, crash_thread, NULL);
        }

    } else {
        membership[0] = 1;
        membershipCount = 1;
        view_id = 1;
        print_view_status();

        if(leader_crash_protocol == 1) {
            pthread_t leadercrashThread;
            pthread_create(&leadercrashThread, NULL, leader_crash_thread, NULL);
        }
    }  

    for (int i = 0; i < 100; i++) {
        lastHeartbeat[i] = -1;
    }

    for (int i = 0; i < 100; i++) {
        crashed[i] = 0;
    }

    // setup threads for heartbeat detection
    pthread_t heartbeatSendThread;
    pthread_create(&heartbeatSendThread, NULL, heartbeat_send_thread, NULL);

    pthread_t heartbeatReceiveThread;
    pthread_create(&heartbeatReceiveThread, NULL, heartbeat_receive_thread, NULL);

    pthread_t manageHeartbeatThread;
    pthread_create(&manageHeartbeatThread, NULL, track_heartbeat_thread, NULL);

   
    while(1) {
        
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        // accept the next incoming connection sent to it
        // might be better logic to work with what's currently in the membership list
        // but this works as well
        int connect = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);

        message_t buff;
        // extract the contents of the message that was sent from that connection
        if (recv(connect, &buff, sizeof(buff), 0) < 0){
            // a continue, just in case, dont want to block
            continue;
        }

        // if the leader receives a join request
        if(buff.msgType == JOIN && processID == leader) {
            int join_id = buff.process_id;

            // check whether the leader is the only member currently, if so skip the REQ ADD + OK step
            if(membershipCount == 1) {

                // skips right to incremeneting view + NEWVIEW
                view_id += 1;
                membership[membershipCount] = join_id;
                membershipCount += 1;
                char member_list[256] = "";

                // construct a string representation of the membership list
                for(int i = 0; i < membershipCount; i ++) {
                    char buff[100];
                    snprintf(buff, sizeof(buff), "%d", membership[i]);
                    strcat(member_list, buff);
                    if(i < membershipCount - 1) {
                        strcat(member_list, ",");
                    }
                }

                // send out the NEWVIEW message
                message_t new_view_msg;
                new_view_msg.msgType = NEWVIEW;
                new_view_msg.view_id = view_id;
                new_view_msg.member_count = membershipCount;
                strcpy(new_view_msg.member_list, member_list);
                int id = membership[1];
                int new_socket_desc = connectTCP(hostNames[id - 1]);
                send(new_socket_desc, &new_view_msg, sizeof(new_view_msg), 0);
                close(new_socket_desc);

                // leader doesn't need to send itself NEWVIEW, so it does the status print manually
                print_view_status();

            } else {
                add_handler(join_id);
            }

        } else if(buff.msgType == REQ && processID != leader) {
            int req_id = buff.req_id;
            int curr_view_id = buff.view_id;
            int peer_id = buff.peer_id;
            ActionType operation = buff.actionType;

            stored_action = operation;
            stored_curr_view = curr_view_id;
            stored_peer = peer_id;
            stored_req_id = req_id;

            message_t ok_response;
            ok_response.msgType = OK;
            ok_response.req_id = req_id;
            ok_response.view_id = curr_view_id;

            // connect to the host via tcp and send the message
            int new_socket_desc = connectTCP(hostNames[leader - 1]);
            send(new_socket_desc, &ok_response, sizeof(ok_response), 0);
            close(new_socket_desc);

        } else if(buff.msgType == NEWVIEW && processID != 1) {
            // update the view and membership lists accordingly
            int new_view = buff.view_id;
            int count = buff.member_count;
            char membership_string[100];
            strcpy(membership_string, buff.member_list);

            view_id = new_view;
            // count is here to dictate the size of the membership array
            pthread_mutex_lock(&heartbeat_mutex);
            membershipCount = count;
            char *tokens = strtok(membership_string, ",");
            int i = 0;
                        
            // parse the list back into a membership array
            while(tokens && i < membershipCount){
                membership[i] = atoi(tokens);
                i += 1;
                tokens = strtok(NULL, ",");
            }
            pthread_mutex_unlock(&heartbeat_mutex);
    
            print_view_status();
        } else if(buff.msgType == CRASH && processID == leader) {

            int peer_id = buff.peer_id;
            delete_handler(peer_id);
        } else if(buff.msgType == KICKED) {
            in_list = 0;
        } else if(buff.msgType == LEAD && processID == leader) {
            pending_action = NONE;
            request_id += 1;

            message_t new_leader_msg;
            new_leader_msg.msgType = NEWLEADER;
            new_leader_msg.req_id = request_id;
            new_leader_msg.view_id = view_id;
            new_leader_msg.actionType = PENDING;

            for(int i = 0; i < membershipCount; i++) {
                int id = membership[i];
                // send to all processes we can detect as alive
                if(id != leader && crashed[i] == 0) {
                    int new_socket_desc = connectTCP(hostNames[id - 1]);
                    send(new_socket_desc, &new_leader_msg, sizeof(new_leader_msg), 0);
                    close(new_socket_desc);
                }
            }

            int needed_responses = membershipCount - 2;

            int curr_responses = 0;
            int peer_to_operate_on = 0;

            while (curr_responses < needed_responses) {
                // in the loop, accepts incoming messages, looking for an "OK"
                struct sockaddr_in client_addr_ok;
                socklen_t addr_len_ok = sizeof(client_addr_ok);
                int connect_ok = accept(listen_sock, (struct sockaddr *)&client_addr_ok, &addr_len_ok);
                if(connect_ok < 0) {
                    continue;
                }

                message_t buff_ok;
                if (recv(connect_ok, &buff_ok, sizeof(buff_ok), 0) < 0){
                    continue;
                }

                // detects an OK, checks that it is valid (same request id and view id)
                if(buff_ok.msgType == RESPONSE) {

                    int req_ok = buff_ok.req_id;
                    int view_ok = buff_ok.view_id;
                    int peer_ok = buff_ok.peer_id;
                    ActionType old_action = buff_ok.actionType;

                    if(req_ok == request_id && view_ok == view_id) {
                        // increments the count
                        // no further checks should be needed, as each process can send at max
                        // only one "OK" with the specific req id + view id
                        // therefore, once the target is reached, that indicates all peers gave the OK
                        if(old_action != NONE) {
                            pending_action = old_action;
                            peer_to_operate_on = peer_ok;
                        }

                        curr_responses += 1;
                    }
                }
                
            }

            if(pending_action == ADD) {
                add_handler(peer_to_operate_on);
            } else  if(pending_action == DEL) {
                delete_handler(peer_to_operate_on);
            }

        } else if(buff.msgType == NEWLEADER && processID != leader) {

            int min = 100;
            for(int j = 0; j < membershipCount; j ++) {
                if(membership[j] < min && membership[j] != leader) {
                    min = membership[j];
                }
            }

            leader = min;

            int new_leader_req_id = buff.req_id;
            int new_leader_view_id = buff.view_id;
            ActionType new_leader_action = buff.actionType;

            message_t new_leader_response_msg;
            new_leader_response_msg.msgType = RESPONSE;
            new_leader_response_msg.req_id = new_leader_req_id;
            new_leader_response_msg.view_id = stored_curr_view;
            new_leader_response_msg.peer_id = stored_peer;
            new_leader_response_msg.actionType = stored_action;
            
            
            // connect to the host via tcp and send the message
            int new_socket_desc = connectTCP(hostNames[leader - 1]);
            send(new_socket_desc, &new_leader_response_msg, sizeof(new_leader_response_msg), 0);
            close(new_socket_desc);

            stored_action = PENDING;
            stored_curr_view = buff.view_id;
            stored_req_id = buff.req_id;
        } else if(buff.msgType == LEADER_CRASH && processID == leader) {

            request_id += 1;
            
            // decides to delete 5
            message_t final_msg;
            final_msg.msgType = REQ;
            final_msg.req_id = request_id;
            final_msg.actionType = DEL;
            final_msg.view_id = view_id;
            final_msg.peer_id = 5;

            for(int i = 0; i < membershipCount; i++) {
                int id = membership[i];
                // send to all processes we can detect as alive
                int next = leader + 1;
                if(id != leader && id != next && crashed[i] == 0) {
                    int new_socket_desc = connectTCP(hostNames[id - 1]);
                    send(new_socket_desc, &final_msg, sizeof(final_msg), 0);
                    close(new_socket_desc);
                }
            }

            exit(0);
        }

    }
    
}