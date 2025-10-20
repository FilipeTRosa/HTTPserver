#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/time.h>

#define MAX_CLIENTS 100
#define BUFFER_SIZE 4096

typedef struct {
    char ip[INET_ADDRSTRLEN];
    int max_rate; // kbps do arquivo txt
    int active_connections;
    long last_request_ms;
    long object_size;
    double last_rtt;
    double last_bw;
} client_info_t;

client_info_t clients[MAX_CLIENTS];
int client_count = 0;
int global_max_rate = 1000; // kbps global
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// Retorna timestamp em ms
long current_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec*1000 + tv.tv_usec/1000;
}

// Procura IP na lista do txt
int load_qos_file(const char* filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        printf("Não foi possível abrir o arquivo %s\n", filename);
        return -1;
    }
    char ip[INET_ADDRSTRLEN];
    int rate;
    while(fscanf(file, "%15s %d", ip, &rate) == 2) {
        strncpy(clients[client_count].ip, ip, INET_ADDRSTRLEN);
        clients[client_count].max_rate = rate;
        clients[client_count].active_connections = 0;
        clients[client_count].last_request_ms = 0;
        clients[client_count].object_size = 0;
        clients[client_count].last_rtt = 0;
        clients[client_count].last_bw = 0;
        client_count++;
        if(client_count >= MAX_CLIENTS) break;
    }
    fclose(file);
    return 0;
}

// Encontra ou cria registro de cliente
client_info_t* get_client_record(const char* ip) {
    pthread_mutex_lock(&clients_mutex);
    for(int i=0;i<client_count;i++){
        if(strcmp(clients[i].ip, ip)==0){
            pthread_mutex_unlock(&clients_mutex);
            return &clients[i];
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return NULL;
}

void *handle_connection(void *client_socket_ptr) {
    int client_socket_fd = *(int*)client_socket_ptr;
    free(client_socket_ptr);

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    getpeername(client_socket_fd, (struct sockaddr*)&addr, &addr_len);
    char client_ip[INET_ADDRSTRLEN];
    strcpy(client_ip, inet_ntoa(addr.sin_addr));

    // --- Busca cliente na lista ou aplica taxa global ---
    client_info_t* client = get_client_record(client_ip);
    int assigned_rate;
    if(client) {
        pthread_mutex_lock(&clients_mutex);
        client->active_connections++;
        assigned_rate = client->max_rate / client->active_connections;
        pthread_mutex_unlock(&clients_mutex);
    } else {
        printf("[Servidor] IP %s não está na lista, atendendo com taxa máxima global %d kbps\n", client_ip, global_max_rate);
        assigned_rate = global_max_rate;
    }

    long now_ms = current_time_ms();
    double rtt = 0;
    if(client && client->last_request_ms>0){
        rtt = (double)(now_ms - client->last_request_ms);
        client->last_rtt = rtt;
    }
    if(client) client->last_request_ms = now_ms;

    printf("[Thread %ld] Cliente conectado: %s | Taxa: %d kbps | RTT estimado: %.2f ms\n",
           pthread_self(), client_ip, assigned_rate, rtt);

    // --- Envio de arquivo ---
    FILE *picture = fopen("carro.jpg","rb");
    if(!picture){
        char* error_msg = "HTTP/1.1 404 Not Found\r\n\r\n";
        send(client_socket_fd, error_msg, strlen(error_msg), 0);
        close(client_socket_fd);
        return NULL;
    }

    fseek(picture,0,SEEK_END);
    long picture_size = ftell(picture);
    rewind(picture);

    char http_header[1024];
    sprintf(http_header,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %ld\r\n"
            "\r\n",
            picture_size);
    send(client_socket_fd, http_header, strlen(http_header), 0);

    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    long sent_total = 0;
    long start_ms = current_time_ms();

    while((bytes_read=fread(buffer,1,sizeof(buffer),picture))>0){
        size_t sent_in_chunk = 0;
        while(sent_in_chunk < bytes_read){
            ssize_t sent_now = send(client_socket_fd, buffer + sent_in_chunk, bytes_read - sent_in_chunk, 0);
            if(sent_now <= 0) goto cleanup;
            sent_in_chunk += sent_now;
            sent_total += sent_now;

            // --- Simula limite de taxa por IP ---
            double time_sec = (double)sent_total*8 / (assigned_rate*1000); // kbps -> bps
            long elapsed_ms = current_time_ms() - start_ms;
            if(elapsed_ms < time_sec*1000){
                usleep((time_sec*1000 - elapsed_ms)*1000);
            }
        }
        // Progresso
        printf("[Thread %ld] %s | Progresso: %.2f%%\n", pthread_self(), client_ip,
               (double)sent_total/picture_size*100);
    }

cleanup:
    long end_ms = current_time_ms();
    double duration_sec = (end_ms - start_ms)/1000.0;
    double bw = (sent_total*8)/1000.0 / duration_sec; // kbps
    if(client){
        pthread_mutex_lock(&clients_mutex);
        client->last_bw = bw;
        client->active_connections--;
        pthread_mutex_unlock(&clients_mutex);
    }

    fclose(picture);
    shutdown(client_socket_fd, SHUT_WR);
    close(client_socket_fd);

    printf("[Thread %ld] Cliente %s finalizado | BW real: %.2f kbps\n", pthread_self(), client_ip, bw);
    return NULL;
}

int main(int argc, char** argv){
    if(argc<3){
        printf("Uso: %s <global_max_rate> <qos_file>\n", argv[0]);
        return 1;
    }
    global_max_rate = atoi(argv[1]);
    load_qos_file(argv[2]);

    int s = socket(PF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in serverSa;
    memset(&serverSa,0,sizeof(serverSa));
    serverSa.sin_family = AF_INET;
    serverSa.sin_addr.s_addr = htonl(INADDR_ANY);
    serverSa.sin_port = htons(5000);

    if(bind(s,(struct sockaddr*)&serverSa,sizeof(serverSa))<0){
        perror("bind failed");
        return 1;
    }
    if(listen(s,10)<0){
        perror("listen failed");
        return 1;
    }

    printf("Vazão global: %d kbps\n", global_max_rate);
    printf("Servidor escutando na porta 5000...\n");

    while(1){
        struct sockaddr_in clientSa;
        socklen_t clientSaSize = sizeof(clientSa);
        int client_socket_fd = accept(s,(struct sockaddr*)&clientSa,&clientSaSize);
        if(client_socket_fd<0) continue;

        pthread_t tid;
        int *ptr = malloc(sizeof(int));
        *ptr = client_socket_fd;
        pthread_create(&tid,NULL,handle_connection,ptr);
        pthread_detach(tid);
    }

    close(s);
    return 0;
}
