# Servidor HTTP com QoS

Trabalho de Programação em Rede da disciplina **Fundamentos e Avaliação de Redes de Computadores** (UNIPAMPA, semestre 2026/2).

Servidor HTTP/1.1 escrito em **C com Pthreads** para Linux, com mecanismos de **Qualidade de Serviço (QoS)**: taxa de envio controlada por cliente, estimativa de atraso e largura de banda em tempo real e controle de admissão.

## Equipe

| Integrante | Papel (Scrum) |
|---|---|
| João Pedro Soll Dias | Scrum Master |
| Simony Meira Franco Cogoy da Silva | Product Owner |
| Samuel da Silva Martins | Developer |

## Versões (MVPs)

| Versão | Pasta | Escopo | Status |
|---|---|---|---|
| V1 | `v1/` | Servidor concorrente (vários clientes simultâneos), HTTP/1.1 com conexões persistentes | Em desenvolvimento |
| V2 | `v2/` | V1 + mecanismos de QoS (ver abaixo) | Planejada |

## Funcionalidades previstas (V2)

- **Taxa controlada por origem:** objetos (exceto HTML) são enviados com taxa máxima definida por IP do cliente, lida de um arquivo auxiliar (IP e taxa em kbps). IP não cadastrado: 1000 kbps, se viável.
- **Painel em tempo real:** clientes atendidos, com estimativa de atraso fim-a-fim e largura de banda, baseada no RTT entre a requisição do HTML e a do primeiro objeto referenciado.
- **Controle de admissão:** o total de clientes simultâneos não excede a vazão máxima do servidor, informada na execução. Conexões simultâneas do mesmo IP dividem a taxa definida para aquele IP.

## Estrutura do repositório

```
.
├── README.md        # este arquivo
├── v1/              # código-fonte da versão 1
│   ├── README.md
│   └── src/
└── v2/              # código-fonte da versão 2 (a criar)
    └── README.md
```

## Requisitos

- Linux (desenvolvido em Ubuntu sobre WSL2)
- `gcc`, `make` e biblioteca Pthreads

```bash
sudo apt install build-essential
```

## Compilação e execução

Cada versão possui seu próprio `README.md` com as instruções de compilação e execução.

## Avaliação experimental

Ferramentas: Wireshark, tcpdump, IPTraf-ng, curl e wget.
Cenários: rede cabeada (Ethernet, IEEE 802.3) e rede sem fio (Wi-Fi, IEEE 802.11).

## Autoria

> Este projeto foi desenvolvido integralmente pela equipe, sem ajuda não autorizada de alunos não membros do projeto no processo de codificação.

<!-- Manter a declaração acima somente se for verdadeira. -->

## Código externo e uso de IA

Trechos de código obtidos na Internet ou produzidos com apoio de IA devem ser referenciados em comentários no código, neste README e no relatório da versão correspondente.

| Arquivo / trecho | Origem (site ou ferramenta de IA) | Observação |
|---|---|---|
| (nenhum até o momento) | | |
