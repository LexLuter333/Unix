#!/bin/bash
set -e  # Остановка при ошибке

echo "=== Сборка программы ==="
make clean > /dev/null 2>&1 || true
make

echo "=== Создание тестового файла A ==="
chmod +x create_test_file_A.sh
./create_test_file_A.sh

echo "" > result.txt
echo "=== BEGIN STEPS ===" >> result.txt

echo "1. Копирование A -> B (создание sparse-файла)" | tee -a result.txt
./myprogram A B

echo "2. Сжатие A и B через gzip" | tee -a result.txt
gzip -c A > A.gz
gzip -c B > B.gz

echo "3. Распаковка B.gz -> C через программу (stdin режим)" | tee -a result.txt
gzip -cd B.gz | ./myprogram C

echo "4. Копирование A -> D с блоком 100 байт" | tee -a result.txt
./myprogram -b 100 A D

echo "" >> result.txt
echo "=== STAT RESULTS ===" >> result.txt
echo "" >> result.txt

for f in A A.gz B B.gz C D; do
    echo "--- $f ---" >> result.txt
    stat "$f" >> result.txt 2>&1 || echo "Файл $f не найден" >> result.txt
    echo "" >> result.txt
done

echo ""
echo "=== Результаты сохранены в result.txt ==="
echo "Рекомендуется проверить:"
echo "  • B и D должны иметь малый физический размер (sparse)"
echo "  • A.gz и B.gz должны быть близки по размеру (gzip эффективно сжимает нули)"
echo "  • C должен совпадать с B по содержимому (проверка целостности)"
