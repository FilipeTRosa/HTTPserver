#!/bin/bash

# Configurações 
NUM_REQUESTS=10 # Quantas requisições concorrentes você quer fazer
URL="http://localhost:5000"
# Fim config

echo "Iniciando $NUM_REQUESTS requisições concorrentes para $URL..."

# Cria uma pasta para salvar os arquivos, se não existir
mkdir -p downloads
rm -f downloads/* # Limpa downloads anteriores

# Loop para lançar todos os comandos curl em segundo plano
for i in $(seq 1 $NUM_REQUESTS)
do
  echo "  -> Lançando requisição #$i"
  # O '&' executa o comando em segundo plano
  # o script continua para o próximo passo imediatamente.
  curl -s -o "downloads/foto_$i.jpg" $URL &
done

echo
echo "Aguardando todas as requisições em segundo plano terminarem..."
#pausa o script até que todos os processos em segundo plano terminem
wait

echo
echo "Concluído! Verifique os arquivos na pasta 'downloads'."
ls -l downloads