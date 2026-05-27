/*
 * server.c - Çok İş Parçacıklı IPC Anahtar-Değer Deposu Sunucusu
 *
 * Bu dosya projenin ana sunucusudur. Tek bir process olarak sürekli çalışır
 * ve birden fazla istemciden gelen eşzamanlı istekleri yönetir.
 *
 * Kullanılan Sistem Programlama Kavramları:
 *
 *   1. IPC (Inter-Process Communication):
 *      Unix Domain Socket kullanılır (/tmp/kvstore.sock).
 *      Aynı makine üzerindeki processler arası iletişim için en hızlı yöntemdir.
 *      TCP/IP'ye kıyasla ağ katmanından geçmediğinden çok daha hızlıdır.
 *
 *   2. Thread Pool (İş Parçacığı Havuzu):
 *      Sunucu başlarken THREAD_POOL_SIZE kadar thread oluşturulur.
 *      Her yeni istemci bağlantısı bir kuyruğa (queue) atılır.
 *      Boşta bekleyen thread'ler kuyruktan iş çekerek istemciyi işler.
 *      Bu yaklaşım, her bağlantı için thread oluşturmaktan çok daha verimlidir.
 *
 *   3. Senkronizasyon (Synchronization):
 *      - queue_mutex + queue_cond: Kuyruk erişimini güvenli hale getirir.
 *        Kuyruk boşken thread'ler uyku moduna geçer (busy wait yapmaz).
 *      - log_mutex: Log dosyasına eş zamanlı yazımı korur.
 *      - KVStore'daki rwlock: Veri yapısını okuma/yazma çakışmalarından korur.
 *
 *   4. Signal Handling:
 *      SIGINT (Ctrl+C) ve SIGTERM sinyalleri yakalanarak sunucu düzgünce kapatılır.
 *      Socket dosyası (/tmp/kvstore.sock) temizlenir, thread'ler beklenir.
 *
 *   5. Loglama:
 *      Tüm işlemler zaman damgasıyla server.log dosyasına yazılır.
 *      Log yazma mutex ile korunur (thread-safe logging).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include "kvstore.h"
#include "protocol.h"

/* ========== Sabitler ========== */
#define THREAD_POOL_SIZE 4   /* Havuzdaki iş parçacığı sayısı */
#define QUEUE_SIZE       100  /* Aynı anda bekleyebilecek maksimum istemci sayısı */

/* ========== Thread Pool Kuyruğu ========== */
/* Döngüsel dizi (circular buffer) ile kuyruk implementasyonu */
int client_queue[QUEUE_SIZE]; /* Bekleyen istemci socket tanımlayıcıları */
int queue_front = 0;          /* Kuyruktan çıkarma indeksi */
int queue_rear  = 0;          /* Kuyruğa ekleme indeksi */
int queue_count = 0;          /* Kuyruktaki eleman sayısı */

/* Kuyruk erişimini koruyacak mutex ve thread uyandırma için condition variable */
pthread_mutex_t queue_mutex;
pthread_cond_t  queue_cond;

/* Thread havuzunu tutan dizi */
pthread_t thread_pool[THREAD_POOL_SIZE];

/* ========== Global Değişkenler ========== */
volatile int server_running = 1; /* Sunucunun çalışmaya devam edip etmeyeceği bayrağı */
KVStore store;                   /* Paylaşılan anahtar-değer deposu */
int server_fd;                   /* Sunucu socket tanımlayıcısı */

/* ========== Loglama ========== */
FILE*           log_file;  /* Log dosyası işaretçisi */
pthread_mutex_t log_mutex; /* Log yazımını thread-safe hale getirir */

/*
 * log_message() - Zaman Damgalı Log Yazma Fonksiyonu
 *
 * printf() gibi değişken sayıda argüman alır (variadic function).
 * Tüm thread'ler bu fonksiyonu kullanabilir; mutex sayesinde log satırları
 * birbirine karışmaz (interleaving olmaz).
 *
 * Kullanım: log_message("INFO", "Istemci baglandi: %d", client_fd);
 */
void log_message(const char* level, const char* format, ...) {
    pthread_mutex_lock(&log_mutex); /* Log dosyasına özel erişim al */

    if (log_file) {
        /* Geçerli zamanı al ve formatla */
        time_t now;
        time(&now);
        char* tarih = ctime(&now);
        tarih[strlen(tarih) - 1] = '\0'; /* ctime() sondaki \n karakterini kaldır */

        fprintf(log_file, "[%s] [%s] ", tarih, level);

        /* Değişken argümanları işle (printf benzeri) */
        va_list args;
        va_start(args, format);
        vfprintf(log_file, format, args);
        va_end(args);

        fprintf(log_file, "\n");
        fflush(log_file); /* Tamponu anında diske yaz, crash anında kayıp olmasın */
    }

    pthread_mutex_unlock(&log_mutex);
}

/*
 * enqueue_client() - İstemci Soketini Kuyruğa Ekle
 *
 * accept() ile yeni bir istemci bağlantısı alındığında bu fonksiyon çağrılır.
 * Kuyruk doluysa bağlantıyı kabul etmeyip kapatır (load shedding).
 * pthread_cond_signal() ile kuyrukta bekleyen bir thread uyandırılır.
 */
void enqueue_client(int client_socket) {
    pthread_mutex_lock(&queue_mutex);

    if (queue_count < QUEUE_SIZE) {
        /* Döngüsel tampon mantığıyla kuyruğa ekle */
        client_queue[queue_rear] = client_socket;
        queue_rear = (queue_rear + 1) % QUEUE_SIZE;
        queue_count++;
        /* Bekleyen bir worker thread'i uyandır */
        pthread_cond_signal(&queue_cond);
    } else {
        /* Kuyruk dolu: bağlantıyı reddet */
        log_message("WARNING", "Istemci kuyrugu dolu, baglanti reddedildi.");
        close(client_socket);
      }

    pthread_mutex_unlock(&queue_mutex);
}

/*
 * dequeue_client() - Kuyruktan İstemci Soketi Çıkar
 *
 * Worker thread'ler tarafından çağrılır. Kuyruk boşsa pthread_cond_wait() ile
 * uyku moduna geçer (CPU'yu bloke etmez, verimli bekleme).
 * Sunucu kapanınca (server_running=0) tüm thread'ler uyandırılır.
 *
 * Dönüş: Geçerli socket fd veya -1 (sunucu kapatılıyor)
 */
int dequeue_client() {
    pthread_mutex_lock(&queue_mutex);

    /* Kuyruk boşsa bekle; sunucu kapanıyorsa çık */
    while (queue_count == 0 && server_running) {
        pthread_cond_wait(&queue_cond, &queue_mutex);
    }

    if (!server_running) {
        pthread_mutex_unlock(&queue_mutex);
        return -1; /* Sunucu kapatılıyor, thread'i sonlandır */
    }

    /* Döngüsel tampon mantığıyla kuyruktan çıkar */
    int client_socket = client_queue[queue_front];
    queue_front = (queue_front + 1) % QUEUE_SIZE;
    queue_count--;

    pthread_mutex_unlock(&queue_mutex);
    return client_socket;
}

/*
 * handle_client() - Bir İstemcinin Tüm İsteklerini İşle
 *
 * Bu fonksiyon bir thread tarafından çağrılır ve istemci bağlantısını
 * sonlanana kadar tutar. İstemci birden fazla komut gönderebilir.
 *
 * read() çağrısı 0 veya negatif döndürdüğünde (bağlantı kapandı) döngüden çıkılır.
 */
void handle_client(int client_socket) {
    Request  req;
    Response res;

    /* İstemci bağlantısı açık kaldığı sürece komutları işle */
    while (read(client_socket, &req, sizeof(Request)) > 0) {
        memset(&res, 0, sizeof(Response)); /* Cevap tamponunu temizle */

        if (req.type == CMD_SET) {
            /* SET: Anahtar-değer çiftini depoya ekle veya güncelle */
            if (kv_set(&store, req.key, req.value) == 0) {
                res.status = 0;
                strcpy(res.message, "OK");
                log_message("INFO", "SET '%s' = '%s'", req.key, req.value);
            } else {
                res.status = -1;
                strcpy(res.message, "Hata: SET islemi basarisiz");
                log_message("ERROR", "SET '%s' basarisiz (bellek hatasi?)", req.key);
            }

        } else if (req.type == CMD_GET) {
            /* GET: Anahtara karşılık gelen değeri getir */
            char val[MAX_VAL_LEN];
            if (kv_get(&store, req.key, val) == 0) {
                res.status = 0;
                strcpy(res.message, val);
                log_message("INFO", "GET '%s' -> '%s' bulundu", req.key, val);
            } else {
                res.status = -1;
                strcpy(res.message, "Hata: Anahtar bulunamadi");
                log_message("INFO", "GET '%s' -> bulunamadi", req.key);
            }

        } else if (req.type == CMD_DELETE) {
            /* DELETE: Anahtarı ve değerini sil */
            if (kv_delete(&store, req.key) == 0) {
                res.status = 0;
                strcpy(res.message, "Silindi");
                log_message("INFO", "DELETE '%s' -> basarili", req.key);
            } else {
                res.status = -1;
                strcpy(res.message, "Hata: Anahtar bulunamadi");
                log_message("INFO", "DELETE '%s' -> bulunamadi", req.key);
            }

        } else if (req.type == CMD_LIST) {
            /* LIST: Depodaki tüm anahtar-değer çiftlerini döndür */
            char liste[MAX_MSG_LEN];
            kv_list(&store, liste, sizeof(liste));
            res.status = 0;
            strncpy(res.message, liste, MAX_MSG_LEN - 1);
            res.message[MAX_MSG_LEN - 1] = '\0';
            log_message("INFO", "LIST komutu calistirildi");

        } else {
            /* Bilinmeyen komut tipi */
            res.status = -1;
            strcpy(res.message, "Hata: Bilinmeyen komut");
            log_message("WARNING", "Bilinmeyen komut tipi alindi: %d", req.type);
        }

        /* Cevabı istemciye gönder */
        if (write(client_socket, &res, sizeof(Response)) == -1) {
            log_message("ERROR", "Cevap gonderme hatasi: istemci baglantisi kesilmis olabilir");
            break;
        }
    }

    close(client_socket);
    log_message("INFO", "Istemci baglantisi kesildi");
}

/*
 * worker_thread() - Thread Havuzundaki İşçi Thread Fonksiyonu
 *
 * Her thread bu fonksiyon ile başlar. Sonsuz döngüde kuyruktan iş çeker.
 * Sunucu kapanınca dequeue_client() -1 döndürür ve thread sonlanır.
 */
void* worker_thread(void* arg) {
    (void)arg; /* Kullanılmayan parametre uyarısını bastır */

    while (server_running) {
        /* Kuyrukta iş var mı bekle, varsa al */
        int client_socket = dequeue_client();
        if (client_socket != -1) {
            handle_client(client_socket);
        }
    }
    return NULL;
}

/*
 * handle_signal() - SIGINT/SIGTERM Sinyal İşleyicisi
 *
 * Kullanıcı Ctrl+C'ye bastığında veya sistem shutdown sinyali gönderdiğinde
 * bu fonksiyon çağrılır. Sunucuyu düzgünce (gracefully) kapatır:
 *   1. server_running bayrağını 0 yapar
 *   2. Bekleyen tüm thread'leri uyandırır (pthread_cond_broadcast)
 *   3. Server socket'i kapatır
 *   4. Socket dosyasını siler (unlink)
 */
void handle_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        log_message("INFO", "Kapatma sinyali alindi (signal: %d), sunucu kapatiliyor...", sig);
        server_running = 0;
        /* Kuyrukta uyuyan tüm thread'leri uyandır ki sonlanabilsinler */
        pthread_cond_broadcast(&queue_cond);
        close(server_fd);
        unlink(SOCKET_PATH); /* /tmp/kvstore.sock dosyasını sil */
    }
}

/*
 * main() - Sunucunun Başlangıç Noktası
 *
 * Sırasıyla şunları yapar:
 *   1. Sinyal işleyicilerini kaydet
 *   2. Log dosyasını aç
 *   3. KVStore'u başlat
 *   4. Thread havuzunu oluştur
 *   5. Unix Domain Socket'i oluştur, bind et, dinle
 *   6. accept() döngüsüne gir (yeni istemcileri kuyruğa at)
 *   7. Kapanışta thread'leri bekle ve kaynakları temizle
 */
int main() {
    /* Sinyal işleyicilerini kaydet */
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN); /* İstemci aniden koparsa sunucunun çökmesini engelle */

    /* Log dosyasını "append" modunda aç (önceki loglar korunur) */
    log_file = fopen("server.log", "a");
    if (!log_file) {
        perror("[KRITIK] Log dosyasi acilamadi");
        exit(EXIT_FAILURE);
    }
    pthread_mutex_init(&log_mutex, NULL);
    log_message("INFO", "========== Sunucu baslatiliyor ==========");

    /* Veri deposunu başlat */
    kv_init(&store);

    /* Kuyruk mutex ve condition variable'ı başlat */
    pthread_mutex_init(&queue_mutex, NULL);
    pthread_cond_init(&queue_cond, NULL);

    /* Thread havuzunu oluştur */
    log_message("INFO", "%d worker thread olusturuluyor...", THREAD_POOL_SIZE);
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&thread_pool[i], NULL, worker_thread, NULL) != 0) {
            log_message("ERROR", "Thread %d olusturulamadi", i);
            perror("[HATA] pthread_create");
            exit(EXIT_FAILURE);
        }
    }

    /* Unix Domain Socket oluştur (AF_UNIX = yerel, SOCK_STREAM = TCP benzeri güvenilir) */
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("[KRITIK] Socket olusturulamadi");
        log_message("ERROR", "socket() cagrisı basarisiz");
        exit(EXIT_FAILURE);
    }

    /* Socket adres yapısını hazırla */
    struct sockaddr_un server_addr;
    memset(&server_addr, 0, sizeof(struct sockaddr_un));
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, SOCKET_PATH, sizeof(server_addr.sun_path) - 1);

    /* Varsa eski socket dosyasını sil (önceki çalışmadan kalma olabilir) */
    unlink(SOCKET_PATH);

    /* Socket'i belirlenen yola bağla */
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(struct sockaddr_un)) == -1) {
        perror("[KRITIK] Bind basarisiz");
        log_message("ERROR", "bind() basarisiz: %s", SOCKET_PATH);
        exit(EXIT_FAILURE);
    }

    /* Dinleme moduna geç (50 bağlantı bekleyebilir) */
    if (listen(server_fd, 50) == -1) {
        perror("[KRITIK] Listen basarisiz");
        log_message("ERROR", "listen() basarisiz");
        exit(EXIT_FAILURE);
    }

    log_message("INFO", "Sunucu hazir. Baglanti bekleniyor: %s", SOCKET_PATH);

    /* Ana accept() döngüsü: Yeni bağlantıları kabul et ve kuyruğa gönder */
    while (server_running) {
        struct sockaddr_un client_addr;
        socklen_t client_len = sizeof(struct sockaddr_un);

        /* accept() yeni bir istemci bağlanana kadar bloklar */
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd == -1) {
            if (server_running) {
                /* Gerçek bir hata (kapatma sırasında değil) */
                perror("[HATA] Accept basarisiz");
                log_message("ERROR", "accept() basarisiz");
            }
            continue;
        }

        log_message("INFO", "Yeni istemci baglandi (fd: %d)", client_fd);
        enqueue_client(client_fd); /* Worker thread'e ilet */
    }

    /* Sunucu kapatılıyor: Tüm thread'lerin bitmesini bekle */
    log_message("INFO", "Tum worker thread'ler bekleniyor...");
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_join(thread_pool[i], NULL);
    }

    /* Kaynakları temizle */
    kv_destroy(&store);
    pthread_mutex_destroy(&queue_mutex);
    pthread_cond_destroy(&queue_cond);

    log_message("INFO", "========== Sunucu durduruldu ==========");
    fclose(log_file);
    pthread_mutex_destroy(&log_mutex);

    return 0;
}
