/*
 *
 * contador.c
 *
 * Descricao: Conta quantas vezes cada palavra de uma lista aparece nos
 *            arquivos de texto de um diretorio. Varias threads leitoras 
 *            processam os arquivos em paralelo e uma unica thread anunciadora
 *            imprime cada ocorrencia encontrada (palavra, arquivo e linha).
 *            Ao final, mostra a contagem por arquivo, o contador global e a
 *            soma dos contadores por arquivo, que devem coincidir.
 *
 * Autor:     Derick Daniel Silva de Andrade
 *
 * Compilar:  gcc -pthread contador.c -o contador
 * Executar:  ./contador <diretorio> <palavra1_> <palavra_2> ... <palavra_n>
 * Exemplo:   ./contador /home/user/documents/textos Felicitous
 */

/* -------------------------------------------------------------------------- */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <ctype.h>

/* -------------------------------------------------------------------------- */

/*
 * Defines
 */

#ifndef NTHREADS
#define NTHREADS 8 // Numero de threads contadoras
#endif
#define TAMANHO_CAMINHO 1024
#define lock pthread_mutex_lock
#define unlock pthread_mutex_unlock
#define wait pthread_cond_wait
#define signal pthread_cond_signal
#define broadcast pthread_cond_broadcast

/* -------------------------------------------------------------------------- */

/*
 * Tipos personalizados
 */

// Caminho e numero de ocorrencias de cada palavra no arquivo
typedef struct {
    char *caminho;
    int *contador; 
} Arquivo;

// Thread anunciadora
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond_evento; // A anunciadora espera eventos nessa condicao
    pthread_cond_t cond_livre;  // As leitoras esperam o ultimo anuncio terminar
    pthread_cond_t cond_pronto; // As leitoras esperam o seu anuncio terminar

    int tem_evento;
    int palavra;    // indice da palavra do evento
    int ocorrencia; // numero global da ocorrencia
    int linha;      // linha do arquivo onde foi achada
    const char *arquivo;
    unsigned long ticket;     // id do ultimo evento postado
    unsigned long feito;      // id do ultimo evento anunciado
    int fim;        // flag, se == 1 entao nao tera mais eventos

    int *total;     // contadores globais por palavra
} Anunciadora;

/* -------------------------------------------------------------------------- */

/* 
 * Variaveis globais
 */

static int qnt_palavras = 0;
static char **palavras;

static int qnt_arquivos = 0;
static Arquivo *arquivos;

static int proximo_arquivo = 0; 
static pthread_mutex_t mutex_fila = PTHREAD_MUTEX_INITIALIZER;

static Anunciadora anunciadora;

/* -------------------------------------------------------------------------- */

/*
 * Prototipos
 */

// Anunciadora
static void anunciadora_init(Anunciadora *a, int n);
static void anunciadora_destruir(Anunciadora *a);
static void anunciadora_finalizar(Anunciadora *a);
static void registrar_ocorrencia(int palavra, const char *arquivo, int linha);
static void *anunciar(void *arg);

// Leitura de arquivos
static char *normalizar_palavra(char *palavra);
static int buscar_palavra(const char *token);
static const char *nome_base(const char *caminho);
static void contar_arquivo(Arquivo *arq);

// Grupos de threads leitoras
static Arquivo *pegar_proximo_arquivo(void);
static void *thread_leitora(void *arg);

// Diretorio
static int listar_arquivos(const char *dir);
static void liberar_arquivos(void);

// Resultados
static void imprimir_resultados(void);

/* -------------------------------------------------------------------------- */

int main(int argc, char *argv[]) {

    // Validacao de argumentos
    if (argc < 3) {
        fprintf(stderr, "Numero insuficiente de argumentos, passe o caminho"
            " dos arquivos e pelo menos uma palavra\n");
        return 1;
    }

    // Guarda as palavras a procurar e inicializa a thread anunciadora
    qnt_palavras = argc - 2;
    palavras = argv + 2;
    anunciadora_init(&anunciadora, qnt_palavras);

    // Monta a lista de arquivos no diretorio
    if (listar_arquivos(argv[1]) != 0) {
        liberar_arquivos();
        anunciadora_destruir(&anunciadora);
        return 1;
    }

    if (qnt_arquivos == 0) {
        fprintf(stderr, "Nenhum documento de texto encontrado\n");
        liberar_arquivos();
        anunciadora_destruir(&anunciadora);
        return 1;
    }

    // Ativa a thread anunciadora antes das leitoras, assim quando uma leitora
    // postar um evento, a anunciadora ja esta esperando.
    pthread_t thread_anunciadora;
    if (pthread_create(&thread_anunciadora, NULL, anunciar, NULL) != 0) {
        fprintf(stderr, "Erro ao criar a thread anunciadora\n");
        liberar_arquivos();
        anunciadora_destruir(&anunciadora);
        return 1;
    }

    // Cria threads leitoras
    // No maximo NTHREADS ou qnt_arquivos, o que for menor
    int num_threads = qnt_arquivos < NTHREADS ? qnt_arquivos : NTHREADS;
    pthread_t leitoras[NTHREADS];
    int criadas = 0;

    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&leitoras[criadas], NULL, thread_leitora,
            NULL) == 0) {
            criadas++;
        } else {
            fprintf(stderr, "Erro ao criar a thread leitora %d\n", i);
        }
    }

    // Se nenhuma thread leitora foi criada, a main vai fazer o trabalho
    if (criadas == 0) {
        thread_leitora(NULL);
    }

    // Espera todas as threads leitoras terminarem de ler
    for (int i = 0; i < criadas; i++) {
        pthread_join(leitoras[i], NULL);
    }

    // Nao tem mais eventos, entao a anunciadora pode encerrar depois de 
    // anunciar todos os eventos pendentes
    anunciadora_finalizar(&anunciadora);
    pthread_join(thread_anunciadora, NULL);

    imprimir_resultados();

    liberar_arquivos();
    anunciadora_destruir(&anunciadora);

    return 0;
}

/* -------------------------------------------------------------------------- */

/* Anunciadora */

static void anunciadora_init(Anunciadora *a, int n) {
    pthread_mutex_init(&a->mutex, NULL);
    pthread_cond_init(&a->cond_evento, NULL);
    pthread_cond_init(&a->cond_livre, NULL);
    pthread_cond_init(&a->cond_pronto, NULL);

    a->tem_evento = a->ticket = a->feito = a->fim = 0;
    
    a->total = calloc(n, sizeof(int));
    if (a->total == NULL) {
        perror("calloc");
        exit(1);
    }
}

static void anunciadora_destruir(Anunciadora *a) {
    pthread_mutex_destroy(&a->mutex);
    pthread_cond_destroy(&a->cond_evento);
    pthread_cond_destroy(&a->cond_livre);
    pthread_cond_destroy(&a->cond_pronto);
    free(a->total);
}

/*
 * Funcao chamada pelas leitoras, vai atualizar o total global, postar o evento
 * para a anunciadora anunciar e vai dormir ate ela terminar de anunciar.
 */
static void registrar_ocorrencia(int palavra, const char *arquivo, int linha) {
    Anunciadora *a = &anunciadora;
    lock(&a->mutex);

    while (a->tem_evento) {
        wait(&a->cond_livre, &a->mutex);
    }

    a->total[palavra]++;
    a->palavra = palavra;
    a->ocorrencia = a->total[palavra];
    a->arquivo = arquivo;
    a->linha = linha;
    a->tem_evento = 1;
    unsigned long meu_ticket = ++a->ticket;
    signal(&a->cond_evento);

    while (a->feito < meu_ticket) {
        wait(&a->cond_pronto, &a->mutex);
    }
    unlock(&a->mutex);
}

static void anunciadora_finalizar(Anunciadora *a) {
    lock(&a->mutex);
    a->fim = 1;
    signal(&a->cond_evento);
    unlock(&a->mutex);
}

/*
 * Dorme ate uma leitora postar um evento. Quando acorda, copia os dados do
 * evento, imprime o anuncio e libera a leitora que estava esperando. Encerra
 * quando `fim` for sinalizado e nao houver nenhum evento pendente.
 * Nao tem parametros porque ela acessa as informacoes inseridas na variavel
 * global `anunciadora`.
 */
static void *anunciar(void *arg) {
    (void) arg;
    Anunciadora *a = &anunciadora;

    lock(&a->mutex);
    while(1) {
        while (!a->tem_evento && !a->fim) {
            wait(&a->cond_evento, &a->mutex);
        }
        if (!a->tem_evento) {
            break;
        }

        int palavra = a->palavra;
        int ocorrencia = a->ocorrencia;
        const char *arquivo = a->arquivo;
        int linha = a->linha;

        unlock(&a->mutex);

        // Printa fora do lock para nao travar quem so quer consultar o estado
        printf("Palavra \"%s\" encontrada no arquivo \"%s\", linha %d"
        " (ocorrencia global numero %d)\n",
        palavras[palavra],
        arquivo,
        linha,
        ocorrencia);

        lock(&a->mutex);

        a->tem_evento = 0;
        a->feito = a->ticket;

        // Broadcast para garantir que a leitora que esta esperando seu anuncio
        // vai ser acordada, pois elas verificam se feito >= meu_ticket, entao
        // somente a thread que foi anunciada vai continuar, as outras dormem.
        broadcast(&a->cond_pronto);
        signal(&a->cond_livre);
    }
    unlock(&a->mutex);
    return NULL;
}

/* -------------------------------------------------------------------------- */

/* Leitura de arquivos */

// Primeiro tira as pontuacoes no fim da palavra substituindo por '\0'.
// Depois tira as que estao no comeco da palavra mudando onde a palavra comeca.
// Retorna o novo ponteiro de onde a palavra comeca
static char *normalizar_palavra(char *palavra) {
    int i = (int) strlen(palavra) - 1;
    while (i >= 0 && ispunct((unsigned char) palavra[i])) {
        palavra[i--] = '\0';
    }
    char *comeco = palavra;
    while (ispunct((unsigned char) *comeco)) {
        comeco++;
    }

    return comeco;
}

// Retorna o indice da palavra buscada igual ao token ou -1
static int buscar_palavra(const char *token) {
    for (int i = 0; i < qnt_palavras; i++) {
        if (strcasecmp(palavras[i], token) == 0) {
            return i;
        }
    }

    return -1;
}

// Retorna o nome do arquivo, sem o resto do caminho
static const char *nome_base(const char *caminho) {
    const char *barra = strrchr(caminho, '/');
    return barra ? barra + 1 : caminho;
}

static void contar_arquivo(Arquivo *arq) {
    FILE *f = fopen(arq->caminho, "r");
    if (f == NULL) {
        perror("fopen");
        return;
    }

    char *linha_txt = NULL;
    size_t cap = 0; // tamanho do buffer
    int num_linha = 0;

    while(getline(&linha_txt, &cap, f) != -1) {
        num_linha++;

        char *salvo;
        char *token = strtok_r(linha_txt, " \t\r\n", &salvo);
        while (token != NULL) {
            char *limpa = normalizar_palavra(token);
            int i = buscar_palavra(limpa);
            if (i >= 0) {
                arq->contador[i]++;
                registrar_ocorrencia(i, nome_base(arq->caminho), num_linha);
            }
            token = strtok_r(NULL, " \t\r\n", &salvo);
        }
    }
    free(linha_txt);
    fclose(f);
}

/* -------------------------------------------------------------------------- */

/* Grupo de Threads Leitoras */

static Arquivo *pegar_proximo_arquivo(void) {
    Arquivo *arq = NULL;
    lock(&mutex_fila);
    if (proximo_arquivo < qnt_arquivos) {
        arq = &arquivos[proximo_arquivo++];
    }
    unlock(&mutex_fila);
    return arq;
}

static void *thread_leitora(void *arg) {
    (void) arg;
    Arquivo *arq;
    while ((arq = pegar_proximo_arquivo()) != NULL) {
        contar_arquivo(arq);
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */

/* Diretorio */


// Preenche `arquivos` e `qnt_arquivos` com os arquivos regulares do diretorio
static int listar_arquivos(const char *dir) {
    DIR *diretorio  = opendir(dir);
    if (diretorio == NULL) {
        perror("opendir");
        return -1;
    }

    int capacidade = 2;
    arquivos = malloc(capacidade * sizeof(Arquivo));
    if (arquivos == NULL) {
        perror("malloc");
        closedir(diretorio);
        return -1;
    }

    struct dirent *entrada;
    struct stat st;
    char caminho_buffer[TAMANHO_CAMINHO];

    while((entrada = readdir(diretorio)) != NULL) {
        // Ignora o diretorio atual e diretorio pai
        if (strcmp(entrada->d_name, ".") == 0 ||
        strcmp(entrada->d_name, "..") == 0) {
            continue;
        }
        // Monta o nome `caminho + arquivo`
        snprintf(caminho_buffer, sizeof(caminho_buffer), "%s/%s",
                 dir, entrada->d_name);
        if (stat(caminho_buffer, &st) != 0) {
            perror("stat");
            continue;
        }

        // Confere se trata-se de um arquivo regular (de texto se presume)
        if (!S_ISREG(st.st_mode)) {
            continue;
        }

        // Dobra o tamanho do buffer de arquivos se ele chegou ao maximo
        if (qnt_arquivos == capacidade) {
            capacidade *= 2;
            Arquivo *tmp = realloc(arquivos, capacidade * sizeof(Arquivo));
            if (tmp == NULL) {
                perror("realloc");
                closedir(diretorio);
                return -1;
            }
            arquivos = tmp;
        }

        // Reserva os campos de caminho e contadores do arquivo atual
        arquivos[qnt_arquivos].caminho = strdup(caminho_buffer);
        arquivos[qnt_arquivos].contador = calloc(qnt_palavras, sizeof(int));
        if (arquivos[qnt_arquivos].caminho == NULL ||
        arquivos[qnt_arquivos].contador == NULL) {
            perror("alloc");
            closedir(diretorio);
            return -1;
        }
        qnt_arquivos++;
    }
    closedir(diretorio);
    return 0;
}

static void liberar_arquivos(void) {
    for (int i = 0; i < qnt_arquivos; i++) {
        free(arquivos[i].caminho);
        free(arquivos[i].contador);
    }
    free(arquivos);
}

/* -------------------------------------------------------------------------- */

/* Resultados */

// Imprime a contagem de palavras por arquivo, a contagem guardada no contador
// global e a soma dos resultados da contagem de palavras por arquivo
static void imprimir_resultados(void) {
    int *soma_arquivos = calloc(qnt_palavras, sizeof(int));
    if (soma_arquivos == NULL) {
        perror("calloc");
        return;
    }
    for (int i = 0; i < qnt_arquivos; i++) {
        printf("Nome: %s\n", nome_base(arquivos[i].caminho));
        for (int j = 0; j < qnt_palavras; j++) {
            int x = arquivos[i].contador[j];
            soma_arquivos[j] += x;
            printf("%s: %d\n", palavras[j], x);
        }
    }
    printf("Contador Global\n");
    for (int i = 0; i < qnt_palavras; i++) {
        printf("%s: %d\n", palavras[i], anunciadora.total[i]);
    }
    printf("Soma Individual dos Arquivos\n");
    for (int i = 0; i < qnt_palavras; i++) {
        printf("%s: %d\n", palavras[i], soma_arquivos[i]);
    }
    free(soma_arquivos);
}
/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */
