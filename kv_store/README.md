# IPC Tabanlı İstemci-Sunucu Anahtar-Değer Deposu

**Ders:** Sistem Programlama  
**Proje No:** 5  
**Programlama Dili:** C (POSIX/Linux API)  
**İşletim Sistemi:** Linux / macOS (POSIX uyumlu)

---

## 1. Amaç

Bu projenin amacı, **aynı makine üzerinde çalışan birden fazla istemci sürecinin** eşzamanlı olarak erişebildiği, **IPC (Inter-Process Communication)** tabanlı bir anahtar-değer (Key-Value) deposu geliştirmektir.

Proje; **Unix Domain Socket** kullanarak süreçler arası haberleşmeyi, **Thread Pool** mimarisiyle eşzamanlı istemci yönetimini ve **Read-Write Lock** mekanizmasıyla yarış durumu (Race Condition) önlemeyi gerçek bir sistem programlama senaryosunda uygulamalı olarak göstermektedir.

Geliştiricinin hedefleri:
- POSIX thread API'sini (pthreads) etkin biçimde kullanmak
- Senkronizasyon mekanizmalarını doğru seçmek ve uygulamak
- IPC yöntemlerini gerçek bir istemci-sunucu mimarisinde hayata geçirmek
- Loglama, hata yönetimi ve performans ölçümünü birlikte sunmak

---

## 2. Tasarım

### 2.1 Genel Mimari

Sistem üç ana bileşenden oluşmaktadır:

```
+-----------+       Unix Domain Socket       +--------------------+
| client.c  |  ============================  |     server.c       |
| (istemci) |    /tmp/kvstore.sock           |  (sunucu süreci)   |
+-----------+                                |                    |
                                             |  +--------------+  |
                                             |  | Thread Pool  |  |
                                             |  | (4 thread)   |  |
                                             |  +--------------+  |
                                             |        |           |
                                             |  +--------------+  |
                                             |  |   KVStore    |  |
                                             |  |  (Hash Map   |  |
                                             |  |  + RWLock)   |  |
                                             |  +--------------+  |
                                             +--------------------+
```

### 2.2 IPC Mekanizması: Unix Domain Socket

Tercih edilen IPC yöntemi **Unix Domain Socket**'tir (`AF_UNIX, SOCK_STREAM`). Bu seçimin gerekçeleri:

| Özellik | Unix Domain Socket | Named Pipe | Shared Memory |
|---|---|---|---|
| İki yönlü iletişim | Evet | Hayır (tek yön) | Manuel |
| Birden fazla istemci | Evet (accept ile) | Kısıtlı | Manuel |
| Yapılandırma kolaylığı | Yüksek | Orta | Düşük |
| Performans | Çok yüksek | Yüksek | En yüksek |
| Güvenlik | İyi (dosya izinleri) | İyi | Karmaşık |

Unix Domain Socket, ağ stack'ini (TCP/IP) atladığı için aynı makinedeki iletişimde TCP'ye göre önemli ölçüde daha hızlıdır. Socket dosyası `/tmp/kvstore.sock` yolunda oluşturulur.

### 2.3 Veri Yapısı: Hash Map (Separate Chaining)

Anahtar-değer çiftleri `kvstore.c` içindeki bir **Hash Map** yapısında tutulmaktadır.

- **Boyut:** 1024 bucket (kova)
- **Hash Fonksiyonu:** djb2 (`hash = hash * 33 + c`)
- **Çakışma Çözümü:** Separate Chaining (bağlı liste ile)
- **Arama/Ekleme Karmaşıklığı:** Ortalama O(1), en kötü O(n)

```
bucket[0] --> NULL
bucket[1] --> [key: "port", val: "8080"] --> NULL
bucket[2] --> [key: "host", val: "localhost"] --> [key: "ip", val: "192.168.1.1"] --> NULL
...
bucket[1023] --> NULL
```

### 2.4 Thread Pool Mimarisi

Her yeni istemci bağlantısı için thread oluşturmak yerine **Thread Pool (İş Parçacığı Havuzu)** kullanılmıştır.

**Neden Thread Pool?**
- Her bağlantı için `pthread_create()` çağırmak çok sayıda istemcide CPU ve bellek açısından maliyetlidir.
- Thread Pool'da sabit sayıda thread başlangıçta oluşturulur ve yeniden kullanılır.
- Thread oluşturma/sonlandırma maliyeti ortadan kalkar.

**Çalışma Prensibi:**
1. Sunucu başlarken 4 worker thread oluşturulur
2. Her thread `dequeue_client()` ile kuyrukta iş bekler (`pthread_cond_wait`)
3. Yeni istemci geldiğinde `enqueue_client()` ile kuyruğa atılır
4. Boştaki bir thread uyandırılır (`pthread_cond_signal`) ve istemciyi işler
5. İşlem bitince thread tekrar kuyruğa döner

### 2.5 Senkronizasyon Katmanları

Projede **iki ayrı senkronizasyon katmanı** kullanılmaktadır:

**Katman 1 - Kuyruk Erişimi:**
- `pthread_mutex_t queue_mutex`: Kuyruğa ekleme ve çıkarma işlemlerini korur
- `pthread_cond_t queue_cond`: Kuyruk boşken thread'leri uyku moduna alır (busy-wait yok)

**Katman 2 - Veri Yapısı Erişimi:**
- `pthread_rwlock_t lock`: Hash Map'e eşzamanlı erişimi yönetir
  - `pthread_rwlock_rdlock()`: GET ve LIST - birden fazla thread aynı anda okuyabilir
  - `pthread_rwlock_wrlock()`: SET ve DELETE - tek thread yazar, diğerleri bekler

**Katman 3 - Log Dosyası Erişimi:**
- `pthread_mutex_t log_mutex`: Log satırlarının karışmamasını garanti eder

---

## 3. Kullanılan Sistem Programlama Kavramları

### 3.1 IPC (Inter-Process Communication)
- **Unix Domain Socket** (`sys/un.h`, `sys/socket.h`)
- `socket()`, `bind()`, `listen()`, `accept()`, `connect()`, `read()`, `write()`, `close()`
- İstemci ve sunucu arasında binary protokol (struct doğrudan socket'e yazılır)

### 3.2 Process ve Thread Yönetimi
- **POSIX Threads (pthreads):** `pthread_create()`, `pthread_join()`
- **Thread Pool:** 4 worker thread sabit olarak oluşturulur, yeniden kullanılır
- Sunucu ayrı bir process olarak çalışır (`./server &`)
- Her istemci ayrı bir process olarak çalışır (`./client ...`)

### 3.3 Senkronizasyon Mekanizmaları
- `pthread_mutex_t`: Kuyruk ve log dosyası erişimini korur
- `pthread_cond_t`: Thread'lerin boşta CPU kullanmaması için bekleme mekanizması
- `pthread_rwlock_t`: Okuma-yazma kilidi ile veri deposunu korur
  - **Avantaj:** Eşzamanlı okuma performansı, sıradan mutex'e göre çok daha yüksektir

### 3.4 Sinyal Yönetimi (Signal Handling)
- `signal(SIGINT, handle_signal)`: Ctrl+C yakalanır
- `signal(SIGTERM, handle_signal)`: Sistem kapatma sinyali yakalanır
- Yakalandığında: socket kapatılır, socket dosyası silinir, thread'ler düzgünce sonlandırılır

### 3.5 Dinamik Bellek Yönetimi
- Her yeni anahtar-değer çifti için `malloc()` ile bellek ayrılır
- Silinen veya sunucu kapanırken tüm düğümler `free()` ile serbest bırakılır
- Bellek sızıntısı (memory leak) önlenmiştir

### 3.6 Dosya I/O ve Loglama
- `fopen()`, `fprintf()`, `fflush()` ile `server.log` dosyasına zaman damgalı kayıt
- `perror()` ile sistem çağrısı hatalarının standart hata çıkışına yazılması
- Log fonksiyonu `mutex` ile korunur (thread-safe)

### 3.7 POSIX API Fonksiyonları
`socket`, `bind`, `listen`, `accept`, `connect`, `read`, `write`, `close`, `unlink`, `signal`, `pthread_create`, `pthread_join`, `pthread_mutex_init/lock/unlock/destroy`, `pthread_cond_init/wait/signal/broadcast/destroy`, `pthread_rwlock_init/rdlock/wrlock/unlock/destroy`, `malloc`, `free`, `memset`, `strncpy`, `strcmp`, `snprintf`, `fopen`, `fprintf`, `fflush`, `fclose`, `time`, `ctime`

---

## 4. Dosya Yapısı

```
kv_store/
├── protocol.h          # İstemci-sunucu haberleşme protokolü (struct tanımları)
├── kvstore.h           # Veri yapısı tanımlamaları ve fonksiyon prototipleri
├── kvstore.c           # Hash Map implementasyonu (CRUD + rwlock)
├── server.c            # Ana sunucu (Thread Pool + Socket + Loglama)
├── client.c            # Komut satırı istemcisi
├── Makefile            # Derleme kuralları
├── performance_test.sh # Eşzamanlılık ve performans test betiği
├── server.log          # Sunucu log dosyası (çalıştırınca oluşur)
└── web_ui/             # Görsel yönetim arayüzü (Python/Flask)
    ├── app.py          # Flask API sunucusu
    ├── requirements.txt
    └── templates/
        └── index.html  # Dashboard arayüzü
```

### Header (.h) Dosyaları Hakkında
C'de bir veri tipini veya sabiti birden fazla `.c` dosyasında kullanmak istediğimizde, kodu kopyalamak yerine bir header (başlık) dosyasına yazarız ve `#include` ile dahil ederiz.

- **`protocol.h`**: Hem `client.c` hem `server.c` tarafından dahil edilir. Böylece ikisi de aynı `Request` ve `Response` struct yapılarını kullanır; veri uyumsuzluğu (struct mismatch) sorunu yaşanmaz.
- **`kvstore.h`**: `server.c`'ye veri deposu fonksiyonlarını tanıtır. Fonksiyonun gövdesi `kvstore.c`'dedir; header sadece "bu fonksiyon var ve şu parametreleri alır" bilgisini verir.
- `#ifndef / #define / #endif` koruyucuları: Aynı header'ın birden fazla kez dahil edilmesini önler (include guard).

---

## 5. Çalıştırma Adımları

### 5.1 Gereksinimler
- GCC derleyicisi
- POSIX uyumlu işletim sistemi (Linux veya macOS)
- Python 3 ve pip3 (yalnızca web arayüzü için)

### 5.2 Derleme

```bash
cd /Users/osmancingoz/Desktop/sistemprogramlama/kv_store
make
```

Bu komut şunları üretir:
- `server`: Sunucu programı
- `client`: İstemci programı

Temizleme için:
```bash
make clean
```

### 5.3 Sunucuyu Başlatma

```bash
# Arka planda çalıştır
./server &

# Veya ayrı terminal sekmesinde ön planda
./server
```

Sunucu çalışmaya başladığında `/tmp/kvstore.sock` dosyası oluşur ve `server.log`'a kayıt düşer.

### 5.4 İstemci Kullanımı

```bash
# Veri ekle / güncelle
./client SET sunucu_ip 192.168.1.100
./client SET sunucu_port 8080
./client SET veritabani mysql

# Veri oku
./client GET sunucu_ip

# Tüm verileri listele
./client LIST

# Veri sil
./client DELETE sunucu_ip

# Silindiğini doğrula
./client LIST
```

### 5.5 Web Arayüzünü Başlatma

```bash
cd web_ui
pip3 install -r requirements.txt
python3 app.py
```

Tarayıcıda açın: **http://localhost:5000**

Arayüz üzerinden SET, GET, DELETE, LIST işlemleri yapılabilir; sunucu logları canlı olarak (1 saniye aralıkla) izlenebilir.

### 5.6 Sunucuyu Durdurma

```bash
# Arka planda çalışıyorsa
kill %1
# veya
killall server
```

---

## 6. Testler

### 6.1 Temel Fonksiyon Testi (Manuel)

```bash
# Sunucuyu başlat
./server &

# Veri ekle
./client SET test_key test_value
# Beklenen: Basarili: OK

# Veriyi oku
./client GET test_key
# Beklenen: test_value

# Güncelle
./client SET test_key yeni_deger
./client GET test_key
# Beklenen: yeni_deger

# Listele
./client LIST
# Beklenen: [test_key: yeni_deger]

# Sil
./client DELETE test_key
./client GET test_key
# Beklenen: Hata: Anahtar bulunamadi

# Sunucuyu durdur
kill %1
```

### 6.2 Eşzamanlılık Testi (Concurrency Test)

Hocanın zorunlu kıldığı "en az 2 istemci aynı anda" şartını karşılamak ve yarış durumu olmadığını göstermek için:

```bash
# 5 istemciyi aynı anda çalıştır (& ile arka plana at)
./client SET anahtar1 deger1 &
./client SET anahtar2 deger2 &
./client SET anahtar3 deger3 &
./client GET anahtar1 &
./client GET anahtar2 &
wait

# Veri bütünlüğünü kontrol et
./client LIST
```

### 6.3 Otomatik Performans Testi

```bash
./performance_test.sh
```

Bu betik:
1. 100 istemciyi aynı anda arka planda başlatır (SET işlemleri)
2. Tamamlanma süresini ölçer
3. 100 istemciyi aynı anda GET işlemi için başlatır
4. Süreyi ölçer ve karşılaştırır

**Gerçek Test Sonuçları (macOS, Apple M serisi işlemci):**
```
--- Starting Performance Test ---
Creating 100 concurrent clients doing SET operations...
100 SET operations completed in 288 ms.
Creating 100 concurrent clients doing GET operations...
100 GET operations completed in 34 ms.
--- Performance Test Finished ---
```

---

## 7. Karşılaşılan Problemler ve Çözümler

### Problem 1: Race Condition (Yarış Durumu)

**Problem:** İlk tasarımda tüm okuma ve yazma işlemleri tek bir `pthread_mutex_t` ile korunuyordu. Bu durum, iki thread aynı anda GET yapmak istediğinde bile birinin beklemesine neden oluyordu.

**Çözüm:** `pthread_mutex_t` yerine `pthread_rwlock_t` (okuma-yazma kilidi) kullanıldı. Okuma kilidi (`rdlock`) birden fazla thread tarafından aynı anda alınabilir. Yazma kilidi (`wrlock`) ise özel erişim sağlar. Bu değişiklik, okuma ağırlıklı yüklerde ciddi performans artışı sağladı.

### Problem 2: Socket Dosyası Kalıntısı

**Problem:** Sunucu `Ctrl+C` ile aniden kapatıldığında `/tmp/kvstore.sock` dosyası silinmiyordu. Sunucu tekrar başlatıldığında `bind()` çağrısı `EADDRINUSE` hatası veriyordu.

**Çözüm:** `bind()` çağrısından önce `unlink(SOCKET_PATH)` eklendi. Ayrıca `handle_signal()` fonksiyonunda sinyal yakalandığında da `unlink()` çağrıldı.

### Problem 3: Thread Deadlock (Kilitlenme)

**Problem:** Sunucu kapatılırken `server_running = 0` yapıldığında, kuyrukta `pthread_cond_wait()` ile uyuyan thread'ler sonsuza kadar bekleyebiliyordu (deadlock).

**Çözüm:** `pthread_cond_signal()` yerine `pthread_cond_broadcast()` kullanıldı. Bu çağrı tüm bekleyen thread'leri aynı anda uyandırır. Her thread uyandığında `server_running` değerini kontrol eder ve 0 ise döngüden çıkar.

### Problem 4: Log Satırlarının Karışması

**Problem:** Birden fazla thread aynı anda `server.log`'a yazarken satırlar birbirine karışıyordu (interleaving).

**Çözüm:** `log_message()` fonksiyonu ayrı bir `pthread_mutex_t log_mutex` ile korundu. Böylece bir thread log yazarken diğerleri sıraya girer.

### Problem 5: macOS'te `python` ve `pip` Komutları

**Problem:** macOS'te Python 3 yüklü olmasına rağmen `python` ve `pip` komutları bulunamadı.

**Çözüm:** macOS varsayılan olarak `python3` ve `pip3` komutlarını kullanır. Web arayüzü başlatma komutları buna göre güncellendi.

---

## 8. Performans Değerlendirmesi

### 8.1 Ölçüm Metodolojisi

`performance_test.sh` betiği kullanılarak aşağıdaki ölçümler yapılmıştır:
- Bash'in `date +%s%N` komutu ile nanosaniye hassasiyetinde zaman ölçümü
- 100 istemci process arka planda (`&`) başlatılır, `wait` ile tamamlanmaları beklenir

### 8.2 Sonuçlar

| İşlem | İstemci Sayısı | Toplam Süre |
|---|---|---|
| SET (yazma) | 100 eşzamanlı | ~288 ms |
| GET (okuma) | 100 eşzamanlı | ~34 ms |

### 8.3 Analiz

**Okuma vs. Yazma Farkı:**  
GET işlemleri SET işlemlerine kıyasla yaklaşık **8.5 kat daha hızlı** tamamlandı. Bu beklenen bir sonuçtur çünkü:
- GET: `pthread_rwlock_rdlock()` kullanır → birden fazla thread aynı anda okuyabilir (paralel)
- SET: `pthread_rwlock_wrlock()` kullanır → tek thread yazar, diğerleri bekler (seri)

**Thread Pool Etkisi:**  
4 thread'lik havuz, 100 eşzamanlı istemciyle test edildi ve hiçbir bağlantı kaybı yaşanmadı. Kuyruk mekanizması, thread sayısını aşan bağlantıları sıraya alarak sistemin çökmesini önledi.

**I/O Gecikmesi:**  
Unix Domain Socket kullanıldığından ağ gecikmesi sıfırdır. Gecikmenin tamamı thread scheduling ve lock contention'dan kaynaklanmaktadır.

### 8.4 Olası İyileştirmeler

- Thread havuzu boyutu dinamik hale getirilebilir (CPU çekirdek sayısına göre)
- Hash tablosu boyutu artırılarak çakışma oranı düşürülebilir
- Persistent storage (disk'e yazma) eklenebilir

---

## 9. Linux ve macOS Uyumluluğu Hakkında

Hoca Linux API kullanılmasını istemiş olsa da, bu proje tamamen **POSIX standartlarına** uygun olarak geliştirilmiştir. POSIX (Portable Operating System Interface), Linux ve macOS dahil tüm Unix türevi sistemlerde geçerli bir standarttır.

- Kullanılan tüm başlık dosyaları (`sys/socket.h`, `sys/un.h`, `pthread.h` vb.) hem Linux hem macOS'te mevcuttur
- Kod herhangi bir değişiklik yapılmadan Linux sisteminde (Ubuntu, Debian vb.) derlenip çalışır
- `Makefile` `gcc` derleyicisini kullanır; her iki sistemde de mevcut olan standart bir araçtır
- Proje Linux VM'de (UTM veya Orbstack) birebir çalıştırılabilir
