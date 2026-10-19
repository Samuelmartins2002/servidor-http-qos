/*

 * Servidor HTTP/1.1 concorrente - Versao 1 (MVP)

 * Fundamentos e Avaliacao de Redes de Computadores - 2026/2

 *

 * Funcionalidades da V1:

 *  - Uma thread (Pthreads) por conexao -> varios clientes simultaneos

 *  - HTTP/1.1 com conexoes persistentes (keep-alive)

 *  - Metodos GET e HEAD, entrega de arquivos estaticos

 *  - Respostas de erro 400, 404, 405, 505 (timeout de ociosidade fecha a conexao)

 *

 * Desenvolvido com apoio de IA (Claude, Anthropic), a partir do exemplo

 * base de sockets da equipe; revisado e testado pelos integrantes.

 *

 * Uso: ./servidor [porta] [diretorio_raiz]

 *

 * Nota: o envio de arquivos e feito em blocos (BLOCO_ENVIO) dentro da

 * funcao enviar_arquivo(); e nesse laco que o controle de taxa da V2

 * sera inserido.

 *

 * OBS: Código desenvolvido com auxílio de inteligência artificial (IA)

 */

#define _GNU_SOURCE

#include <stdio.h>

#include <stdarg.h>

#include <stdlib.h>

#include <string.h>

#include <unistd.h>

#include <errno.h>

#include <signal.h>

#include <pthread.h>

#include <fcntl.h>

#include <time.h>

#include <sys/stat.h>

#include <sys/socket.h>

#include <sys/time.h>

#include <netinet/in.h>

#include <arpa/inet.h>
#include <math.h>
#include <ctype.h>



#define PORTA_PADRAO     8080

#define RAIZ_PADRAO      "www"

#define BACKLOG          64

#define TAM_REQ_MAX      8192   /* tamanho maximo do cabecalho da requisicao */

#define BLOCO_ENVIO      4096   /* bytes por send() ao enviar arquivos */

#define TIMEOUT_KA_SEG   15     /* tempo ocioso maximo de uma conexao persistente */

#define MAX_REQ_CONEXAO  100    /* maximo de requisicoes por conexao */



static const char *raiz = RAIZ_PADRAO;

static pthread_mutex_t mutex_log = PTHREAD_MUTEX_INITIALIZER;

/* QoS: tabela de taxas, conexoes admitidas e limitador compartilhado por IP. */
#define MAX_IPS 1024
#define TAXA_PADRAO_KBPS 1000.0

typedef struct {
    char ip[INET_ADDRSTRLEN];
    double taxa_kbps;
    int conexoes;
    double proximo_envio; /* relogio monotônico em segundos */
    int usado;
    double html_time;
    int aguardando_objeto;
    double rtt_ms;
    double banda_estim_kbps;
    int ativo_transferencia;
} estado_ip_t;

static estado_ip_t estados[MAX_IPS];
static pthread_mutex_t mutex_qos = PTHREAD_MUTEX_INITIALIZER;
static double vazao_max_kbps = 10000.0;
static double vazao_reservada_kbps = 0.0;

static double agora_monotono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static estado_ip_t *estado_ip_locked(const char *ip, int criar) {
    estado_ip_t *livre = NULL;
    for (int i = 0; i < MAX_IPS; i++) {
        if (estados[i].usado && strcmp(estados[i].ip, ip) == 0) return &estados[i];
        if (!estados[i].usado && !livre) livre = &estados[i];
    }
    if (!criar || !livre) return NULL;
    memset(livre, 0, sizeof(*livre));
    snprintf(livre->ip, sizeof(livre->ip), "%s", ip);
    livre->taxa_kbps = TAXA_PADRAO_KBPS;
    livre->usado = 1;
    return livre;
}

static void carregar_taxas(const char *arquivo) {
    FILE *f = fopen(arquivo, "r");
    if (!f) { perror("Aviso: nao foi possivel abrir arquivo de taxas; usando 1000 kbps para todos"); return; }
    char linha[256];
    while (fgets(linha, sizeof(linha), f)) {
        char ip[INET_ADDRSTRLEN], extra;
        double taxa;
        char *p = linha;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0') continue;
        if (sscanf(p, "%15s %lf %c", ip, &taxa, &extra) < 2 || taxa <= 0) {
            fprintf(stderr, "Linha ignorada no arquivo de taxas: %s", linha); continue;
        }
        struct in_addr addr;
        if (inet_pton(AF_INET, ip, &addr) != 1) {
            fprintf(stderr, "IP invalido no arquivo de taxas: %s\n", ip); continue;
        }
        pthread_mutex_lock(&mutex_qos);
        estado_ip_t *e = estado_ip_locked(ip, 1);
        if (e) e->taxa_kbps = taxa;
        pthread_mutex_unlock(&mutex_qos);
    }
    fclose(f);
}


/* Reserva a taxa apenas uma vez por IP: varias conexoes do mesmo IP compartilham-na. */
static int admitir_cliente(const char *ip) {
    int ok = 0;
    pthread_mutex_lock(&mutex_qos);
    estado_ip_t *e = estado_ip_locked(ip, 1);
    if (e) {
        if (e->conexoes > 0 || vazao_reservada_kbps + e->taxa_kbps <= vazao_max_kbps) {
            if (e->conexoes == 0) vazao_reservada_kbps += e->taxa_kbps;
            e->conexoes++;
            ok = 1;
        }
    }
    pthread_mutex_unlock(&mutex_qos);
    return ok;
}

static void liberar_cliente(const char *ip) {
    pthread_mutex_lock(&mutex_qos);
    estado_ip_t *e = estado_ip_locked(ip, 0);
    if (e && e->conexoes > 0) {
        e->conexoes--;
        if (e->conexoes == 0) {
            vazao_reservada_kbps -= e->taxa_kbps;
            if (vazao_reservada_kbps < 0) vazao_reservada_kbps = 0;
            e->ativo_transferencia = 0;
        }
    }
    pthread_mutex_unlock(&mutex_qos);
}

/* Agenda cada bloco em um relogio compartilhado por IP; evita que threads
   concorrentes multipliquem a taxa permitida para o mesmo cliente. */
static void limitar_taxa_ip(const char *ip, size_t bytes) {
    double espera;
    pthread_mutex_lock(&mutex_qos);
    estado_ip_t *e = estado_ip_locked(ip, 1);
    double taxa = e ? e->taxa_kbps : TAXA_PADRAO_KBPS;
    double agora = agora_monotono();
    double inicio = (e && e->proximo_envio > agora) ? e->proximo_envio : agora;
    double duracao = ((double)bytes * 8.0) / (taxa * 1000.0);
    if (e) e->proximo_envio = inicio + duracao;
    espera = inicio - agora;
    pthread_mutex_unlock(&mutex_qos);
    while (espera > 0) {
        struct timespec ts;
        ts.tv_sec = (time_t)espera;
        ts.tv_nsec = (long)((espera - (double)ts.tv_sec) * 1e9);
        if (nanosleep(&ts, &ts) == 0) break;
        if (errno != EINTR) break;
        espera = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    }
}

/* RTT aproximado: tempo entre pedido do HTML e pedido do primeiro objeto. */
static void registrar_requisicao_qos(const char *ip, const char *caminho) {
    double agora = agora_monotono();
    int eh_html = (strstr(caminho, ".html") || strstr(caminho, ".htm") || strcmp(caminho, "/") == 0);
    pthread_mutex_lock(&mutex_qos);
    estado_ip_t *e = estado_ip_locked(ip, 1);
    if (e) {
        if (eh_html) {
            e->html_time = agora;
            e->aguardando_objeto = 1;
            e->rtt_ms = 0;
            e->banda_estim_kbps = 0;
        } else if (e->aguardando_objeto) {
            e->rtt_ms = (agora - e->html_time) * 1000.0;
            /* Estimativa de banda configurada; RTT e banda aparecem separadamente. */
            e->banda_estim_kbps = e->taxa_kbps;
            e->aguardando_objeto = 0;
        }
    }
    pthread_mutex_unlock(&mutex_qos);
}

static void mostrar_estado_qos(const char *ip, const char *caminho) {
    pthread_mutex_lock(&mutex_qos);
    estado_ip_t *e = estado_ip_locked(ip, 0);
    if (e) {
        printf("[QoS] IP=%s taxa_limite=%.0f kbps conexoes=%d reserva=%.0f/%.0f kbps",
               ip, e->taxa_kbps, e->conexoes, vazao_reservada_kbps, vazao_max_kbps);
        if (e->rtt_ms > 0) printf(" RTT_estimado=%.2f ms banda_configurada=%.0f kbps", e->rtt_ms, e->banda_estim_kbps);
        printf(" requisicao=%s\n", caminho);
        fflush(stdout);
    }
    pthread_mutex_unlock(&mutex_qos);
}




typedef struct {

    int fd;

    struct sockaddr_in addr;

} cliente_t;



typedef struct {

    char metodo[8];

    char caminho[1024];

    char versao[16];

    int  keep_alive;

} requisicao_t;



/* ------------------------------------------------------------------ */

/* Utilitarios                                                         */

/* ------------------------------------------------------------------ */



static void log_msg(const char *ip, const char *fmt, ...)

{

    char hora[16];

    time_t t = time(NULL);

    struct tm tm;

    localtime_r(&t, &tm);

    strftime(hora, sizeof(hora), "%H:%M:%S", &tm);



    pthread_mutex_lock(&mutex_log);

    printf("[%s] %-15s ", hora, ip);

    va_list ap;

    va_start(ap, fmt);

    vprintf(fmt, ap);

    va_end(ap);

    printf("\n");

    fflush(stdout);

    pthread_mutex_unlock(&mutex_log);

}



/* Envia exatamente n bytes (trata envio parcial). Retorna 0 ok, -1 erro. */

static int enviar_tudo(int fd, const char *dados, size_t n)

{

    size_t enviado = 0;

    while (enviado < n) {

        ssize_t r = send(fd, dados + enviado, n - enviado, MSG_NOSIGNAL);

        if (r < 0) {

            if (errno == EINTR) continue;

            return -1;

        }

        enviado += (size_t)r;

    }

    return 0;

}



static const char *tipo_mime(const char *caminho)

{

    const char *ext = strrchr(caminho, '.');

    if (!ext) return "application/octet-stream";

    if (!strcasecmp(ext, ".html") || !strcasecmp(ext, ".htm")) return "text/html; charset=utf-8";

    if (!strcasecmp(ext, ".css"))  return "text/css";

    if (!strcasecmp(ext, ".js"))   return "application/javascript";

    if (!strcasecmp(ext, ".txt"))  return "text/plain; charset=utf-8";

    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) return "image/jpeg";

    if (!strcasecmp(ext, ".png"))  return "image/png";

    if (!strcasecmp(ext, ".gif"))  return "image/gif";

    if (!strcasecmp(ext, ".bmp"))  return "image/bmp";

    if (!strcasecmp(ext, ".webp")) return "image/webp";

    if (!strcasecmp(ext, ".ico"))  return "image/x-icon";

    if (!strcasecmp(ext, ".pdf"))  return "application/pdf";

    return "application/octet-stream";

}



static const char *texto_status(int codigo)

{

    switch (codigo) {

        case 200: return "OK";

        case 400: return "Bad Request";

        case 404: return "Not Found";

        case 405: return "Method Not Allowed";

        case 500: return "Internal Server Error";

        case 505: return "HTTP Version Not Supported";

        default:  return "Unknown";

    }

}



/* Decodifica %XX em place. Retorna -1 se invalido. */

static int url_decode(char *s)

{

    char *o = s;

    while (*s) {

        if (*s == '%') {

            if (!s[1] || !s[2]) return -1;

            char h[3] = { s[1], s[2], 0 };

            char *fim;

            long v = strtol(h, &fim, 16);

            if (*fim) return -1;

            *o++ = (char)v;

            s += 3;

        } else {

            *o++ = *s++;

        }

    }

    *o = '\0';

    return 0;

}



/* ------------------------------------------------------------------ */

/* Respostas                                                           */

/* ------------------------------------------------------------------ */



static int enviar_erro(int fd, int codigo, int keep_alive, int so_cabecalho)

{

    char corpo[256], cab[512];

    int nc = snprintf(corpo, sizeof(corpo),

        "<html><body><h1>%d %s</h1></body></html>\n", codigo, texto_status(codigo));

    int nh = snprintf(cab, sizeof(cab),

        "HTTP/1.1 %d %s\r\n"

        "Content-Type: text/html; charset=utf-8\r\n"

        "Content-Length: %d\r\n"

        "Connection: %s\r\n"

        "%s"

        "\r\n",

        codigo, texto_status(codigo), nc,

        keep_alive ? "keep-alive" : "close",

        codigo == 405 ? "Allow: GET, HEAD\r\n" : "");

    if (enviar_tudo(fd, cab, (size_t)nh) < 0) return -1;

    if (!so_cabecalho && enviar_tudo(fd, corpo, (size_t)nc) < 0) return -1;

    return 0;

}



/* Retorna 0 se enviou ok, -1 se houve erro de conexao. */

static int enviar_arquivo(int fd, const char *ip, const requisicao_t *rq)

{

    char caminho_fs[2048];

    char url[1024];



    /* separa a query string (?...) e decodifica %XX */

    strncpy(url, rq->caminho, sizeof(url) - 1);

    url[sizeof(url) - 1] = '\0';

    char *q = strchr(url, '?');

    if (q) *q = '\0';

    if (url_decode(url) < 0 || url[0] != '/' || strstr(url, ".."))

        return enviar_erro(fd, 400, rq->keep_alive, 0);



    if (strcmp(url, "/") == 0) strcpy(url, "/index.html");

    snprintf(caminho_fs, sizeof(caminho_fs), "%s%s", raiz, url);



    struct stat st;

    int arq = open(caminho_fs, O_RDONLY);

    if (arq < 0 || fstat(arq, &st) < 0 || !S_ISREG(st.st_mode)) {

        if (arq >= 0) close(arq);

        log_msg(ip, "%s %s -> 404", rq->metodo, rq->caminho);

        return enviar_erro(fd, 404, rq->keep_alive, strcmp(rq->metodo, "HEAD") == 0);

    }



    char cab[512];

    int nh = snprintf(cab, sizeof(cab),

        "HTTP/1.1 200 OK\r\n"

        "Content-Type: %s\r\n"

        "Content-Length: %lld\r\n"

        "Connection: %s\r\n"

        "%s"

        "\r\n",

        tipo_mime(caminho_fs), (long long)st.st_size,

        rq->keep_alive ? "keep-alive" : "close",

        rq->keep_alive ? "Keep-Alive: timeout=15, max=100\r\n" : "");



    if (enviar_tudo(fd, cab, (size_t)nh) < 0) { close(arq); return -1; }



    int ret = 0;

    if (strcmp(rq->metodo, "GET") == 0) {

        char bloco[BLOCO_ENVIO];

        ssize_t n;

        /* >>> V2: o controle de taxa por IP entrara neste laco <<< */

        while ((n = read(arq, bloco, sizeof(bloco))) > 0) {
            const char *ext = strrchr(caminho_fs, '.');
            int eh_html = ext && (!strcasecmp(ext, ".html") || !strcasecmp(ext, ".htm"));
            if (!eh_html) limitar_taxa_ip(ip, (size_t)n);
            if (enviar_tudo(fd, bloco, (size_t)n) < 0) { ret = -1; break; }

        }

        if (n < 0) ret = -1;

    }

    close(arq);

    log_msg(ip, "%s %s -> 200 (%lld bytes)%s", rq->metodo, rq->caminho,

            (long long)st.st_size, ret < 0 ? " [interrompido]" : "");

    return ret;

}



/* ------------------------------------------------------------------ */

/* Leitura e parsing da requisicao                                     */

/* ------------------------------------------------------------------ */



/*

 * Le do socket ate encontrar "\r\n\r\n". 'buf' mantem bytes ja lidos

 * (*len), inclusive de requisicoes seguintes (pipelining).

 * Retorna: tamanho do cabecalho (>0) | 0 conexao fechada/timeout | -1 muito grande

 */

static ssize_t ler_cabecalho(int fd, char *buf, size_t *len)

{

    for (;;) {

        buf[*len] = '\0';

        char *fim = strstr(buf, "\r\n\r\n");

        if (fim) return (fim - buf) + 4;

        if (*len >= TAM_REQ_MAX) return -1;

        ssize_t r = recv(fd, buf + *len, TAM_REQ_MAX - *len, 0);

        if (r < 0 && errno == EINTR) continue;

        if (r <= 0) return 0;   /* fechou, erro ou timeout */

        *len += (size_t)r;

    }

}



/* Retorna 0 ok, -1 requisicao mal formada. Preenche rq. */

static int parsear_requisicao(char *cab, requisicao_t *rq)

{

    memset(rq, 0, sizeof(*rq));

    char *fim_linha = strstr(cab, "\r\n");

    if (!fim_linha) return -1;

    *fim_linha = '\0';



    char m[8], c[1024], v[16], extra[2];

    if (sscanf(cab, "%7s %1023s %15s %1s", m, c, v, extra) != 3) return -1;

    strcpy(rq->metodo, m);

    strcpy(rq->caminho, c);

    strcpy(rq->versao, v);



    if (strncmp(v, "HTTP/", 5) != 0) return -1;



    /* padrao: 1.1 persistente, 1.0 fecha */

    rq->keep_alive = (strcmp(v, "HTTP/1.1") == 0);



    /* percorre os cabecalhos procurando "Connection" */

    char *p = fim_linha + 2;

    while (*p) {

        char *eol = strstr(p, "\r\n");

        if (!eol || eol == p) break;

        *eol = '\0';

        if (strncasecmp(p, "Connection:", 11) == 0) {

            const char *val = p + 11;

            while (*val == ' ' || *val == '\t') val++;

            if (strcasecmp(val, "close") == 0) rq->keep_alive = 0;

            else if (strcasecmp(val, "keep-alive") == 0) rq->keep_alive = 1;

        }

        p = eol + 2;

    }

    return 0;

}



/* ------------------------------------------------------------------ */

/* Thread de atendimento                                               */

/* ------------------------------------------------------------------ */



static void *atender_cliente(void *arg)

{

    cliente_t *cli = (cliente_t *)arg;

    int fd = cli->fd;

    char ip[INET_ADDRSTRLEN];

    inet_ntop(AF_INET, &cli->addr.sin_addr, ip, sizeof(ip));

    free(cli);



    /* timeout de leitura: encerra conexoes persistentes ociosas */

    struct timeval tv = { .tv_sec = TIMEOUT_KA_SEG, .tv_usec = 0 };

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));



    log_msg(ip, "conexao aberta (fd=%d)", fd);



    char buf[TAM_REQ_MAX + 1];

    size_t len = 0;

    int atendidas = 0;



    while (atendidas < MAX_REQ_CONEXAO) {

        ssize_t tam_cab = ler_cabecalho(fd, buf, &len);

        if (tam_cab == 0) break;                         /* cliente fechou / timeout */

        if (tam_cab < 0) { enviar_erro(fd, 400, 0, 0); break; }



        /* copia o cabecalho para parsear sem destruir o restante do buffer */

        char cab[TAM_REQ_MAX + 1];

        memcpy(cab, buf, (size_t)tam_cab);

        cab[tam_cab] = '\0';



        /* remove a requisicao consumida do buffer (mantem pipelining) */

        memmove(buf, buf + tam_cab, len - (size_t)tam_cab);

        len -= (size_t)tam_cab;

        atendidas++;



        requisicao_t rq;

        if (parsear_requisicao(cab, &rq) < 0) {

            log_msg(ip, "requisicao invalida -> 400");

            enviar_erro(fd, 400, 0, 0);

            break;

        }

        if (strcmp(rq.versao, "HTTP/1.1") != 0 && strcmp(rq.versao, "HTTP/1.0") != 0) {

            enviar_erro(fd, 505, 0, 0);

            break;

        }

        if (strcmp(rq.metodo, "GET") != 0 && strcmp(rq.metodo, "HEAD") != 0) {

            log_msg(ip, "%s %s -> 405", rq.metodo, rq.caminho);

            enviar_erro(fd, 405, 0, 0);   /* corpo nao lido: fecha a conexao */

            break;

        }

        if (atendidas >= MAX_REQ_CONEXAO) rq.keep_alive = 0;



        registrar_requisicao_qos(ip, rq.caminho);
        mostrar_estado_qos(ip, rq.caminho);
        if (enviar_arquivo(fd, ip, &rq) < 0) break;

        if (!rq.keep_alive) break;

    }



    close(fd);
    liberar_cliente(ip);

    log_msg(ip, "conexao fechada (fd=%d, %d requisicoes)", fd, atendidas);

    return NULL;

}



/* ------------------------------------------------------------------ */

/* main                                                                */

/* ------------------------------------------------------------------ */



int main(int argc, char *argv[])

{

    int porta = (argc > 1) ? atoi(argv[1]) : PORTA_PADRAO;

    if (argc > 2) raiz = argv[2];
    const char *arquivo_taxas = (argc > 3) ? argv[3] : "taxas.txt";
    if (argc > 4) vazao_max_kbps = atof(argv[4]);
    if (vazao_max_kbps <= 0) { fprintf(stderr, "Vazao maxima deve ser > 0 kbps\n"); return 1; }
    carregar_taxas(arquivo_taxas);

    if (porta <= 0 || porta > 65535) {

        fprintf(stderr, "Uso: %s [porta] [diretorio_raiz] [arquivo_taxas] [vazao_max_kbps]\n", argv[0]);

        return 1;

    }



    signal(SIGPIPE, SIG_IGN);



    int servidor_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (servidor_fd < 0) { perror("Erro ao criar socket"); return 1; }



    int opt = 1;

    setsockopt(servidor_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));



    struct sockaddr_in endereco;

    memset(&endereco, 0, sizeof(endereco));

    endereco.sin_family = AF_INET;

    endereco.sin_addr.s_addr = INADDR_ANY;

    endereco.sin_port = htons((uint16_t)porta);



    if (bind(servidor_fd, (struct sockaddr *)&endereco, sizeof(endereco)) < 0) {

        perror("Erro no bind"); close(servidor_fd); return 1;

    }

    if (listen(servidor_fd, BACKLOG) < 0) {

        perror("Erro no listen"); close(servidor_fd); return 1;

    }



    printf("Servidor HTTP/1.1 na porta %d, raiz '%s', taxas='%s', vazao maxima=%.0f kbps\n", porta, raiz, arquivo_taxas, vazao_max_kbps);



    for (;;) {

        cliente_t *cli = malloc(sizeof(*cli));

        if (!cli) { perror("malloc"); continue; }

        socklen_t tam = sizeof(cli->addr);



        cli->fd = accept(servidor_fd, (struct sockaddr *)&cli->addr, &tam);

        if (cli->fd < 0) {

            if (errno != EINTR) perror("Erro no accept");

            free(cli);

            continue;

        }



        char ip_novo[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli->addr.sin_addr, ip_novo, sizeof(ip_novo));
        if (!admitir_cliente(ip_novo)) {
            const char *resp = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\nRetry-After: 2\r\n\r\n";
            (void)enviar_tudo(cli->fd, resp, strlen(resp));
            log_msg(ip_novo, "controle de admissao: recusado (limite %.0f kbps)", vazao_max_kbps);
            close(cli->fd); free(cli); continue;
        }

        pthread_t tid;
        if (pthread_create(&tid, NULL, atender_cliente, cli) != 0) {

            perror("Erro no pthread_create");
            liberar_cliente(ip_novo);
            close(cli->fd);

            free(cli);

            continue;

        }

        pthread_detach(tid);

    }



    close(servidor_fd);

    return 0;

}