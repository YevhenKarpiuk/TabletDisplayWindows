# TabletDisplayWindows

Windows-часть TabletDisplay: IddCx/UMDF-драйвер виртуального дисплея и пользовательский host-процесс.

Доступны IddCx-каркас, ADB/TCP session, synthetic video, проверка настоящего Media Foundation encoder и захват основного Windows-дисплея через DXGI Desktop Duplication:

```cmd
build.cmd
test.cmd
install-driver.cmd Release
check-driver.cmd
demo.cmd
encoder-demo.cmd --seconds 5
stream.cmd --seconds 10
```

`stream.cmd` запускайте из обычного интерактивного пользовательского сеанса Windows: Desktop Duplication намеренно недоступен из service/sandbox. Пока IddCx IPC не подключён, передаётся основной физический дисплей, масштабированный до 1280×800.

`install-driver.cmd Release` собирает, test-sign-подписывает и устанавливает IddCx-пакет с запросом UAC. Для отката используйте `uninstall-driver.cmd`; детали Secure Boot/Test Mode описаны в [doc/BUILD.md](doc/BUILD.md).

Подробности: [doc/BUILD.md](doc/BUILD.md) и [doc/IMPLEMENTATION_PLAN.md](doc/IMPLEMENTATION_PLAN.md).
