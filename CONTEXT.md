# TabletDisplayWindows

Windows-часть системы, превращающей Android-планшет в дополнительный монитор Windows 11 по USB/ADB.

- Парный проект: `C:\Users\ka\Documents\kotlin\TabletDisplayAndroid`.
- MVP: один виртуальный дисплей 1280×800/1920×1200 или 800×1280/1200×1920, 30/60 Гц, H.264, single-touch → мышь.
- Стек: C++20, UMDF 2/IddCx, D3D11/DXGI, Media Foundation, Winsock, MSBuild/WDK.
- Транспорт: `adb forward tcp:27183 tcp:27183`, protocol v1 из `doc/PROTOCOL.md`.
- Инструменты: VS 2022 + WDK/Spectre; portable WDK 10.0.26100.6584, Android SDK и FFmpeg 9.0.1 в `C:\tools`.
- Команды: `build.cmd`, `test.cmd`, `install-driver.cmd`, `check-driver.cmd`, `uninstall-driver.cmd`, `virtual-stream.cmd`, `release.cmd`.
- Готово: test-signed IddCx `ROOT\DISPLAY\0000`; latest-frame IOCTL; автоматический режим 1280×800@60; конвейер capture/BGRA→NV12/encode; ADB TCP; двунаправленный protocol v1; single-touch → Windows mouse.
- Lenovo TB-X606F: поток виртуального дисплея 1280×800@60 отображается аппаратным `OMX.MTK.VIDEO.DECODER.AVC`; Windows кодирует Intel Quick Sync H.264, Android синхронизирует вывод через одно-кадровый pacing.
- `stream.cmd` требует обычный интерактивный Windows-сеанс; Desktop Duplication недоступен из service/sandbox.
- IddCx: test-signed пакет установлен при включённом Secure Boot; `ROOT\DISPLAY\0000` и дочерний монитор запущены без ошибок, monitor container GUID стабилен. Test Mode не нужен.
- Проверено на `HA15VXDN`: 1280×800 и 800×1280 @60, tap/drag, mouse-up при разрыве и Host reconnect; аппаратные encoder/decoder, сборки и tests проходят. Host поворачивает raw IddCx-кадр без переключения видеорежимов.
- Release 0.1.0: test-signed driver ZIP с `Install.cmd`/`Uninstall.cmd`, portable tray Host с bundled ADB и Android release APK; общий значок в `assets`/Android mipmap.
- Далее: длительный soak-тест и GPU zero-copy/shared texture.
- Подробный план: [doc/IMPLEMENTATION_PLAN.md](doc/IMPLEMENTATION_PLAN.md).
