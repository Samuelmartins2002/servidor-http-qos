# Versão 1: servidor HTTP/1.1 concorrente

Disciplina: Fundamentos e Avaliação de Redes de Computadores (UNIPAMPA, 2026/2)

## Integrantes

- [NOME 1]
- [NOME 2]
- [NOME 3]

## Escopo da versão

Servidor HTTP capaz de tratar conexões concorrentes (vários clientes simultaneamente), seguindo o protocolo HTTP/1.1 com conexões persistentes. Escrito em C com Pthreads.

## Compilação e execução

Ambiente: Linux (Ubuntu sobre WSL2), `gcc` e `make`.

```bash
# compilar (ajustar conforme o Makefile)
make

# executar (ajustar conforme os parâmetros implementados)
./servidor <porta>
```

Teste rápido, em outro terminal:

```bash
curl -v http://127.0.0.1:<porta>/
```

Observações pertinentes sobre compilação e execução:

- [preencher: diretório raiz dos arquivos servidos, dependências, flags, limitações conhecidas]

## Declaração de autoria

Este projeto foi desenvolvido integralmente pela equipe, sem ajuda não autorizada de alunos não membros do projeto no processo de codificação.

<!-- Manter a declaração acima somente se for verdadeira. -->

## Código externo e uso de IA

Trechos de código da Internet ou produzidos com apoio de IA estão identificados em comentários no código-fonte e listados aqui:

| Arquivo / trecho | Origem (site ou ferramenta de IA) | Observação |
|---|---|---|
| (nenhum até o momento) | | |
