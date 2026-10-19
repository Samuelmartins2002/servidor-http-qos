# Servidor HTTP com QoS — compilação e plano de testes

## O que foi acrescentado

- Arquivo de taxas por IPv4 (`IP taxa_kbps`), com padrão de 1000 kbps para IPs não cadastrados.
- Limitador compartilhado por IP: conexões simultâneas originadas do mesmo IP dividem a mesma taxa configurada.
- Controle de admissão por vazão reservada: uma nova origem é recusada com HTTP 503 quando sua taxa ultrapassaria a capacidade configurada. A taxa é reservada uma vez por IP, não uma vez por conexão.
- Saída de status em tempo real no terminal: IP, taxa configurada, conexões, vazão reservada, caminho e estimativa do intervalo entre a requisição do HTML e a primeira requisição de objeto.

> A estimativa chamada `RTT_estimado` é o intervalo observado no servidor entre receber a requisição do HTML e receber a próxima requisição de objeto. Não é uma medição pura do RTT TCP: inclui o tempo de processamento do cliente/browser. `banda_configurada` informa o limite configurado, não uma medição independente da banda real.

## 1. Compilar (Linux)

```bash
gcc -Wall -Wextra -O2 -pthread servidor_v1_limpo.c -o servidor_qos
```

## 2. Criar diretório de teste e arquivo grande

```bash
mkdir -p www
cat > www/index.html <<'HTML'
<!doctype html>
<html><body>
<h1>Teste QoS</h1>
<img src="/objeto.bin" alt="objeto de teste">
</body></html>
HTML
# 2 MiB para facilitar a medição
 dd if=/dev/zero of=www/objeto.bin bs=1M count=2 status=progress
```

## 3. Configurar taxas

Crie `taxas.txt` no diretório em que executará o servidor. Uma linha por IP, separada por espaço ou tabulação:

```text
# IP taxa_maxima_kbps
127.0.0.1 300
192.168.1.10 500
192.168.1.11 1500
```

O IP precisa ser o endereço de origem que o servidor realmente enxerga. Para testes locais, normalmente será `127.0.0.1`. IPs não listados usam 1000 kbps.

## 4. Executar

Formato:

```bash
./servidor_qos [porta] [diretorio_raiz] [arquivo_taxas] [vazao_max_kbps]
```

Exemplo:

```bash
./servidor_qos 8080 ./www ./taxas.txt 2000
```

O último argumento é a capacidade máxima do servidor em kbps. Mantenha o terminal aberto para observar as mensagens QoS.

## 5. Testes funcionais

Em outro terminal:

### Teste A — HTML e objeto

```bash
curl -v http://127.0.0.1:8080/
curl -o /dev/null -sS -w 'HTTP=%{http_code} bytes=%{size_download} tempo=%{time_total}s\n' http://127.0.0.1:8080/objeto.bin
```

Esperado: HTTP 200; o terminal do servidor mostra IP, taxa e requisição. Após acessar primeiro `/` e depois um objeto, a saída poderá mostrar `RTT_estimado`.

### Teste B — comparar taxa de 300 kbps com 1000 kbps

1. Com `127.0.0.1 300`, transfira o objeto e anote `tempo_total`.
2. Altere a linha para `127.0.0.1 1000` e reinicie o servidor.
3. Repita a transferência com o mesmo arquivo.

Esperado: a transferência limitada a 300 kbps demora aproximadamente 3,33 vezes o tempo da de 1000 kbps, desconsiderando overhead e variações. A taxa está em **kilobits por segundo**, não kilobytes por segundo.

### Teste C — conexões simultâneas do mesmo IP compartilham a taxa

Com a taxa de `127.0.0.1` configurada em 300 kbps, rode:

```bash
curl -sS -o /dev/null -w 'A %{time_total}s\n' http://127.0.0.1:8080/objeto.bin &
curl -sS -o /dev/null -w 'B %{time_total}s\n' http://127.0.0.1:8080/objeto.bin &
wait
```

Compare com o tempo de uma transferência isolada. Com compartilhamento correto, duas transferências concorrentes devem demorar mais individualmente; a soma da vazão delas não deve simplesmente dobrar para cerca de 600 kbps.

### Teste D — controle de admissão

1. Configure `127.0.0.1 300` em `taxas.txt`.
2. Inicie o servidor com capacidade menor que 300 kbps:

```bash
./servidor_qos 8080 ./www ./taxas.txt 200
```

3. Em outro terminal:

```bash
curl -i http://127.0.0.1:8080/
```

Esperado: `HTTP/1.1 503 Service Unavailable` e uma mensagem de recusa no terminal do servidor. Com capacidade de 500 kbps, a origem de 300 kbps deve ser admitida.

### Teste E — IP não cadastrado

Remova/comente a linha de `127.0.0.1` e inicie com capacidade de pelo menos 1000 kbps. Faça uma requisição.

Esperado: o servidor usa o padrão de 1000 kbps. Se a capacidade máxima for inferior a 1000 kbps, a nova origem será recusada pelo controle de admissão.

### Teste F — HTTP e captura de pacotes

Em outro terminal, capture o tráfego antes de executar `curl`:

```bash
sudo tcpdump -i lo -nn -w qos-teste.pcap tcp port 8080
```

Pare com `Ctrl+C` e abra `qos-teste.pcap` no Wireshark. Para interface cabeada/Wi-Fi, substitua `lo` pela interface correta (por exemplo `eth0` ou `wlan0`). O HTTP sem TLS permite observar requisições e respostas. Use também `iptraf-ng` se estiver instalado.

## 6. Como registrar os resultados

Para cada cenário, registre: taxa configurada, capacidade máxima, número de clientes/conexões, tamanho do objeto, código HTTP, tempo de transferência, taxa observada aproximada (`tamanho_bytes * 8 / tempo / 1000`) e observações do terminal/Wireshark. Repita cada teste pelo menos três vezes e compare médias.

## Limitações que devem ser validadas antes da entrega

- O painel é uma saída textual atualizada por requisição, não uma interface gráfica dedicada.
- A estimativa de RTT depende de o cliente requisitar primeiro o HTML e depois um objeto, e de o servidor observar ambas as requisições do mesmo IP. Navegadores podem não requisitar imagens em todos os cenários; `curl` é útil para controlar a sequência.
- A admissão reserva a taxa máxima configurada por IP ativo. Isso é conservador e não representa a vazão instantânea medida na interface física.
- Em ambientes com NAT, clientes diferentes podem aparecer com o mesmo IP e compartilhar a mesma taxa, conforme o requisito baseado no endereço IP.

O código teve auxílio de IA;
