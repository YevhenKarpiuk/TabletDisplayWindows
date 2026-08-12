# Сборка TabletDisplayWindows

## Зафиксированное окружение

- Windows 11 x64.
- Visual Studio 2022: MSVC v143, Windows Driver Kit, Spectre libraries (x64).
- Windows SDK/WDK `10.0.26100.6584`, целевая SDK `10.0.26100.0`.
- ADB: `C:\tools\android\sdk\platform-tools\adb.exe`.
- Portable FFmpeg 9.0.1: `C:\tools\ffmpeg`; только для проверок, не runtime-зависимость.

## Команды

```cmd
build.cmd Debug
build.cmd Release
test.cmd
connect.cmd [--serial SERIAL]
demo.cmd [--serial SERIAL]
encoder-demo.cmd [--serial SERIAL] [--seconds N]
stream.cmd [--serial SERIAL] [--seconds N]
```

- `build.cmd` восстанавливает portable WDK/SDK и собирает solution без подписи.
- `test.cmd` запускает parser/golden, H.264 и BGRA→NV12/Annex B tests.
- `encoder-demo.cmd` кодирует динамический 1280×800/30 паттерн штатным software H264 MFT и передаёт его Android.
- `stream.cmd` захватывает основной дисплей через DXGI Desktop Duplication, преобразует BGRA→NV12 и передаёт H.264. Нужен интерактивный desktop.

Артефакты: `artifacts\x64\<Configuration>`.

## Текущее состояние

- Driver — IddCx-база Microsoft IddSample; Driver/Host IPC ещё не реализован.
- Host 0.4.0 — ADB/session, DXGI capture, CPU NV12 conversion, Media Foundation H.264 и protocol sender.
- Software H264 MFT проверен FFmpeg (Constrained Baseline 1280×800) и планшетом: 150/150 кадров, аппаратный MTK decoder.
- Intel Quick Sync MFT обнаружен; hardware output будет активирован с прямым GPU frame path от IddCx.
- Песочница/служба не имеет права `IDXGIOutput1::DuplicateOutput`; `stream.cmd` запускается как обычное пользовательское приложение.
- INF/CAT ранее проверены штатной Release-сборкой; для установки нужна test-signing конфигурация.
