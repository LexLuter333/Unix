#!/bin/bash
RESULT_FILE="result.txt"
LOG_FILE="/tmp/myinit.log"
PID_FILE="/tmp/myinit.pid"
TEST_DIR="/tmp/myinit_test_$$"

rm -f "$RESULT_FILE" "$PID_FILE"
> "$LOG_FILE"
mkdir -p "$TEST_DIR"

make clean >/dev/null 2>&1
make >/dev/null 2>&1

if [ ! -x "./myinit" ]; then
    echo "Ошибка: не удалось собрать myinit" > "$RESULT_FILE"
    exit 1
fi

cat > "$TEST_DIR/config.txt" << EOF
/bin/sleep 1000 $TEST_DIR/in1 $TEST_DIR/out1
/bin/sleep 1000 $TEST_DIR/in2 $TEST_DIR/out2
/bin/sleep 1000 $TEST_DIR/in3 $TEST_DIR/out3
EOF

cat > "$TEST_DIR/config_one.txt" << EOF
/bin/sleep 1000 $TEST_DIR/in1 $TEST_DIR/out1
EOF

echo "=== Отчет о тестировании myinit ===" > "$RESULT_FILE"

echo "Тест 1: Запуск myinit с тремя процессами" >> "$RESULT_FILE"
echo "Ожидаемый результат: запущено 3 дочерних процесса" >> "$RESULT_FILE"

./myinit -c "$TEST_DIR/config.txt" &
sleep 2

if [ ! -f "$PID_FILE" ]; then
    echo "Фактический результат: PID-файл не создан" >> "$RESULT_FILE"
    echo "Тест 1: НЕ ПРОЙДЕН" >> "$RESULT_FILE"
    exit 1
fi
MYINIT_PID=$(cat "$PID_FILE")

COUNT=$(ps --ppid "$MYINIT_PID" --no-headers 2>/dev/null | wc -l)
echo "Фактический результат: запущено $COUNT дочерних процессов" >> "$RESULT_FILE"
if [ "$COUNT" -eq 3 ]; then
    echo "Тест 1: ПРОЙДЕН" >> "$RESULT_FILE"
else
    echo "Тест 1: НЕ ПРОЙДЕН" >> "$RESULT_FILE"
fi

echo "" >> "$RESULT_FILE"
echo "Тест 2: Завершение процесса и автоматический рестарт" >> "$RESULT_FILE"
echo "Ожидаемый результат: после завершения одного процесса он будет перезапущен" >> "$RESULT_FILE"

CHILD_PIDS=($(ps --ppid "$MYINIT_PID" --no-headers -o pid= 2>/dev/null))
if [ "${#CHILD_PIDS[@]}" -gt 1 ]; then
    kill "${CHILD_PIDS[1]}" 2>/dev/null
    sleep 1
    COUNT=$(ps --ppid "$MYINIT_PID" --no-headers 2>/dev/null | wc -l)
    echo "Фактический результат: запущено $COUNT дочерних процессов после рестарта" >> "$RESULT_FILE"
    if [ "$COUNT" -eq 3 ]; then
        echo "Тест 2: ПРОЙДЕН" >> "$RESULT_FILE"
    else
        echo "Тест 2: НЕ ПРОЙДЕН" >> "$RESULT_FILE"
    fi
else
    echo "Фактический результат: не найдено дочерних процессов для теста" >> "$RESULT_FILE"
    echo "Тест 2: НЕ ПРОЙДЕН" >> "$RESULT_FILE"
fi

echo "" >> "$RESULT_FILE"
echo "Тест 3: Перезагрузка конфигурации по сигналу SIGHUP" >> "$RESULT_FILE"
echo "Ожидаемый результат: после замены конфига и отправки SIGHUP запущен 1 процесс" >> "$RESULT_FILE"

cp "$TEST_DIR/config_one.txt" "$TEST_DIR/config.txt"
kill -HUP "$MYINIT_PID" 2>/dev/null
sleep 3

COUNT=$(ps --ppid "$MYINIT_PID" --no-headers 2>/dev/null | wc -l)
echo "Фактический результат: запущено $COUNT дочерних процессов" >> "$RESULT_FILE"
if [ "$COUNT" -eq 1 ]; then
    echo "Тест 3: ПРОЙДЕН" >> "$RESULT_FILE"
else
    echo "Тест 3: НЕ ПРОЙДЕН" >> "$RESULT_FILE"
fi

echo "" >> "$RESULT_FILE"
echo "=== Содержимое лога $LOG_FILE ===" >> "$RESULT_FILE"
if [ -f "$LOG_FILE" ]; then
    cat "$LOG_FILE" >> "$RESULT_FILE"
else
    echo "Лог-файл не найден" >> "$RESULT_FILE"
fi

kill "$MYINIT_PID" 2>/dev/null
wait "$MYINIT_PID" 2>/dev/null
rm -f "$PID_FILE"
rm -rf "$TEST_DIR"
