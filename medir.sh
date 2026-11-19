#!/usr/bin/env bash
# Mede o tempo de execucao do contador com 1 e 8 threads leitoras.
#
# Uso:   ./medir.sh <diretorio> <palavra1> [palavra2 ...]
# Ex.:   ./medir.sh /home/derick/Downloads/txt Felicitous
#        REPS=20 ./medir.sh /home/derick/Downloads/txt Against
#
# Requer contador.c na pasta atual (ou FONTE=caminho/contador.c) com o define
#     #ifndef NTHREADS
#     #define NTHREADS 8
#     #endif
# para que o valor possa ser trocado com -DNTHREADS=<n> na compilacao.

set -e

REPS=${REPS:-10}
FONTE=${FONTE:-contador.c}

if [ $# -lt 2 ]; then
    echo "Uso: $0 <diretorio> <palavra1> [palavra2 ...]" >&2
    exit 1
fi

if ! grep -q '#ifndef NTHREADS' "$FONTE"; then
    echo "Erro: $FONTE precisa de '#ifndef NTHREADS' em volta do '#define NTHREADS 8'." >&2
    exit 1
fi

echo "CPU: $(lscpu 2>/dev/null | sed -n 's/^Model name: *//p' | head -1) | nucleos logicos: $(nproc)"
echo "Repeticoes por configuracao: $REPS (apos 1 execucao de aquecimento)"
echo "Argumentos: $*"

for n in 1 4 8 16; do
    bin="/tmp/contador_$n"
    gcc -O2 -pthread -DNTHREADS=$n "$FONTE" -o "$bin"

    # Aquecimento: deixa os arquivos no cache de disco
    "$bin" "$@" > /dev/null

    tempos=()
    for ((i = 0; i < REPS; i++)); do
        ini=$(date +%s%N)
        "$bin" "$@" > /dev/null   # saida descartada para nao medir o terminal
        fim=$(date +%s%N)
        tempos+=($(( (fim - ini) / 1000000 )))   # milissegundos
    done

    printf '%s\n' "${tempos[@]}" | sort -n | awk -v n="$n" '
        { a[NR] = $1; soma += $1 }
        END {
            media = soma / NR
            for (i = 1; i <= NR; i++) var += (a[i] - media) ^ 2
            printf "NTHREADS=%d: media %.1f ms | mediana %d ms | min %d ms | desvio %.1f ms\n",
                   n, media, a[int((NR + 1) / 2)], a[1], sqrt(var / NR)
        }'
done
