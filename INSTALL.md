# Установка dantes-inferno-nx 1.0.3 (Dante's Inferno для Nintendo Switch)

dantes-inferno-nx — нативный порт Dante's Inferno (Xbox 360) для Switch в виде homebrew-программы. Готовых файлов не
распространяется: код игры, шейдеры и программа собираются у вас на компьютере **из вашей собственной копии** (диск
Xbox 360 и Title Update 2).

> С разгоном (sys-clk) игра идёт в 60 fps почти везде; на стоковых частотах 40–55 fps, в самых тяжёлых сценах 22–30.

## Что понадобится

| Что | Подробности |
| --- | --- |
| Nintendo Switch | С кастомной прошивкой **Atmosphère** и Homebrew Menu |
| Карта microSD | Свободно **~7 ГБ**, exFAT или FAT32 (самый большой файл игры 3,2 ГБ) |
| Образ вашего диска | Dante's Inferno для Xbox 360, `.iso`, издание **Europe** (En/De/It), Media ID **27C36C6C** |
| Title Update 2 | Для этого издания: `TU_12K226F_000000C000000.00000000000G3` |
| Компьютер | **macOS** (проверено) или Linux (не проверялось), ~10 ГБ свободного места, интернет |

Каждый файл проверяется по контрольной сумме: код собирается из исполняемого файла с TU2, другие издания и обновления
к нему не подходят.

## Шаг 1. Инструменты (один раз)

1. **Инструменты Xcode:** `xcode-select --install`.
2. **Homebrew** (https://brew.sh), затем `brew install cmake ninja python git`.
3. **devkitPro:** установщик со страницы https://github.com/devkitPro/pacman/releases, затем
   `sudo dkp-pacman -S switch-dev deko3d`.
4. **Vulkan SDK** (https://vulkan.lunarg.com): нужны `dxc` и `spirv-val` для шейдеров; на macOS ставится в
   `~/VulkanSDK`.

## Шаг 2. Подготовить файлы игры

```
git clone --recursive --branch v1.0.3 https://github.com/VasilkovND/dantes-inferno-nx
cd dantes-inferno-nx
python3 dante/tools/stage_sd.py --disc "<путь к ISO>" --tu "<путь к TU_12K226F_000000C000000.00000000000G3>" -o sd/dantes-inferno-nx
```

Скрипт проверит обновление и диск и распакует диск в `sd/dantes-inferno-nx/game/` вместе с `default.xexp` из
обновления (несколько минут).

### Если игра установлена на жёсткий диск Xbox 360 (Games on Demand)

Вместо распаковки диска можно положить пакет Games on Demand как есть: содержимое папки
`Content/0000000000000000/454108CF/00007000/` с жёсткого диска Xbox 360 (файл-заголовок и папка `<имя>.data` со
всеми кусками `Data0000`, `Data0001`, ...) — в `sdmc:/switch/dantes-inferno-nx/god/`. Папка `game/` тогда не нужна:
программа сама возьмёт `default.xex` из пакета. В пакете издания 49028C6A исполняемый файл уже обновлён до Title
Update 2, так что обновление не нужно. Папка `00000002` (дополнения) не нужна.

Поддерживаемые издания: Europe (27C36C6C) и 49028C6A — диском с их Title Update 2 или пакетом Games on Demand.

Собрать программу из пакета Games on Demand тоже можно: передайте `build_mac.sh` папку пакета вместо папки
`game/` (`dante/tools/build_mac.sh "<путь>/454108CF/00007000"`), `default.xex` скрипт достанет из него сам.

## Шаг 3. Собрать программу и шейдеры

```
dante/tools/build_mac.sh sd/dantes-inferno-nx/game
```

Первый раз ~30–40 минут и ~4 ГБ места: скрипт скачает проект Hell's Gate (PC-порт, ровно проверенную версию) и
ReXGlue, сгенерирует код игры из вашего `default.xex`, извлечёт и переведёт 1 359 шейдеров и соберёт программу.
Результат:

- `dante/out/sw/dante.nro`
- `dante/out/shaders/dante_shaders.dslib`
- `dante/out/shaders/shadercache_dksh.bin`

## Шаг 4. Скопировать на карту

```
sdmc:/switch/dantes-inferno-nx/
    dantes-inferno-nx.nro    dante/out/sw/dante.nro, переименован
    dante_shaders.dslib      dante/out/shaders/
    shadercache_dksh.bin     dante/out/shaders/
    game/                    sd/dantes-inferno-nx/game/ целиком
```

Логи пишутся в `sdmc:/switch/dantes-inferno-nx/logs/`.

## Запуск

1. Запустите **любую установленную игру, удерживая кнопку R**: так откроется Homebrew Menu с полным доступом к памяти.
   Через альбом (режим апплета) памяти не хватит.
2. Выберите **Dante's Inferno**.

Счётчик FPS: **ZL + ZR + вверх на крестовине**.

Язык игры по умолчанию английский. Для немецкого или итальянского положите рядом с программой файл `dante.toml` со
строкой `user_language = 3` (немецкий) или `user_language = 6` (итальянский); это пока не проверялось.

Если игра покажет шейдер, которого нет в библиотеке, программа сохранит его в `shaders_dump/` рядом с собой: пришлите
эту папку и `logs/` разработчику.
