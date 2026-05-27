/*
 * kvstore.h - Anahtar-Değer Deposunun Veri Yapısı Tanımlamaları
 *
 * Bu başlık dosyası, projenin çekirdek veri yapısı olan Hash Map'i ve
 * bu yapı üzerinde çalışan fonksiyonların prototiplerini (imzalarını) tanımlar.
 *
 * Neden ayrı bir .h dosyası?
 * server.c bu veri yapısını doğrudan kullanır. Eğer her şeyi server.c'ye yazsaydık
 * kod çok uzun ve karmaşık olurdu. Ayrıca ileride farklı bir client veya test modülü
 * de bu veri yapısına ihtiyaç duyarsa, sadece #include "kvstore.h" eklemek yeterli olur.
 * Bu yaklaşım "Separation of Concerns" (sorumlulukları ayırma) ilkesidir.
 */

#ifndef KVSTORE_H
#define KVSTORE_H

#include <pthread.h>
#include "protocol.h"

/* Hash tablosunun boyutu. Asal sayıya yakın seçilmesi çakışmaları (collision) azaltır.
 * 1024 = 2^10 olduğundan modulo işlemi de verimli çalışır. */
#define HASH_TABLE_SIZE 1024

/*
 * Node - Hash Tablosundaki Her Bir Düğüm (Separate Chaining için)
 *
 * Farklı anahtarlar aynı hash değerine düşebilir (collision - çakışma).
 * Bu sorunu çözmek için her bucket'ta (kova) bir bağlı liste (linked list) tutuyoruz.
 * Her node, kendi anahtar-değer çiftini ve bir sonraki node'a işaretçiyi içerir.
 */
typedef struct Node {
    char key[MAX_KEY_LEN];    /* Anahtar (örn: "kullanici_adi") */
    char value[MAX_VAL_LEN];  /* Değer (örn: "ahmet") */
    struct Node* next;         /* Aynı bucket'taki bir sonraki düğüme işaretçi */
} Node;

/*
 * KVStore - Ana Veri Deposu Yapısı
 *
 * buckets: Her bir hash indeksinde bir bağlı liste başlangıcı tutan dizi.
 *          Boyutu HASH_TABLE_SIZE kadardır.
 *
 * lock: pthread okuma-yazma kilidi (Read-Write Lock).
 *       Birden fazla thread aynı anda okuma yapabilir (rdlock) FAKAT
 *       yazma işlemi (wrlock) sırasında diğer tüm erişimler engellenir.
 *       Bu sayede Race Condition (yarış durumu) oluşması önlenir ve
 *       performans, sıradan mutex'e göre çok daha yüksek olur.
 */
typedef struct {
    Node* buckets[HASH_TABLE_SIZE]; /* Hash tablosu */
    pthread_rwlock_t lock;           /* Okuma-Yazma Kilidi (senkronizasyon için) */
} KVStore;

/*
 * Fonksiyon Prototipleri (Function Prototypes)
 *
 * Burada sadece fonksiyonların adları, parametreleri ve dönüş tipleri yazılır.
 * Gerçek implementasyon (gövde) kvstore.c dosyasında bulunur.
 * Bu sayede derleyici, fonksiyonu kullanmadan önce nasıl çağrılacağını bilir.
 */
void kv_init(KVStore* store);                                             /* Depoyu başlat */
int  kv_set(KVStore* store, const char* key, const char* value);          /* Değer ekle/güncelle */
int  kv_get(KVStore* store, const char* key, char* out_value);            /* Değer getir */
int  kv_delete(KVStore* store, const char* key);                          /* Değer sil */
void kv_list(KVStore* store, char* out_buffer, int max_len);              /* Tümünü listele */
void kv_destroy(KVStore* store);                                          /* Belleği temizle */

#endif /* KVSTORE_H */
