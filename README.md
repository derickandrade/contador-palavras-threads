# Contagem Concorrente de Palavras em Arquivos de Texto com Threads POSIX

Universidade de Brasília · Departamento de Ciência da Computação

**Autor:** Dérick Daniel Silva de Andrade

## Resumo

Este repositório apresenta um programa em C, baseado em *threads* POSIX (Pthreads), que conta as ocorrências de um conjunto de palavras em cada arquivo de um diretório. Um grupo de até `NTHREADS` *threads* leitoras processa os arquivos em paralelo, retirando-os de uma fila compartilhada, e uma única *thread* anunciadora exibe cada ocorrência encontrada (palavra, arquivo, linha e número global). A exclusão mútua e a comunicação entre *threads* são implementadas com *mutex* e variáveis de condição. A avaliação experimental, realizada com 28 livros de domínio público (aproximadamente 28 MB), mostrou contagens globais idênticas à soma por arquivo e *speedup* de até 4,16 com 8 leitoras para uma palavra rara. Para uma palavra frequente, o *speedup* ficou entre 1,06 e 1,11, o que se deve à serialização das entregas síncronas na anunciadora.

## Sumário

1. [Formalização do problema](#1-formalização-do-problema)
2. [Algoritmo desenvolvido](#2-algoritmo-desenvolvido)
3. [Compilação e execução](#3-compilação-e-execução)
4. [Avaliação experimental](#4-avaliação-experimental)
5. [Conclusão e trabalhos futuros](#5-conclusão-e-trabalhos-futuros)
6. [Materiais complementares](#6-materiais-complementares)
7. [Referências](#referências)

## 1. Formalização do problema

### 1.1 Entradas, saídas e definição de ocorrência

O programa recebe um diretório $D$ e um conjunto de palavras $P = \lbrace p_1, \dots, p_k \rbrace$, com $k \geq 1$. Seja $A = \lbrace a_1, \dots, a_n \rbrace$ o conjunto de arquivos regulares de $D$, presumidos como texto UTF-8. Subdiretórios e outros tipos de entrada são ignorados.

Cada arquivo é dividido em *tokens*, isto é, sequências de caracteres não brancos. Removida a pontuação do início e do fim, um *token* é uma ocorrência de $p_i$ se for igual a $p_i$ sem diferenciar maiúsculas de minúsculas. Assim, "Gato" e "(GATO)" são ocorrências de "gato", mas "gatos" não é. A remoção de pontuação e a comparação de caixa valem apenas para caracteres ASCII.

As saídas são:

- o número $c(i, j)$ de ocorrências de $p_i$ em $a_j$;
- o total global $C(i) = \sum_j c(i, j)$;
- um anúncio para cada ocorrência, com a palavra, o arquivo, a linha e o número global da ocorrência.

### 1.2 Requisitos e problemas de concorrência

Os requisitos são:

1. **Paralelismo:** arquivos distintos são processados simultaneamente.
2. **Exclusão mútua:** $C(i)$ coincide com o valor de uma execução sequencial e com a soma dos $c(i, j)$, para qualquer escalonamento.
3. **Anúncio completo:** toda ocorrência é anunciada exatamente uma vez, com numeração consecutiva por palavra.
4. **Terminação:** ausência de *deadlocks* e liberação de todos os recursos.

Os problemas a resolver são:

- a condição de corrida no incremento de `total[p]`;
- a comunicação de várias leitoras com uma única anunciadora, sem que um anúncio sobrescreva outro e sem que uma notificação se perca (*lost wakeup*);
- a distribuição dos arquivos sem repetição;
- o término coordenado, já que a anunciadora só encerra depois de anunciar tudo.

## 2. Algoritmo desenvolvido

### 2.1 Visão geral e estruturas de dados

O programa possui a *thread* principal, até $T$ leitoras ($T$ é a constante `NTHREADS`, 8 por padrão, ajustável na compilação) e uma anunciadora. A *thread* principal valida os argumentos, lista os arquivos regulares do diretório, cria a anunciadora e as $\min(T, n)$ leitoras, espera as leitoras com `pthread_join`, avisa à anunciadora que não haverá mais eventos, espera o seu término e imprime os resultados.

Cada arquivo é representado por uma estrutura `Arquivo`, com o caminho e um vetor `contador` (uma posição por palavra). Esse vetor é privado da leitora que processa o arquivo e, por isso, dispensa sincronização.

A estrutura global `Anunciadora` guarda o estado compartilhado:

- o contador global `total[]`;
- uma "caixa de correio" de uma posição, com os dados do evento e o indicador `tem_evento`;
- as senhas `ticket` e `feito`;
- o indicador `fim`;
- um *mutex* com três variáveis de condição (`cond_evento`, `cond_livre` e `cond_pronto`).

### 2.2 *Threads* leitoras

Cada leitora obtém o próximo arquivo da fila (o vetor de arquivos percorrido por um índice protegido pelo *mutex* `fila`) e o processa até a fila acabar. Assim, quem termina um arquivo pega outro, o que equilibra a carga e limita o número de *threads* a $T$. O arquivo é lido linha a linha (`getline`) e dividido em *tokens* (`strtok_r`). A cada *token* que corresponde a uma palavra procurada, a leitora incrementa o contador do arquivo, sem *lock*, e chama `registrar_ocorrencia`.

### 2.3 *Thread* anunciadora e protocolo de comunicação

A comunicação é uma entrega síncrona sobre a caixa de correio: a leitora atualiza o contador global e publica o evento na mesma região crítica, e só continua depois que o seu anúncio foi impresso. A anunciadora imprime fora da região crítica.

```text
registrar_ocorrencia(p, arquivo, linha):      // leitora
    lock(mutex)
    enquanto tem_evento: wait(cond_livre)
    total[p]++; evento = (p, total[p], arquivo, linha)
    tem_evento = 1; meu = ++ticket; signal(cond_evento)
    enquanto feito < meu: wait(cond_pronto)
    unlock(mutex)

anunciar():                                   // anunciadora
    lock(mutex)
    repetir:
        enquanto !tem_evento e !fim: wait(cond_evento)
        se !tem_evento: sair
        copiar evento; unlock(mutex); imprimir; lock(mutex)
        tem_evento = 0; feito = ticket
        broadcast(cond_pronto); signal(cond_livre)
    unlock(mutex)
```

### 2.4 Argumentos de corretude

- **Exclusão mútua.** O incremento de `total[p]` e a publicação do evento ocorrem sob o mesmo *mutex*, e o contador de cada arquivo é privado e só é lido pela *thread* principal depois do `join`. Como há no máximo um evento em andamento, os números anunciados de cada palavra são consecutivos.
- **Espera com predicado.** Todo `wait` está em um `while` que testa novamente a condição, sobre estado protegido pelo *mutex*. Assim, um sinal emitido antes de alguém dormir não se perde, e despertares espúrios, permitidos pela especificação POSIX [4, 1], são inofensivos. Mesmo com `broadcast`, cada leitora só prossegue quando `feito` alcança a sua senha.
- **Término e ausência de *deadlock*.** Cada leitora só retorna depois que o seu anúncio foi impresso. Logo, ao fim dos `join`, não há evento pendente, e basta marcar `fim` e sinalizar a anunciadora. Nenhuma *thread* segura dois *mutexes* ao mesmo tempo, o que impede espera circular.

## 3. Compilação e execução

**Requisitos:** compilador GCC e suporte a Pthreads (Linux).

**Compilação** (o número de leitoras é definido em tempo de compilação por `NTHREADS`; o padrão é 8):

```bash
gcc -O2 -pthread -DNTHREADS=8 -o contador <arquivo-fonte>.c
```

**Execução:**

```bash
./contador <diretório> <palavra1> [<palavra2> ...]
```

**Exemplo:**

```bash
./contador ./textos Felicitous Froward Aghast
```

A saída contém, em ordem: os anúncios de cada ocorrência (palavra, arquivo, linha e número global), as contagens por arquivo, o contador global e a soma individual dos arquivos, que devem coincidir.

**Medição de desempenho:** o *script* `medir.sh` compila cada configuração com `-DNTHREADS=n` e mede o tempo médio de execução.

```bash
./medir.sh <diretório> <palavra>
```

## 4. Avaliação experimental

### 4.1 Ambiente e dados

Os testes foram realizados em um Intel Core i5-9400F de 2,90 GHz (6 núcleos lógicos), sob Linux Mint 22.3, com GCC 13.3.0 e as opções `-O2 -pthread`. O conjunto de dados contém 28 livros de domínio público em inglês, em texto simples, com aproximadamente 28 MB, obtidos do acervo da Public Domain Library [2].

### 4.2 Corretude

Com as palavras *Felicitous*, *Froward* e *Aghast*, o contador global (3, 2 e 31 ocorrências, respectivamente) coincidiu com a soma por arquivo, e os números anunciados de cada palavra formaram a sequência $1, \dots, C(i)$, sem repetições. Os anúncios de arquivos diferentes aparecem intercalados (as cinco primeiras ocorrências de *Aghast* vieram de quatro livros), o que evidencia processamento simultâneo.

### 4.3 Desempenho

Cada configuração foi compilada com `-DNTHREADS=n` e executada dez vezes após um aquecimento (arquivos no *cache* de disco), com a saída enviada a `/dev/null`. Os cenários foram uma palavra rara (*Felicitous*, 3 ocorrências no total) e uma frequente (*Against*, 299 ocorrências somente em *Les Misérables*). A tabela apresenta o tempo médio $T$ (em ms, com desvio padrão), o *speedup* $S = T_1 / T_n$ e a eficiência $E = S / n$. Com $n = 1$, há uma leitora, além da anunciadora.

| n  | *Felicitous* (rara): T (ms) | S    | E    | *Against* (frequente): T (ms) | S    | E    |
|----|-----------------------------|------|------|-------------------------------|------|------|
| 1  | 180,0 ± 2,8                 | 1,00 | 100% | 1028,6 ± 36,8                 | 1,00 | 100% |
| 4  | 50,9 ± 3,2                  | 3,54 | 88%  | 972,8 ± 31,9                  | 1,06 | 26%  |
| 8  | 43,3 ± 4,3                  | 4,16 | 52%  | 948,6 ± 25,6                  | 1,08 | 14%  |
| 16 | 49,1 ± 6,1                  | 3,67 | 23%  | 926,0 ± 22,9                  | 1,11 | 7%   |

**Palavra rara.** O ganho é expressivo até 4 leitoras ($S = 3{,}54$, eficiência de 88%), e o melhor tempo ocorre com 8 ($S = 4{,}16$). Com 16, o tempo piora, pois há apenas 6 núcleos lógicos e as *threads* extras só aumentam o custo de escalonamento. O valor ficou abaixo de 6, provavelmente pela parte sequencial e pelo desbalanceamento entre arquivos (cada um é lido por uma única leitora).

**Palavra frequente.** O *speedup* ficou entre 1,06 e 1,11, praticamente sem ganho. Como a leitura é a mesma nos dois cenários, os cerca de 850 ms a mais em $T_1$ podem ser atribuídos às ocorrências, cada uma exigindo uma entrega síncrona à anunciadora, que as serializa.

### 4.4 Limitação observada

Em *Les Misérables*, o programa contou 299 ocorrências de *against*, contra 300 em um editor de texto. A diferença vem de uma ocorrência colada a aspas curvas: `ispunct` atua byte a byte e não reconhece os bytes UTF-8 da aspa como pontuação, então o *token* "“Against" não é igual a "against". Em *The Picture of Dorian Gray*, sem pontuação não ASCII junto às ocorrências, a contagem coincidiu. Logo, a igualdade entre o contador global e a soma por arquivo não garante a ausência de erros de tokenização.

## 5. Conclusão e trabalhos futuros

O programa atende aos requisitos: lê os arquivos em paralelo, mantém o contador global correto sob concorrência e anuncia cada ocorrência por uma única anunciadora. O trabalho evidencia a importância de separar o que é privado, como o contador de cada arquivo, do que é compartilhado e exige exclusão mútua e variáveis de condição com predicado.

Os experimentos mostraram que o desempenho depende da frequência da palavra: com uma rara, 8 leitoras deram *speedup* de 4,2 em 6 núcleos lógicos e 16 foram piores; com uma frequente, a entrega síncrona serializou o trabalho na anunciadora e limitou o *speedup* a cerca de 1,1.

Como trabalhos futuros, propõem-se:

- uma fila de capacidade maior (produtor-consumidor com *buffer* limitado) ou o anúncio em lote, para reduzir a serialização;
- a tokenização com caracteres largos (`wchar_t`), para tratar corretamente pontuação não ASCII.


## Referências

[1] BUTENHOF, D. R. *Programming with POSIX Threads*. Reading: Addison-Wesley, 1997.

[2] PUBLIC DOMAIN LIBRARY. *Public Domain Library*. Disponível em: <https://publicdomainlibrary.org/>. Acesso em: 12 nov. 2026.

[3] TANENBAUM, A. S.; BOS, H. *Sistemas Operacionais Modernos*. 4. ed. São Paulo: Pearson, 2016.

[4] THE OPEN GROUP. *The Open Group Base Specifications Issue 7, 2018 edition (IEEE Std 1003.1-2017)*. Disponível em: <https://pubs.opengroup.org/onlinepubs/9699919799/>. Acesso em: 12 nov. 2026.
