# TabletDisplay wire protocol v1.0

Статус: **зафиксирован для MVP**. Этот файл и каталог `protocol/golden` должны изменяться одновременно и оставаться побитово одинаковыми в Windows- и Android-репозиториях.

Ключевые слова MUST, MUST NOT, SHOULD и MAY используются в нормативном смысле.

## 1. Транспорт и модель соединения

- Android слушает только `127.0.0.1:27183`.
- Windows выполняет `adb forward tcp:27183 tcp:27183` и открывает один TCP-сеанс.
- Windows всегда инициирует handshake. Одновременно допускается один клиент.
- TCP передаёт поток байтов: границы одного `send` не являются границами сообщения.
- CRC не используется. При ошибке framing/parser соединение закрывается; искать следующий magic внутри потока запрещено, потому что такие байты могут встретиться в H.264.
- После reconnect обе стороны создают новые nonce, сбрасывают sequence в `1` и повторяют полный handshake, `DISPLAY_INFO`, `VIDEO_CONFIG` и IDR.

## 2. Представление данных

- Все многобайтовые целые — unsigned little-endian.
- Значения sequence и времени находятся в диапазоне `1..2^63-1`.
- Время — монотонные микросекунды часов отправителя, не Unix time. Часы разных сторон напрямую не сравниваются.
- Строки — строгий UTF-8 без BOM и завершающего NUL.
- Зарезервированные поля и биты отправитель заполняет нулями; получатель v1.0 отклоняет ненулевые значения.

## 3. Общий заголовок

Каждое сообщение состоит из 32-байтного заголовка и `payload_length` байт payload.

| Offset | Size | Поле | Значение |
|---:|---:|---|---|
| 0 | 4 | `magic` | ASCII `TDS1`, байты `54 44 53 31` |
| 4 | 1 | `version_major` | `1` |
| 5 | 1 | `version_minor` | `0` |
| 6 | 2 | `header_size` | `32` |
| 8 | 2 | `message_type` | тип из раздела 5 |
| 10 | 2 | `flags` | флаги из раздела 4 |
| 12 | 4 | `payload_length` | размер payload без заголовка |
| 16 | 8 | `sequence` | независимый счётчик направления |
| 24 | 8 | `timestamp_us` | монотонное время создания сообщения |

Получатель MUST проверить magic, версию, размер заголовка, флаги, sequence и лимит payload до выделения памяти под payload.

Sequence начинается с `1` отдельно для Windows→Android и Android→Windows и увеличивается ровно на один для каждого сообщения, включая пропущенный optional type. Разрыв sequence поверх TCP означает повреждение состояния протокола: отправляется fatal `ERROR(SEQUENCE_GAP)`, затем соединение закрывается.

## 4. Флаги заголовка

| Bit | Имя | Назначение |
|---:|---|---|
| 0 | `CRITICAL` | неизвестный тип нельзя пропустить |
| 1 | `KEY_FRAME` | `VIDEO_FRAME` содержит IDR |
| 2 | `DISCONTINUITY` | decoder должен сбросить зависимое состояние до этого frame |
| 3 | `END_OF_STREAM` | штатное завершение текущего stream |

Все определённые v1.0 сообщения MUST иметь `CRITICAL`. Биты 1–3 разрешены только у `VIDEO_FRAME`. Неизвестный тип с очищенным `CRITICAL` и нулевыми остальными флагами пропускается после проверки длины и sequence. Неизвестный critical type завершает сеанс.

## 5. Типы сообщений

| Код | Имя | Направление | Payload |
|---:|---|---|---:|
| `0x0001` | `HELLO` | Windows→Android | 32 |
| `0x0002` | `HELLO_ACK` | Android→Windows | 32 |
| `0x0003` | `DISPLAY_INFO` | Android→Windows | 40 |
| `0x0010` | `VIDEO_CONFIG` | Windows→Android | 28 + CSD |
| `0x0011` | `VIDEO_FRAME` | Windows→Android | 32 + access unit |
| `0x0012` | `REQUEST_KEYFRAME` | Android→Windows | 24 |
| `0x0020` | `TOUCH` | Android→Windows | 32 |
| `0x0030` | `PING` | оба | 16 |
| `0x0031` | `PONG` | оба | 24 |
| `0x00FF` | `ERROR` | оба | 20 + text |

Сообщение известного типа в неверном направлении является `UNEXPECTED_MESSAGE`.

### 5.1 HELLO

| Offset | Size | Поле |
|---:|---:|---|
| 0 | 1 | `role`: `1 = WINDOWS_HOST` |
| 1 | 1 | reserved |
| 2 | 1 | `min_major` |
| 3 | 1 | `max_major` |
| 4 | 1 | `min_minor` |
| 5 | 1 | `max_minor` |
| 6 | 2 | reserved |
| 8 | 8 | случайный `session_nonce` |
| 16 | 8 | `capabilities` |
| 24 | 4 | максимальный принимаемый payload |
| 28 | 2 | желаемый heartbeat interval, ms |
| 30 | 2 | idle timeout, ms |

Для v1.0 диапазон версий равен `1.0..1.0`. Heartbeat по умолчанию `1000 ms`, idle timeout `5000 ms`; idle timeout MUST быть не меньше трёх heartbeat interval.

Capabilities:

| Bit | Имя |
|---:|---|
| 0 | `H264_ANNEX_B` |
| 1 | `H264_BASELINE` |
| 2 | `H264_MAIN` |
| 3 | `H264_HIGH` |
| 4 | `TOUCH_SINGLE` |
| 5 | `KEYFRAME_REQUEST` |
| 6 | `DISPLAY_INFO` |
| 7 | `HEARTBEAT` |

### 5.2 HELLO_ACK

| Offset | Size | Поле |
|---:|---:|---|
| 0 | 2 | status: `0 OK`, `1 UNSUPPORTED_VERSION`, `2 MISSING_CAPABILITY`, `3 BUSY` |
| 2 | 1 | selected major |
| 3 | 1 | selected minor |
| 4 | 4 | reserved |
| 8 | 8 | echoed Windows nonce |
| 16 | 8 | Android nonce |
| 24 | 8 | пересечение capabilities |

При ненулевом status Android отправляет ACK и закрывает соединение. Для MVP обязательны `H264_ANNEX_B`, хотя бы один общий AVC profile, `KEYFRAME_REQUEST`, `DISPLAY_INFO` и `HEARTBEAT`.

### 5.3 DISPLAY_INFO

| Offset | Size | Поле |
|---:|---:|---|
| 0 | 4 | физическая ширина Surface, px |
| 4 | 4 | физическая высота Surface, px |
| 8 | 4 | density, dpi |
| 12 | 4 | refresh rate, mHz |
| 16 | 4 | максимальная AVC width |
| 20 | 4 | максимальная AVC height |
| 24 | 4 | максимальный AVC FPS |
| 28 | 4 | максимальный размер одного access unit |
| 32 | 4 | profile mask: bit 0 Baseline, bit 1 Main, bit 2 High |
| 36 | 2 | clockwise rotation: `0`, `90`, `180`, `270` |
| 38 | 2 | reserved |

### 5.4 VIDEO_CONFIG

| Offset | Size | Поле |
|---:|---:|---|
| 0 | 4 | `stream_id`, ненулевой |
| 4 | 4 | `config_id`, начинается с 1 и растёт при reconfigure |
| 8 | 2 | width |
| 10 | 2 | height |
| 12 | 2 | FPS numerator |
| 14 | 2 | FPS denominator, ненулевой |
| 16 | 1 | AVC `profile_idc`: 66, 77 или 100 |
| 17 | 1 | AVC `level_idc` |
| 18 | 2 | config flags; bit 0 `ANNEX_B_CSD` обязателен |
| 20 | 4 | целевой bitrate, bit/s |
| 24 | 4 | `csd_length` |
| 28 | N | SPS/PPS в Annex B |

`payload_length == 28 + csd_length`. CSD начинается с трёх- или четырёхбайтного Annex B start code и содержит SPS/PPS. MVP допускает только 1280×800 и 1920×1200 при 30/1 или 60/1 FPS.

### 5.5 VIDEO_FRAME

| Offset | Size | Поле |
|---:|---:|---|
| 0 | 4 | `stream_id` |
| 4 | 4 | `config_id` |
| 8 | 8 | `frame_id`, начинается с 1 |
| 16 | 8 | presentation time, us |
| 24 | 4 | `data_length` |
| 28 | 4 | reserved |
| 32 | N | один полный H.264 access unit в Annex B |

`payload_length == 32 + data_length`. Один message равен одному access unit; объединять или делить access unit произвольно запрещено. Ненулевой data начинается с Annex B start code. IDR MUST иметь `KEY_FRAME`. При `END_OF_STREAM` data length равен нулю; в остальных случаях он ненулевой.

`DISCONTINUITY` означает: очистить bounded queue, сбросить decoder и ждать этот IDR. После `VIDEO_CONFIG`, reconnect или `REQUEST_KEYFRAME` первым frame должен быть IDR с `KEY_FRAME|DISCONTINUITY`.

### 5.6 REQUEST_KEYFRAME

| Offset | Size | Поле |
|---:|---:|---|
| 0 | 4 | `stream_id` |
| 4 | 4 | `config_id` |
| 8 | 8 | последний корректно принятый `frame_id`, либо 0 |
| 16 | 2 | reason |
| 18 | 2 | reserved |
| 20 | 4 | reserved |

Reason: `1 FRAME_GAP`, `2 DECODER_ERROR`, `3 SURFACE_RESTORED`, `4 RECONNECT`, `5 QUEUE_OVERFLOW`.

### 5.7 TOUCH

| Offset | Size | Поле |
|---:|---:|---|
| 0 | 4 | `stream_id` |
| 4 | 4 | `pointer_id`; для MVP 0 |
| 8 | 1 | action: `1 DOWN`, `2 MOVE`, `3 UP`, `4 CANCEL` |
| 9 | 1 | buttons; bit 0 primary |
| 10 | 2 | normalized X, `0..65535` |
| 12 | 2 | normalized Y, `0..65535` |
| 14 | 2 | pressure, `0..65535` |
| 16 | 4 | `contact_sequence`, начинается с 1 |
| 20 | 8 | Android event time, us |
| 28 | 4 | reserved |

`0` соответствует левому/верхнему краю display rectangle, `65535` — правому/нижнему. Letterbox/pillarbox не входит в rectangle. После DOWN все события контакта имеют тот же pointer id; разрыв сеанса или потеря Surface требует CANCEL.

### 5.8 PING и PONG

`PING`: token u64 и sender time u64. `PONG`: тот же token, echoed sender time u64 и responder time u64. PONG отправляется немедленно. Если за idle timeout не получено ни одного полного сообщения, сторона закрывает соединение.

### 5.9 ERROR

| Offset | Size | Поле |
|---:|---:|---|
| 0 | 2 | error code |
| 2 | 1 | severity: `1 WARNING`, `2 RECOVERABLE`, `3 FATAL` |
| 3 | 1 | reserved |
| 4 | 2 | offending message type, либо 0 |
| 6 | 2 | reserved |
| 8 | 8 | offending sequence, либо 0 |
| 16 | 2 | UTF-8 text length |
| 18 | 2 | reserved |
| 20 | N | диагностический текст |

Коды: `1 MALFORMED_HEADER`, `2 UNSUPPORTED_VERSION`, `3 UNKNOWN_CRITICAL_TYPE`, `4 PAYLOAD_TOO_LARGE`, `5 INVALID_PAYLOAD`, `6 UNEXPECTED_MESSAGE`, `7 UNSUPPORTED_CODEC`, `8 INTERNAL_ERROR`, `9 TIMEOUT`, `10 SEQUENCE_GAP`.

Текст предназначен только для диагностики, не для ветвления логики. После FATAL отправитель закрывает TCP сразу после записи ERROR. Если заголовок нельзя безопасно разобрать, соединение закрывается без ERROR.

## 6. State machine

1. Windows TCP connect → `HELLO(seq=1)`.
2. Android → `HELLO_ACK(seq=1)`; при OK сразу `DISPLAY_INFO(seq=2)`.
3. Windows → `VIDEO_CONFIG(seq=2)`.
4. Windows → IDR `VIDEO_FRAME(seq=3, KEY_FRAME|DISCONTINUITY)`, затем обычные frames.
5. Android MAY отправлять `TOUCH` и `REQUEST_KEYFRAME` только после принятого config.
6. `PING/PONG` разрешены после успешного ACK и используют общие sequence своих направлений.
7. Новый `VIDEO_CONFIG` атомарно заменяет старый config; frame со старым config id после этого отклоняется.

## 7. Лимиты и безопасный parser

| Значение | Лимит |
|---|---:|
| общий payload | 8,388,640 bytes |
| H.264 access unit | 8,388,608 bytes |
| CSD | 65,536 bytes |
| ERROR text | 1,024 bytes |
| header | ровно 32 bytes |
| handshake timeout | 5,000 ms |

Эффективный лимит payload равен минимуму hard limit и значения из HELLO. Reader хранит не более одного заголовка и одного текущего payload, корректно обрабатывает любое разбиение TCP, EOF и timeout. Проверки умножений/сложений выполняются до allocation.

## 8. Совместимость

- Изменение major означает несовместимый framing или смысл существующего поля.
- Minor MAY добавлять optional message types с очищенным `CRITICAL`.
- Размер и смысл существующих v1.0 payload не меняются.
- Новый critical type или flag требует новой согласованной версии.
- Обе реализации и golden vectors обновляются одним логическим изменением.

## 9. Golden vectors

Канонические байты находятся в `protocol/golden/*.hex`; пробелы и переводы строк незначимы. `manifest.tsv` задаёт направление, ожидаемый результат, type, flags, payload length и sequence.

Положительные vectors должны:

1. разбираться при любом разбиении на TCP chunks;
2. после decode→encode давать побитово исходные байты;
3. совпадать между Windows и Android.

Отрицательные vectors проверяют bad magic, oversized payload и sequence gap до опасного allocation.
