# TabletDisplay wire protocol v1.0

Статус: **зафиксирован для MVP**. Этот файл и каталог `protocol/golden` должны изменяться одновременно и оставаться побитово одинаковыми в Windows- и Android-репозиториях.

Ключевые слова MUST, MUST NOT, SHOULD и MAY используются в нормативном смысле.

## 1. Транспорт и модель соединения

- Для USB Android слушает `127.0.0.1:27183`; для Wi-Fi — порт `27185` с шифрованием и сопряжением (раздел 10).
- Windows выполняет `adb -s <serial> forward tcp:<local> tcp:27183` и открывает один TCP-сеанс на устройство. При нескольких одновременно подключённых планшетах каждый получает свой локальный порт (`27183`, `27184`, …) и независимый сеанс; сам протокол от этого не меняется.
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
| 0 | 4 | физическая ширина дисплея в текущей ориентации, px |
| 4 | 4 | физическая высота дисплея в текущей ориентации, px |
| 8 | 4 | density, dpi |
| 12 | 4 | refresh rate, mHz |
| 16 | 4 | максимальная AVC width декодера |
| 20 | 4 | максимальная AVC height декодера |
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

`payload_length == 28 + csd_length`. CSD начинается с трёх- или четырёхбайтного Annex B start code и содержит SPS/PPS. Width и height чётные, каждая в диапазоне `128..4096`, FPS равен 30/1 или 60/1. Windows выбирает режим по `DISPLAY_INFO`: исходное разрешение дисплея или исходное, делённое на 2, 3 или 4 с округлением вниз до чётного, в ориентации монитора Windows; режим не превышает пределы AVC из `DISPLAY_INFO`.

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

## 10. Wi-Fi транспорт

Wi-Fi работает без ADB. Сообщения протокола v1 (разделы 3–9) не меняются: они передаются внутри зашифрованного канала, описанного ниже. Все строки ниже — ASCII без завершающего нуля, `||` — конкатенация.

### 10.1 Обнаружение и порт

- Android слушает TCP `27185` на всех IPv4-интерфейсах, пока приложение на экране.
- Android публикует DNS-SD/mDNS сервис `_tabletdisplay._tcp` на этом порту. Имя экземпляра — модель устройства, TXT: `id=<32 hex>` (постоянный случайный идентификатор установки), `v=1`.
- Windows всегда клиент. Одновременно Android обслуживает один сеанс протокола (USB или Wi-Fi); второй получает `HELLO_ACK` со статусом `BUSY`.

### 10.2 Кадры рукопожатия

До установки шифрования каждое сообщение — `u32 length` (little-endian, `1..65536`) и тело. Первое тело от Windows начинается с magic `TDW1`, затем байт типа; остальные тела начинаются с байта типа. Идентификаторы `id_w`/`id_a` — 16 случайных байт, постоянные для установки Windows/Android. Ключи — P-256, открытый ключ — 65 байт в несжатой форме (`04 || X || Y`), общий секрет `Z` — 32 байта координаты X (big-endian).

### 10.3 Сопряжение (один раз)

| Шаг | Направление | Тело |
|---|---|---|
| P1 | W→A | `TDW1` · `01` · `id_w[16]` · `name_len u8` · `name` · `pk_w[65]` |
| P2 | A→W | `02` · `id_a[16]` · `name_len u8` · `name` · `pk_a[65]` · `commit[32]` |
| P3 | W→A | `03` · `n_w[16]` |
| P4 | A→W | `04` · `n_a[16]` |
| P5 | оба | `05` · `accepted u8` (`1` — пользователь подтвердил код, `0` — отклонил) |

- `commit = SHA-256("TDW1 commit" || pk_a || pk_w || n_a)`; Windows проверяет его после P4. Обязательство не даёт посреднику подобрать nonce под нужный код.
- Код сравнения: `SAS = BE32(SHA-256("TDW1 sas" || pk_w || pk_a || n_w || n_a)[0..4]) mod 1000000`, показывается шестью цифрами на обоих устройствах. Пользователь подтверждает, что коды совпадают.
- Ключ сопряжения `K = HMAC-SHA256(Z, "TDW1 pair" || pk_w || pk_a || n_w || n_a)`. Каждая сторона сохраняет `K` и идентификатор партнёра только если подтвердила сама и получила `accepted = 1`. Таймаут ожидания решения — 120 с.

### 10.4 Сеанс

| Шаг | Направление | Тело |
|---|---|---|
| S1 | W→A | `TDW1` · `10` · `id_w[16]` · `s_w[32]` |
| S2 | A→W | `11` · `status u8` · `id_a[16]` · `s_a[32]` · `mac_a[32]` |
| S3 | W→A | `12` · `mac_w[32]` |

- `status = 1` — Windows не сопряжён с этим Android; тогда S2 состоит только из `11 01`, и соединение закрывается.
- `mac_a = HMAC-SHA256(K, "TDW1 server" || id_w || id_a || s_w || s_a)`, `mac_w = HMAC-SHA256(K, "TDW1 client" || id_w || id_a || s_w || s_a)`. Windows проверяет `id_a` и `mac_a`, Android — `mac_w`; при ошибке соединение закрывается.
- Ключи направлений: `key_w2a = HMAC-SHA256(K, "TDW1 w2a" || s_w || s_a)`, `key_a2w = HMAC-SHA256(K, "TDW1 a2w" || s_w || s_a)`.

### 10.5 Шифрованные записи

После S3 каждый поток байтов протокола v1 передаётся записями: `u32 N` (little-endian) · `ciphertext[N-16]` · `tag[16]`. Шифр — AES-256-GCM, nonce — 4 нулевых байта и `u64` счётчик записей направления (little-endian, с нуля), AAD — 4 байта `N`. `N` в диапазоне `17..16 MiB + 16`. Ошибка проверки тега, переполнение счётчика или неверная длина закрывают соединение. Границы записей не обязаны совпадать с границами сообщений.

### 10.6 Тестовые векторы

`d_w = SHA-256("windows private") mod p`, `d_a = SHA-256("android private") mod p`, `n_w = 00..0f`, `n_a = 10..1f`, `id_w = 20..2f`, `id_a = 30..3f`, `s_w = 40..5f`, `s_a = 60..7f`:

```
pk_w   04e8027af4ad682be3a309ea73145fbd202cb00420aa9fcbd0b1dc761e87bda40c6ab23185a2fe7743ee6a1127dd204eb2a8ba11ab134f7b25f60a38642c405af3
pk_a   042a03eb4a58771474e7f228eab79050860361563168751ddd7bb43865001a1c6e48fa12b852bcde33f8bd247b6a0f4afaa512c81f37fc21eb7ffd8be9d3cff67b
Z      c0f8077e1eba2503e67030e472e0a50116a9ac779f1619dee9693b2f5085fd2b
commit 643e92289bac675c26a92ac4217b459aee1ebab1bd7c11494a54f0191d2947b3
SAS    063562
K      0287e6bb7e19591233de56920f2a42d43c45c0a1e0d73ed08e0bae9a2eccc76c
mac_a  1c9b48aa5eb9ddd2911c27552dc9ff16b9b34dc0d6550e38137717fd2d54a805
mac_w  3c229f72f9b85f815b7f52edd31fd9ff88e19a602f20590eedb5963c57343e0e
key_w2a 712c4bef122d7bd457a06f902b1fd586a44501cb96bec1ed55fe778666ff2307
key_a2w 031be4c4307f4092841bfd943ed9ef8c0aea9e99d5afc70fa8bf5d9130519221
```

Запись W→A со счётчиком 1 и открытым текстом `TabletDisplay record`:
`240000008606ab897ebbd39aea03bc14a402399a7773490910c0124457071d5122b2ad641b9d6241`.
