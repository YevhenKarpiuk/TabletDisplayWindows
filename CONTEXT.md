# TabletDisplayWindows

Windows-часть системы, превращающей Android-планшет в дополнительный монитор Windows 11 по USB/ADB.

- Парный проект: `C:\Users\ka\Documents\kotlin\TabletDisplayAndroid`.
- MVP: один виртуальный дисплей 1280×800/1920×1200 или 800×1280/1200×1920, 30/60 Гц, H.264, single-touch → мышь.
- Стек: C++20, UMDF 2/IddCx, D3D11/DXGI, Media Foundation, Winsock, MSBuild/WDK.
- Транспорт: `adb forward tcp:27183 tcp:27183`, protocol v1 из `doc/PROTOCOL.md`.
- Инструменты: VS 2022 + WDK/Spectre; portable WDK 10.0.26100.6584, Android SDK и FFmpeg 9.0.1 в `C:\tools`.
- Команды: `build.cmd`, `test.cmd`, `install-driver.cmd`, `check-driver.cmd`, `uninstall-driver.cmd`, `virtual-stream.cmd`, `release.cmd`.
- Готово: test-signed IddCx `ROOT\DISPLAY\0000`; динамический monitor lease после Android-handshake; shared D3D11 texture driver→Host с keyed mutex; GPU BGRA→NV12/rotation и zero-copy NV12→Quick Sync; CPU IPC fallback; ADB TCP; protocol v1; single-touch → Windows mouse.
- Lenovo TB-X606F: поток @60 отображается аппаратным `OMX.MTK.VIDEO.DECODER.AVC`; Windows использует realtime/low-latency Intel Quick Sync H.264, Android выводит кадр на ближайшей развёртке.
- `stream.cmd` требует обычный интерактивный Windows-сеанс; Desktop Duplication недоступен из service/sandbox.
- IddCx: test-signed пакет установлен при включённом Secure Boot; `ROOT\DISPLAY\0000` и дочерний монитор запущены без ошибок, monitor container GUID стабилен. Test Mode не нужен.
- Проверено на `HA15VXDN`: 1280×800 и 800×1280 @60, tap/drag, reconnect, аппаратные encoder/decoder. Shared-texture поток 60 секунд стабилен (~112,5 МБ private); CPU fallback работает после штатного выхода и аварийного завершения Host.
- Release 0.1.5: test-signed driver ZIP с `Install.cmd`/`Uninstall.cmd`, portable tray Host с bundled ADB, динамическим монитором, мягким отсоединением display path и восстановлением taskbar-регистрации окон после monitor departure, автозапуском текущего пользователя и остановкой потока при выключении дисплеев Windows; Android release APK 0.1.2.
- Далее: субъективная проверка задержки и длительный (60 минут) soak/reconnect; затем выпуск обновлённого release-комплекта.
- Подробный план: [doc/IMPLEMENTATION_PLAN.md](doc/IMPLEMENTATION_PLAN.md).
