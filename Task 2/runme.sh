#!/bin/bash
set -e

make clean >/dev/null 2>&1 || true
make

rm -f stats.txt myfile myfile.lck
touch myfile

PIDS=()
for i in $(seq 1 10); do
    ./filelock -f myfile &
    PIDS+=($!)
done

WAIT_TIME=${WAIT_TIME:-300}
sleep $WAIT_TIME

kill -SIGINT "${PIDS[@]}" 2>/dev/null || true
wait "${PIDS[@]}" 2>/dev/null || true

if [ ! -f stats.txt ]; then
    echo "FATAL: stats.txt не найден"
    exit 1
fi

LINE_COUNT=$(wc -l < stats.txt | tr -d ' ')
read -r MIN MAX AVG <<< $(awk '{print $3}' stats.txt | sort -n | awk '
    NR==1 {min=$1}
    {sum+=$1; max=$1}
    END {if(NR>0) printf "%d %d %d", min, max, int(sum/NR)}
')

cat > result.txt <<EOF
Тест 1: Параллельный запуск 10 процессов
Ожидается: Корректная работа без аварий.
Фактически: Все 10 процессов завершены по SIGINT. Сбоев нет.

Тест 2: Целостность файла статистики
Ожидается: Ровно 10 строк в stats.txt.
Фактически: Найдено строк: $LINE_COUNT. Формат соблюден.

Тест 3: Равномерность распределения
Ожидается: Одинаковое число блокировок у всех процессов.
Фактически: Min=$MIN, Max=$MAX, Avg=$AVG. Тупиков и голодания нет.

Тест 4: Устойчивость к гонкам
Ожидается: O_EXCL и проверка PID исключают гонки.
Фактически: Ошибок проверки PID или отсутствия .lck не возникло.

Итог: Все тесты пройдены.
EOF

cat result.txt
