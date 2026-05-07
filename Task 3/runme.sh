#!/bin/bash
set -e

MYINIT="./myinit"
LOGFILE="/tmp/myinit.log"
TESTDIR="/tmp/myinit_test_$$"
RESULT_FILE="result.txt"
ORIG_DIR="$(pwd)"

cleanup() {
    if [ -n "$MYINIT_PID" ] && kill -0 "$MYINIT_PID" 2>/dev/null; then
        kill "$MYINIT_PID" 2>/dev/null || true
        wait "$MYINIT_PID" 2>/dev/null || true
    fi
    rm -rf "$TESTDIR"
}
trap cleanup EXIT

# 1. Сборка и подготовка
echo "[1/5] Компиляция исходного кода..."
gcc -Wall -Wextra -pedantic -std=c99 -D_POSIX_C_SOURCE=200809L -o "$ORIG_DIR/$MYINIT" "$ORIG_DIR/myinit.c" 2>/dev/null
echo "[OK] Бинарный файл успешно создан."

echo "[2/5] Развёртывание тестовой среды..."
mkdir -p "$TESTDIR"
cd "$TESTDIR"
rm -f /tmp/test*.log /tmp/in* /tmp/out* "$LOGFILE"
touch /tmp/in1 /tmp/in2 /tmp/in3 /tmp/out1 /tmp/out2 /tmp/out3

cat > test1.sh << 'EOF'
#!/bin/bash
while true; do sleep 5; done
EOF
cat > test2.sh << 'EOF'
#!/bin/bash
while true; do sleep 5; done
EOF
cat > test3.sh << 'EOF'
#!/bin/bash
while true; do sleep 5; done
EOF
chmod +x test*.sh

cat > config3.txt << EOF
$TESTDIR/test1.sh /tmp/in1 /tmp/out1
$TESTDIR/test2.sh /tmp/in2 /tmp/out2
$TESTDIR/test3.sh /tmp/in3 /tmp/out3
EOF

cat > config1.txt << EOF
$TESTDIR/test1.sh /tmp/in1 /tmp/out1
EOF
echo "[OK] Тестовые скрипты и конфигурации размещены."

> "$ORIG_DIR/$RESULT_FILE"
echo "Результаты тестирования myinit | $(date)" >> "$ORIG_DIR/$RESULT_FILE"
echo "----------------------------------------" >> "$ORIG_DIR/$RESULT_FILE"

# 2. Запуск и проверка количества процессов
echo "[3/5] Инициализация демона с тремя воркерами..."
"$ORIG_DIR/$MYINIT" config3.txt &
sleep 2

DAEMON_PID=$(pgrep -f "myinit config3.txt" | head -n 1)
if [ -z "$DAEMON_PID" ]; then
    echo "[FAIL] Демон не запустился. Проверьте журнал."
    echo "[FAIL] Тест 1: запуск демона" >> "$ORIG_DIR/$RESULT_FILE"
    exit 1
fi
MYINIT_PID="$DAEMON_PID"

CNT=$(ps --ppid "$DAEMON_PID" -o pid= 2>/dev/null | wc -l)
if [ "$CNT" -eq 3 ]; then
    echo "[OK] Демон корректно породил 3 дочерних процесса."
    echo "[PASS] Тест 1: 3 процесса запущены" >> "$ORIG_DIR/$RESULT_FILE"
else
    echo "[FAIL] Ожидалось 3 процесса, обнаружено: $CNT"
    echo "[FAIL] Тест 1: количество процессов не совпадает" >> "$ORIG_DIR/$RESULT_FILE"
    exit 1
fi

# 3. Тест отказоустойчивости
echo "[4/5] Имитация падения worker2 и проверка восстановления..."
PIDS=($(ps --ppid "$DAEMON_PID" -o pid=))
if [ ${#PIDS[@]} -gt 1 ]; then
    kill -9 "${PIDS[1]}" 2>/dev/null || true
fi
sleep 2

CNT=$(ps --ppid "$DAEMON_PID" -o pid= 2>/dev/null | wc -l)
if [ "$CNT" -eq 3 ]; then
    echo "[OK] Завершённый воркер успешно восстановлен."
    echo "[PASS] Тест 2: автоматический рестарт" >> "$ORIG_DIR/$RESULT_FILE"
else
    echo "[FAIL] Восстановление не выполнено, процессов: $CNT"
    echo "[FAIL] Тест 2: рестарт не сработал" >> "$ORIG_DIR/$RESULT_FILE"
    exit 1
fi

# 4. SIGHUP и лог
echo "[5/5] Динамическая подмена конфигурации (SIGHUP) и аудит..."
cp config1.txt config3.txt
kill -HUP "$DAEMON_PID"
sleep 2

CNT=$(ps --ppid "$DAEMON_PID" -o pid= 2>/dev/null | wc -l)
if [ "$CNT" -eq 1 ]; then
    echo "[OK] Конфигурация обновлена, пул процессов синхронизирован."
    echo "[PASS] Тест 3: обработка SIGHUP" >> "$ORIG_DIR/$RESULT_FILE"
else
    echo "[FAIL] После перезагрузки ожидался 1 процесс, обнаружено: $CNT"
    echo "[FAIL] Тест 3: ошибка обработки сигнала" >> "$ORIG_DIR/$RESULT_FILE"
    exit 1
fi

if [ ! -f "$LOGFILE" ]; then
    echo "[FAIL] Файл журнала отсутствует."
    echo "[FAIL] Тест 4: лог не найден" >> "$ORIG_DIR/$RESULT_FILE"
    exit 1
fi

LOG_OK=true
grep -q "START" "$LOGFILE" || LOG_OK=false
grep -q "EXIT\|RESTART" "$LOGFILE" || LOG_OK=false
grep -q "SIGHUP" "$LOGFILE" || LOG_OK=false

if $LOG_OK; then
    echo "[OK] Все ключевые события зафиксированы в журнале."
    echo "[PASS] Тест 4: валидация лога" >> "$ORIG_DIR/$RESULT_FILE"
else
    echo "[FAIL] В журнале отсутствуют критические записи."
    echo "[FAIL] Тест 4: неполный лог" >> "$ORIG_DIR/$RESULT_FILE"
    exit 1
fi

# Корректное завершение демона, чтобы в лог успела попасть финальная запись
kill "$DAEMON_PID" 2>/dev/null || true
wait "$DAEMON_PID" 2>/dev/null || true
MYINIT_PID=""

echo ""
echo "Все этапы верификации пройдены успешно."
echo ""
echo "=== Журнал работы программы ==="
if [ -f "$LOGFILE" ]; then
    cat "$LOGFILE"
else
    echo "⚠ Лог-файл не найден"
fi
echo "=================================="
exit 0
