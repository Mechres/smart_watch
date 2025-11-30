# _Sample project_

(See the README.md file in the upper level 'examples' directory for more information about examples.)

This is the simplest buildable example. The example is used by command `idf.py create-project`
that copies the project to user specified path and set it's name. For more information follow the [docs page](https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html#start-a-new-project)



## How to use example
We encourage the users to use the example as a template for the new projects.
A recommended way is to follow the instructions on a [docs page](https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html#start-a-new-project).

## Example folder contents

The project **sample_project** contains one source file in C language [main.c](main/main.c). The file is located in folder [main](main).

ESP-IDF projects are built using CMake. The project build configuration is contained in `CMakeLists.txt`
files that provide set of directives and instructions describing the project's source files and targets
(executable, library, or both). 

Below is short explanation of remaining files in the project folder.

```
├── CMakeLists.txt
├── main
│   ├── CMakeLists.txt
│   └── main.c
└── README.md                  This is the file you are currently reading
```
Additionally, the sample project contains Makefile and component.mk files, used for the legacy Make based build system.
They are not used or needed when building with CMake and idf.py.

## BLE entegrasyonu

Bu proje artık BLE üzerinden telefonla haberleşmeye hazır. NimBLE tabanlı bir GATT sunucusu açılır ve aşağıdaki özellikler yayınlanır:

- **SmartWatch Service (UUID: 1d8a-503d-e931-369f-9f164b6f-106f-596a-178d)**
  - `Notification RX` (UUID: 2480-757d-4f07-9fa5-0f48-e412-5a9b-dab8): Telefon bildirimlerini saatin alması için Write/Write No Response özelliği. Gönderilen veri `Baslik\nMesaj` veya `Baslik|Mesaj` formatında olmalı. Gövde olmadan gönderirseniz başlık otomatik olarak “Bildirim” olarak kullanılır.
  - `Control RX` (UUID: b31c-b75e-410c-29ba-0b45-9da7-834d-f66e-0c6e): Saatteki özellikleri uzaktan yönetmek için Write/Write No Response. Şu an desteklenen komutlar: `wifi_on`, `wifi_off`, `screen_on`, `screen_off`.

BLE cihaz adı `SmartWatch BLE` olarak yayınlanır. Varsayılan reklam modu bağlanılabilir ve keşfedilebilir; bağlantı koptuğunda otomatik olarak tekrar reklam başlatılır.

Bildirim başlığı, Compact watchface üzerinde `MSG*` satırı olarak gösterilir (`*` okunmamış bildirimi belirtir). Gövde metni arka planda saklanır ve ileride telefon uygulaması ile genişletilebilir.
