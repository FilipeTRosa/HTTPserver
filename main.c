/* 
   A very simple HTTP socket server that sends an image file.
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h> 

int main(int argc, const char **argv)
{
  int serverPort = 5000;
  int rc;
  struct sockaddr_in serverSa;
  struct sockaddr_in clientSa;
  socklen_t clientSaSize = sizeof(clientSa); 
  int on = 1;
  int c;
  int s = socket(PF_INET, SOCK_STREAM, 0);
  rc = setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  
  memset(&serverSa, 0, sizeof(serverSa));
  serverSa.sin_family = AF_INET;
  serverSa.sin_addr.s_addr = htonl(INADDR_ANY);
  serverSa.sin_port = htons(serverPort);
  rc = bind(s, (struct sockaddr *)&serverSa, sizeof(serverSa));
  if (rc < 0)
  {
    perror("bind failed");
    exit(1);
  }

  rc = listen(s, 10);
  if (rc < 0)
  {
    perror("listen failed");
    exit(1);
  }

  printf("Servidor escutando na porta %d. Aguardando conexões...\n", serverPort);
 while (1)
  {
    int client_socket_fd = accept(s, (struct sockaddr *)&clientSa, &clientSaSize);
    if (client_socket_fd < 0)
    {
      perror("accept failed");
      continue;
    }

    printf("Cliente conectado: %s\n", inet_ntoa(clientSa.sin_addr));

    FILE *picture = fopen("carro.jpg", "rb");
    if (picture == NULL) {
        printf("Erro: arquivo minha_foto.jpg não encontrado.\n");
        char* error_msg = "HTTP/1.1 404 Not Found\r\n\r\n";
        send(client_socket_fd, error_msg, strlen(error_msg), 0);
        close(client_socket_fd);
        continue;
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
        continue;
    }
    printf("Cabeçalhos HTTP enviados.\n");

  
    char buffer[4096];
    size_t bytes_read;
    long total_bytes_sent_from_file = 0; // Contador para o total de dados da imagem enviados
    int loop_count = 0; // Contador para ver quantas vezes o loop de leitura roda

    // Loop principal para ler o arquivo
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), picture)) > 0) {
        loop_count++;
        printf("Loop de leitura #%d: Li %zu bytes do arquivo.\n", loop_count, bytes_read);
        
        size_t total_sent_in_chunk = 0;
        // Loop interno para garantir que todo o buffer seja enviado
        while (total_sent_in_chunk < bytes_read) {
            ssize_t sent_now = send(client_socket_fd, 
                                    buffer + total_sent_in_chunk,
                                    bytes_read - total_sent_in_chunk,
                                    0);

            if (sent_now < 0) {
                perror("ERRO no envio dos dados da imagem");
                goto cleanup; 
            }
            total_sent_in_chunk += sent_now;
        }
        total_bytes_sent_from_file += total_sent_in_chunk;
        printf("--> Enviei um total de %zu bytes deste pedaço.\n", total_sent_in_chunk);
    }
    
    printf("\n--- FIM DA TRANSMISSÃO ---\n");
    printf("Tamanho original do arquivo: %ld bytes\n", picture_size);
    printf("Total de bytes da imagem enviados: %ld bytes\n", total_bytes_sent_from_file);
    printf("O loop de leitura do arquivo rodou %d vezes.\n", loop_count);

cleanup: // Rótulo para limpeza
    fclose(picture);
    // Avisa que não vamos mais escrever, permitindo que o buffer seja esvaziado
    shutdown(client_socket_fd, SHUT_WR); 
    // Agora sim, fechamos o descritor de arquivo do socket
    close(client_socket_fd);
    
    printf("Conexão com o cliente fechada.\n\n");
  }

  close(s);
  return 0;
}