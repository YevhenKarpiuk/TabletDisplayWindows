# TabletDisplayWindows

Windows-часть системы, превращающей Android-планшет в дополнительный монитор Windows 11 по USB/ADB.

- Парный проект: `C:\Users\ka\Documents\kotlin\TabletDisplayAndroid`.
- MVP: один виртуальный дисплей 1280×800/1920×1200, 30/60 Гц, H.264, touch → мышь.
- Стек: C++20, UMDF 2/IddCx, D3D11, Media Foundation, Winsock, MSBuild/WDK.
- Транспорт: `adb forward tcp:27183 tcp:27183`, затем бинарный protocol v1 из `doc/PROTOCOL.md`.
- Инструменты: VS 2022 + WDK/Spectre; portable WDK 10.0.26100.6584 в `C:\tools\wdk`; ADB в `C:\tools\android\sdk`.
- Сборка/тест: `build.cmd Debug|Release`, `test.cmd`; demo: `demo.cmd [--serial SERIAL]`.
- Готово: IddCx-каркас собирается; parser/golden tests; ADB handshake/PING; synthetic AVC Baseline sender.
- Проверено на Lenovo TB-X606F: host → ADB/TCP → аппаратный MediaCodec → Surface, 1280×800; PixelCopy подтвердил 53 цвета тестового кадра.
- Portable FFmpeg 9.0.1: `C:\tools\ffmpeg`; нужен только для генерации/проверки fixtures, не для запуска host.
- Далее: test-sign/install драйвера, реальный виртуальный монитор, Driver/Host IPC, Media Foundation encoder, reconnect и touch.
- Подробный план: [doc/IMPLEMENTATION_PLAN.md](doc/IMPLEMENTATION_PLAN.md).
