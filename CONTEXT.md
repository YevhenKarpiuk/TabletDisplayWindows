# TabletDisplayWindows

Windows-часть системы, превращающей Android-планшет в дополнительный монитор Windows 11 по USB/ADB.

- Парный проект: `C:\Users\ka\Documents\kotlin\TabletDisplayAndroid`.
- MVP: один виртуальный дисплей 1280×800/1920×1200, 30/60 Гц, H.264, touch → мышь.
- Стек: C++20, UMDF 2/IddCx, D3D11/DXGI, Media Foundation, Winsock, MSBuild/WDK.
- Транспорт: `adb forward tcp:27183 tcp:27183`, protocol v1 из `doc/PROTOCOL.md`.
- Инструменты: VS 2022 + WDK/Spectre; portable WDK 10.0.26100.6584, Android SDK и FFmpeg 9.0.1 в `C:\tools`.
- Команды: `build.cmd`, `test.cmd`, `install-driver.cmd`, `check-driver.cmd`, `uninstall-driver.cmd`, `demo.cmd`, `encoder-demo.cmd`, `stream.cmd`, `virtual-stream.cmd`.
- Готово: IddCx-каркас и установка; latest-frame IOCTL из swap-chain в Host; parser/tests; ADB session; synthetic H.264; DXGI primary-desktop capture; BGRA→NV12; low-latency Media Foundation H.264 sender.
- Lenovo TB-X606F: synthetic/MF H.264 и реальный DXGI desktop stream 1280×800 отображаются аппаратным `OMX.MTK.VIDEO.DECODER.AVC`; исправлен перекрывавший видео background Android `SurfaceView`.
- CPU capture сейчас использует software H264 MFT. Intel Quick Sync обнаружен, но прямой hardware path будет включён вместе с D3D11-текстурами IddCx.
- `stream.cmd` требует обычный интерактивный Windows-сеанс; Desktop Duplication недоступен из service/sandbox.
- IddCx: test-signed пакет установлен при включённом Secure Boot; `ROOT\DISPLAY\0000` и дочерний монитор запущены без ошибок, monitor container GUID стабилен. Test Mode не нужен.
- Далее: проверить `virtual-stream.cmd` на планшете; затем GPU zero-copy/hardware encoder, reconnect и touch.
- Подробный план: [doc/IMPLEMENTATION_PLAN.md](doc/IMPLEMENTATION_PLAN.md).
