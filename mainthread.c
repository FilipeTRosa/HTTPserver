#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h> 

//lógica de comunicação com o cliente 
void *handle_connection(void *client_socket_ptr) {
    // 1. Receber e converter o ponteiro do socket para o tipo correto
    int client_socket_fd = *(int*)client_socket_ptr;
    // Libera a memória que foi alocada na main para passar o argumento
    free(client_socket_ptr);

    printf("[Thread %ld] Atendendo cliente.\n", pthread_self());

    // --- LÓGICA DE ENVIO
    FILE *picture = fopen("carro.jpg", "rb");
    if (picture == NULL) {
        printf("[Thread %ld] Erro: arquivo minha_foto.jpg não encontrado.\n", pthread_self());
        char* error_msg = "HTTP/1.1 404 Not Found\r\n\r\n";
        send(client_socket_fd, error_msg, strlen(error_msg), 0);
        close(client_socket_fd);
        return NULL;
    }

    fseek(picture, 0, SEEK_END);
    long picture_size = ftell(picture);
    rewind(picture);

    char http_header[1024];
    sprintf(http_header, "HTTP/1.1 200 OK\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "Content-Length: %ld\r\n"
                         "\r\n",
                         picture_size);

    if (send(client_socket_fd, http_header, strlen(http_header), 0) < 0) {
        perror("send header failed");
        fclose(picture);
        close(client_socket_fd);
        return NULL;
    }
    printf("[Thread %ld] Cabeçalhos HTTP enviados.\n", pthread_self());

    char buffer[4096];
    size_t bytes_read;

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), picture)) > 0) {
        size_t total_sent_in_chunk = 0;
        while (total_sent_in_chunk < bytes_read) {
            ssize_t sent_now = send(client_socket_fd, buffer + total_sent_in_chunk, bytes_read - total_sent_in_chunk, 0);
            if (sent_now < 0) {
                perror("send picture data failed");
                goto cleanup;
            }
            total_sent_in_chunk += sent_now;
        }
    }
    printf("[Thread %ld] Dados da foto enviados. Tamanho: %ld bytes\n", pthread_self(), picture_size);

cleanup:
    fclose(picture);
    shutdown(client_socket_fd, SHUT_WR);
    close(client_socket_fd);
    printf("[Thread %ld] Conexão com o cliente fechada.\n", pthread_self());
    return NULL;
}

int main(int argc, const char **argv)
{
  int serverPort = 5000;
  int s = socket(PF_INET, SOCK_STREAM, 0);
  int on = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  
  struct sockaddr_in serverSa;
  memset(&serverSa, 0, sizeof(serverSa));
  serverSa.sin_family = AF_INET;
  serverSa.sin_addr.s_addr = htonl(INADDR_ANY);
  serverSa.sin_port = htons(serverPort);
  
  if (bind(s, (struct sockaddr *)&serverSa, sizeof(serverSa)) < 0) {
    perror("bind failed");
    exit(1);
  }

  if (listen(s, 10) < 0) {
    perror("listen failed");
    exit(1);
  }
  printf("Servidor escutando na porta %d. Aguardando conexões...\n", serverPort);

  while (1)
  {
    struct sockaddr_in clientSa;
    socklen_t clientSaSize = sizeof(clientSa);
    
    int client_socket_fd = accept(s, (struct sockaddr *)&clientSa, &clientSaSize);
    if (client_socket_fd < 0) {
      perror("accept failed");
      continue;
    }

    printf("[Main] Cliente conectado: %s. Criando thread para atendê-lo.\n", inet_ntoa(clientSa.sin_addr));

    // --- LÓGICA DE CRIAÇÃO DE THREADS ---
    pthread_t thread_id;
    // Aloca memória para passar o descritor do socket para a thread
    int *client_socket_ptr = malloc(sizeof(int));
    *client_socket_ptr = client_socket_fd;

    // Cria a nova thread, passando a função 'handle_connection' e o ponteiro do socket
    if (pthread_create(&thread_id, NULL, handle_connection, (void*)client_socket_ptr) != 0) {
        perror("pthread_create failed");
        free(client_socket_ptr);
        close(client_socket_fd);
    }
    // Desanexa a thread para que seus recursos sejam liberados automaticamente ao terminar
    pthread_detach(thread_id);
  }

  close(s);
  return 0;
}