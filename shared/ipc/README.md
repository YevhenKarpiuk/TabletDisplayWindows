# Driver/host IPC

`FrameTransport.h` фиксирует внутренний ABI Driver/Host v1. Драйвер копирует IddCx BGRA surface в staging texture и хранит только последний кадр. Host открывает device interface `{C6E7470B-1371-4F25-9EEC-4EF58D3A84CE}` и запрашивает кадр read-only IOCTL, передавая последний принятый sequence.

Лимит кадра — 1920×1200 или 1200×1920 BGRA8. Отсутствие Host не блокирует swap-chain; медленный Host пропускает устаревшие кадры. Device security разрешает полный доступ SYSTEM/Administrators и чтение Builtin Users.
