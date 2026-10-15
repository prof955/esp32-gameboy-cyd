# ESPboy GameBoy — Arduino IDE edition for CYD

Bu, PlatformIO portundan ayrı Arduino IDE sketch'idir; mevcut PlatformIO projesi `CYD_GameBoy_Port/` içinde korunmuştur. Ekran katmanı, verdiğiniz çalışan CYD örneğindeki **Arduino_GFX + Arduino_ESP32SPI (HSPI, 40 MHz)** bağlantısını kullanır. Emülatör ana döngüde çalışırken özel görüntü görevi diğer ESP32 çekirdeğinde LCD'ye gönderim yapar; iki küçük gölge çerçeve belleği ve tek yuvalı kuyruk, eski görüntüleri biriktirmek yerine en yeni kareyi sunar.

## Arduino IDE kurulumu

1. Boards Manager'dan Espressif **ESP32** kart paketinin **2.0.17** sürümünü kurun. Ses DAC ve donanım timer kodu bu core sürümüne göre hazırlanmıştır.
2. **Tools → Board → ESP32 Arduino** altında `ESP32 Dev Module` seçin; 4 MB flash ve varsayılan partition düzenini kullanın.
3. Library Manager'dan şu kütüphaneleri kurun:
   - **GFX Library for Arduino** (`Arduino_GFX_Library.h`) — **1.4.9** sürümünü seçin. Bu sketch bu sürüm ve ESP32 core 2.0.17 ile derleme kontrolünden geçti; daha yeni Arduino_GFX sürümleri daha yeni ESP32 core gerektirebilir.
   - **XPT2046_Touchscreen** by Paul Stoffregen.
4. `CYD_GameBoy_Arduino.ino` dosyasını Arduino IDE'de açın, kart/portu seçin, **Verify** ardından **Upload** yapın.
5. İsteğe bağlı Serial Monitor: `115200` baud. Oyun çalışırken yaklaşık her 5 saniyede emülasyon/LCD FPS'i, düşürülen kareleri, DAC'ın ürettiği sessiz-olmayan örnek sayısını (`audioActive`) ve boş heap'i yazar.

## microSD ve ROM'lar

FAT32 microSD karta `/ROMS/` klasörünü oluşturup kendi kullanma hakkınız olan `.gb` ROM dosyalarını koyun. Oyun kayıtları `/SAVES/` içine yazılır. Ticari ROM dosyaları kaynak paketine dahil değildir.

Açılışta ROM listesinden oyuna dokunun ve **PLAY**'e basın. Ekran portre yönündedir: 160×144 Game Boy görüntüsü 240×216 alana 3:2 ölçeklenir; alt bölüm dokunmatik tuşlara ayrılmıştır. Tuşlar latch davranışındadır: bir kez dokununca basılı kalır, tekrar dokununca bırakılır. Böylece tek dokunuşlu panelde yön ve A/B birlikte kullanılabilir. `CLR` tüm tuşları bırakır; üst bölümdeki `PAL`, `SND` ve `MENU` palet, ses ve ROM menüsünü kontrol eder.

Emülasyon Game Boy'un yaklaşık **59,73 Hz** kare temposuna göre çalışır. LCD aktarımı geride kalırsa kuyrukta bekleyen eski ekran görüntüsü atılabilir; emülatör eski kareleri oynatarak birikme yapmaz. Gerçek karttaki FPS, ROM ve donanım/kitaplık sürümüne bağlıdır; bu workspace'te fiziksel CYD üzerinde ölçüm yapılmamıştır.

## Donanım eşlemesi

Aşağıdaki pinler verdiğiniz çalışan ESP32-2432S028R örneğindeki bağlantılardır:

- ILI9341: SCK 14, MOSI 13, MISO 12, CS 15, DC 2, arka ışık 21; ayrılmış HSPI, 40 MHz
- XPT2046: SCK 25, MOSI 32, MISO 39, CS 33, IRQ 36
- microSD: SCK 18, MOSI 23, MISO 19, CS 5
- DAC / hoparlör amfisi: GPIO 26 (CYD P4; P4'e uygun hoparlör bağlı olmalı)
- Bu kart revizyonunda amplifikatör enable: GPIO 4, **LOW = açık / HIGH = sessiz**

Peanut-GB, APU register callback'lerine `0xFFxx` adresi verirken ses kodu `0x10..0x3F` offset'i bekliyordu. Arduino sürümünde bu adres eşleşmesi düzeltildi; önceki durumda APU hiç açılmadığından DAC sürekli sessizlik üretiyordu.

TFT, Arduino_GFX'in açıkça HSPI'ye atanmış `Arduino_ESP32SPI` veri yolundadır. Dokunmatik ve SD, `SPI`/VSPI'yi sırayla paylaşır; SD erişiminden sonra VSPI dokunmatik pinlerine geri alınır. XPT2046 ham dokunma kalibrasyonu, verdiğiniz örnekteki `560..3720` ve `590..3580` aralıklarından alınmıştır; portre yönü için X/Y dönüşümü kodda uygulanır.

## Sınırlar

Peanut-GB DMG Game Boy emülasyonudur; Game Boy Color modu ve bazı özel mapper'lar desteklenmeyebilir. Derleme kontrolü başarılıdır; **fiziksel CYD'de oyun/FPS ve dokunmatik davranışı henüz doğrulanmamıştır**. Kaynak lisansı `LICENSE` dosyasındadır.
