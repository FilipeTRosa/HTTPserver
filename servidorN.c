// servidor_realtime.c
// Servidor HTTP multithread com RTT, banda por cliente e log em tempo real
// Bianca Durgante - Projeto Redes UNIPAMPA

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/stat.h>

#define PORTA_PADRAO 5000
#define MAX_CLIENTES 100
#define BUF_SIZE 4096
#define TX_PADRAO 1000 // kB/s padrão para IPs não listados
#define MAX_QOS 100

typedef struct {
    char ip[INET_ADDRSTRLEN];
    double taxa_kBps;
} QoS_IP;

typedef struct {
    char ip[INET_ADDRSTRLEN];
    double last_rtt;
    double last_bandwidth;
    struct timeval last_request_time;
    int requisicoes;
    pthread_t thread_id;
} ClienteInfo;

ClienteInfo clientes[MAX_CLIENTES];
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;
QoS_IP qos_ips[MAX_QOS];
int qos_count = 0;

double vazao_max = 10000; // kB/s
double vazao_atual = 0.0;

// Funções
void *atender_cliente(void *arg);
void *monitorar_clientes(void *arg);
void enviar_arquivo(int sock, const char *nome_arquivo, double taxa_kBps);
int buscar_cliente(const char *ip);
int registrar_cliente(const char *ip);
double calcular_tempo(struct timeval inicio, struct timeval fim);
double buscar_taxa_ip(const char *ip);
void carregar_qos(const char *arquivo_qos);
long tamanho_arquivo_kb(const char *nome_arquivo);
void log_requisicao(const char *ip, double rtt, double banda, int req, pthread_t tid);
int pode_aceitar(double banda_solicitada);

int main(int argc, char *argv[]) {
    int server_fd;
    struct sockaddr_in address;
    pthread_t thread_monitor;

    int porta = (argc > 1) ? atoi(argv[1]) : PORTA_PADRAO;
    const char *arquivo_qos = (argc > 2) ? argv[2] : "ips.txt";
    vazao_max = (argc > 3) ? atof(argv[3]) : 10000;

    // Inicializa clientes
    for (int i = 0; i < MAX_CLIENTES; i++)
        clientes[i].requisicoes = 0;

    carregar_qos(arquivo_qos);

    // Criação do socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Erro ao criar socket");
        exit(EXIT_FAILURE);
    }

    int on = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(porta);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Erro no bind");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("Erro no listen");
        exit(EXIT_FAILURE);
    }

    printf("Servidor iniciado na porta %d...\n", porta);
    printf("Vazão máxima do servidor: %.2f kB/s\n", vazao_max);

    pthread_create(&thread_monitor, NULL, monitorar_clientes, NULL);

    while (1) {
        struct sockaddr_in cliente_addr;
        socklen_t cliente_len = sizeof(cliente_addr);
        int new_socket = accept(server_fd, (struct sockaddr *)&cliente_addr, &cliente_len);
        if (new_socket < 0) {
            perror("Erro no accept");
            continue;
        }

        // Cria thread para atender o cliente
        pthread_t t;
        struct {
            int sock;
            struct sockaddr_in addr;
        } *arg_thread = malloc(sizeof(*arg_thread));
        arg_thread->sock = new_socket;
        arg_thread->addr = cliente_addr;
        pthread_create(&t, NULL, atender_cliente, arg_thread);
        pthread_detach(t);
    }

    close(server_fd);
    return 0;
}

// Carrega arquivo QoS
void carregar_qos(const char *arquivo_qos) {
    FILE *fp = fopen(arquivo_qos, "r");
    if (!fp) {
        perror("Erro ao abrir arquivo QoS");
        return;
    }
    qos_count = 0;
    while (fscanf(fp, "%s %lf", qos_ips[qos_count].ip, &qos_ips[qos_count].taxa_kBps) == 2) {
        qos_count++;
        if (qos_count >= MAX_QOS) break;
    }
    fclose(fp);
    printf("QoS carregado: %d IPs\n", qos_count);
}

// Busca taxa para IP
double buscar_taxa_ip(const char *ip) {
    for (int i = 0; i < qos_count; i++)
        if (strcmp(ip, qos_ips[i].ip) == 0)
            return qos_ips[i].taxa_kBps;
    return TX_PADRAO;
}

// Verifica se pode aceitar nova requisição sem ultrapassar vazão
int pode_aceitar(double banda_solicitada) {
    pthread_mutex_lock(&lock);
    int resultado = (vazao_atual + banda_solicitada <= vazao_max);
    pthread_mutex_unlock(&lock);
    return resultado;
}

// Atendimento de cada cliente
void *atender_cliente(void *arg) {
    struct {
        int sock;
        struct sockaddr_in addr;
    } *dados = arg;

    int sock = dados->sock;
    struct sockaddr_in cliente_addr = dados->addr;
    free(dados);

    char ip_cliente[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cliente_addr.sin_addr, ip_cliente, INET_ADDRSTRLEN);

    double taxa_cliente = buscar_taxa_ip(ip_cliente);

    if (!pode_aceitar(taxa_cliente)) {
        pthread_mutex_lock(&print_lock);
        printf("Rejeitado: %s - limite de banda atingido (%.2f / %.2f kB/s)\n",
               ip_cliente, vazao_atual, vazao_max);
        pthread_mutex_unlock(&print_lock);
        close(sock);
        return NULL;
    }

    pthread_mutex_lock(&lock);
    vazao_atual += taxa_cliente;
    int idx = buscar_cliente(ip_cliente);
    if (idx == -1) idx = registrar_cliente(ip_cliente);
    pthread_mutex_unlock(&lock);

    char buffer[BUF_SIZE];
    ssize_t n = read(sock, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        close(sock);
        pthread_mutex_lock(&lock); vazao_atual -= taxa_cliente; pthread_mutex_unlock(&lock);
        return NULL;
    }
    buffer[n] = '\0';

    char metodo[8], caminho[128];
    sscanf(buffer, "%s %s", metodo, caminho);

    const char *arquivo = NULL;
    if (strcmp(caminho, "/html") == 0) arquivo = "html_simulado.txt";
    else if (strcmp(caminho, "/gato.jpg") == 0) arquivo = "gato.jpg";
    else if (strcmp(caminho, "/banda.jpg") == 0) arquivo = "banda.jpg";
    else if (strcmp(caminho, "/carro.jpg") == 0) arquivo = "carro.jpg";
    else if (strcmp(caminho, "/jogo.jpg") == 0) arquivo = "jogo.jpg";
    else {
        const char *msg = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send(sock, msg, strlen(msg), 0);
        close(sock);
        pthread_mutex_lock(&lock); vazao_atual -= taxa_cliente; pthread_mutex_unlock(&lock);
        return NULL;
    }

    struct timeval inicio, fim;
    gettimeofday(&inicio, NULL);

    enviar_arquivo(sock, arquivo, taxa_cliente);

    gettimeofday(&fim, NULL);
    double duracao = calcular_tempo(inicio, fim);
    long tamanho_kB = tamanho_arquivo_kb(arquivo);

    pthread_mutex_lock(&lock);
    ClienteInfo *cli = &clientes[idx];
    cli->last_rtt = (cli->requisicoes > 0) ? calcular_tempo(cli->last_request_time, inicio) : 0;
    cli->last_bandwidth = tamanho_kB / duracao;
    cli->last_request_time = inicio;
    cli->requisicoes++;
    cli->thread_id = pthread_self();
    vazao_atual -= taxa_cliente;
    pthread_mutex_unlock(&lock);

    log_requisicao(ip_cliente, cli->last_rtt, cli->last_bandwidth, cli->requisicoes, cli->thread_id);

    close(sock);
    return NULL;
}

// Função para envio de arquivo com controle de banda
void enviar_arquivo(int sock, const char *nome_arquivo, double taxa_kBps) {
    FILE *fp = fopen(nome_arquivo, "rb");
    if (!fp) {
        const char *msg = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send(sock, msg, strlen(msg), 0);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long tamanho = ftell(fp);
    rewind(fp);

    char header[128];
    sprintf(header, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", tamanho);
    send(sock, header, strlen(header), 0);

    char buf[BUF_SIZE];
    size_t bytes;
    useconds_t atraso = (useconds_t)(1000000 * (BUF_SIZE / 1024.0) / taxa_kBps);

    while ((bytes = fread(buf, 1, sizeof(buf), fp)) > 0) {
        send(sock, buf, bytes, 0);
        usleep(atraso);
    }

    fclose(fp);
}

// Busca cliente existente
int buscar_cliente(const char *ip) {
    for (int i = 0; i < MAX_CLIENTES; i++)
        if (clientes[i].requisicoes > 0 && strcmp(clientes[i].ip, ip) == 0)
            return i;
    return -1;
}

// Registra novo cliente
int registrar_cliente(const char *ip) {
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (clientes[i].requisicoes == 0) {
            strcpy(clientes[i].ip, ip);
            clientes[i].last_rtt = 0.0;
            clientes[i].last_bandwidth = 0.0;
            clientes[i].requisicoes = 0;
            gettimeofday(&clientes[i].last_request_time, NULL);
            return i;
        }
    }
    return -1;
}

// Calcula tempo em segundos
double calcular_tempo(struct timeval inicio, struct timeval fim) {
    return (fim.tv_sec - inicio.tv_sec) + (fim.tv_usec - inicio.tv_usec) / 1e6;
}

// Log em tempo real
void log_requisicao(const char *ip, double rtt, double banda, int req, pthread_t tid) {
    pthread_mutex_lock(&print_lock);
    printf("%-15s | %-8.3f | %-12.2f | %-4d | %lu\n",
           ip, rtt, banda, req, (unsigned long)tid);
    pthread_mutex_unlock(&print_lock);
}

// Tamanho do arquivo em KB
long tamanho_arquivo_kb(const char *nome_arquivo) {
    struct stat st;
    if (stat(nome_arquivo, &st) != 0) return -1;
    return st.st_size / 1024;
}

// Monitoramento final resumido (pode ser chamado manualmente se quiser)
void *monitorar_clientes(void *arg) {
    (void)arg;
    while (1) {
        sleep(5); // imprime a cada 5s
        pthread_mutex_lock(&print_lock);
        printf("\n=== HISTÓRICO DE REQUISIÇÕES ===\n");
        printf("%-15s | %-8s | %-12s | %-4s | %-10s\n", 
               "IP", "RTT(s)", "Banda(kB/s)", "Req", "Thread");
        printf("--------------------------------------------------------------\n");
        pthread_mutex_lock(&lock);
        for (int i = 0; i < MAX_CLIENTES; i++) {
            if (clientes[i].requisicoes > 0) {
                printf("%-15s | %-8.3f | %-12.2f | %-4d | %lu\n",
                       clientes[i].ip,
                       clientes[i].last_rtt,
                       clientes[i].last_bandwidth,
                       clientes[i].requisicoes,
                       (unsigned long)clientes[i].thread_id);
            }
        }
        printf("Vazão atual do servidor: %.2f / %.2f kB/s\n", vazao_atual, vazao_max);
        pthread_mutex_unlock(&lock);
        pthread_mutex_unlock(&print_lock);
    }
    return NULL;
}
