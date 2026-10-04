#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORTA 8080

int main(void)
{
    int servidor_fd;
    int cliente_fd;

    struct sockaddr_in endereco;

    char buffer[1024];

    /*
     * 1. Criar o socket TCP
     */
    servidor_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (servidor_fd < 0)
    {
        perror("Erro ao criar socket");
        return 1;
    }

    printf("Socket criado.\n");

    /*
     * 2. Configurar o endereco do servidor
     */
    memset(&endereco, 0, sizeof(endereco));

    endereco.sin_family = AF_INET;
    endereco.sin_addr.s_addr = INADDR_ANY;
    endereco.sin_port = htons(PORTA);

    /*
     * 3. Associar socket ao IP/porta
     */
    if (bind(servidor_fd,
             (struct sockaddr *)&endereco,
             sizeof(endereco)) < 0)
    {
        perror("Erro no bind");
        close(servidor_fd);
        return 1;
    }

    printf("Bind realizado na porta %d.\n", PORTA);

    /*
     * 4. Colocar socket em modo de escuta
     */
    if (listen(servidor_fd, 10) < 0)
    {
        perror("Erro no listen");
        close(servidor_fd);
        return 1;
    }

    printf("Servidor aguardando conexoes...\n");

    /*
     * 5. Aceitar um cliente
     */
    cliente_fd = accept(servidor_fd, NULL, NULL);

    if (cliente_fd < 0)
    {
        perror("Erro no accept");
        close(servidor_fd);
        return 1;
    }

    printf("Cliente conectado!\n");

    /*
     * 6. Receber dados do cliente
     */
    ssize_t bytes_recebidos;

    bytes_recebidos = recv(
        cliente_fd,
        buffer,
        sizeof(buffer) - 1,
        0
    );

    if (bytes_recebidos < 0)
    {
        perror("Erro no recv");
        close(cliente_fd);
        close(servidor_fd);
        return 1;
    }

    buffer[bytes_recebidos] = '\0';

    printf("Cliente enviou: %s\n", buffer);

    /*
     * 7. Enviar resposta
     */
    const char *resposta = "Mensagem recebida pelo servidor!\n";

    send(
        cliente_fd,
        resposta,
        strlen(resposta),
        0
    );

    /*
     * 8. Fechar conexoes
     */
    close(cliente_fd);
    close(servidor_fd);

    printf("Servidor encerrado.\n");

    return 0;
}