# Golden vectors

Каждый `.hex` содержит полный wire message: 32-байтный header и payload. Parser игнорирует ASCII whitespace. Ожидаемые поля и ошибки перечислены в `manifest.tsv`.

Векторы v1.0 используют фиксированные nonce, sequence и timestamps только для воспроизводимого теста; production-код генерирует их во время сеанса.
