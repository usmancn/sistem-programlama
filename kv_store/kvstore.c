/*
 * kvstore.c - Anahtar-Değer Deposunun Çekirdek Implementasyonu
 *
 * Bu dosya, Hash Map veri yapısı üzerinde çalışan tüm CRUD (Create, Read,
 * Update, Delete) operasyonlarını gerçekleştirir.
 *
 * Kullanılan Sistem Programlama Kavramları:
 *   - pthread_rwlock_t: Birden fazla thread'in aynı anda okuma yapmasına izin
 *     verirken yazma işlemini özel (exclusive) kilitle korur.
 *   - Hash fonksiyonu (djb2): Anahtarları sabit sürede (O(1)) tabloya yerleştirir.
 *   - Separate Chaining: Hash çakışmalarını bağlı liste ile çözer.
 *   - Dinamik bellek yönetimi: malloc() ile yeni düğüm oluşturulur,
 *     free() ile silinen düğümün belleği geri verilir.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kvstore.h"

/*
 * hash() - djb2 Hash Fonksiyonu
 *
 * Verilen string anahtarı sayısal bir indekse dönüştürür.
 * Bu fonksiyon Daniel J. Bernstein tarafından tasarlanmış olup
 * basit ve etkili dağılımı nedeniyle yaygın kullanılır.
 *
 * Algoritma: hash = hash * 33 + c (bit operasyonu ile hızlandırılmış)
 * static anahtar kelimesi: Bu fonksiyon sadece bu dosya içinde kullanılabilir.
 */
static unsigned int hash(const char *str) {
    unsigned long h = 5381; /* Başlangıç seed değeri */
    int c;
    /* Her karakteri sırasıyla hash değerine dahil et */
    while ((c = *str++))
        h = ((h << 5) + h) + c; /* h * 33 + c */
    /* Tablo boyutuna göre modulo al, geçerli bir indeks döndür */
    return h % HASH_TABLE_SIZE;
}

/*
 * kv_init() - Depoyu Başlat
 *
 * Hash tablosundaki tüm bucket işaretçilerini NULL'a çeker ve
 * okuma-yazma kilidini (pthread_rwlock_t) başlatır.
 * Sunucu başlarken bir kez çağrılır.
 */
void kv_init(KVStore* store) {
    /* Tüm bucket işaretçilerini 0 (NULL) yap */
    memset(store->buckets, 0, sizeof(store->buckets));
    /* pthread rwlock'u varsayılan özelliklerle başlat */
    if (pthread_rwlock_init(&store->lock, NULL) != 0) {
        perror("[HATA] pthread_rwlock_init basarisiz");
    }
}

/*
 * kv_set() - Anahtar-Değer Çifti Ekle veya Güncelle
 *
 * Senkronizasyon: pthread_rwlock_wrlock() kullanır.
 *   Yazma kilidi alındığında diğer tüm okuma ve yazma işlemleri bloklanır.
 *   Bu, iki thread'in aynı anda aynı bucket'a yazmasını (Race Condition) engeller.
 *
 * Dönüş: 0 başarı, -1 hata (malloc başarısız olursa)
 */
int kv_set(KVStore* store, const char* key, const char* value) {
    unsigned int idx = hash(key);

    /* Yazma kilidi al: Başka hiçbir thread bu süreçte veri okuyamaz veya yazamaz */
    pthread_rwlock_wrlock(&store->lock);

    /* Önce bu anahtarın zaten var olup olmadığını kontrol et (güncelleme durumu) */
    Node* current = store->buckets[idx];
    while (current != NULL) {
        if (strcmp(current->key, key) == 0) {
            /* Anahtar bulundu, değeri güncelle */
            strncpy(current->value, value, MAX_VAL_LEN - 1);
            current->value[MAX_VAL_LEN - 1] = '\0';
            pthread_rwlock_unlock(&store->lock); /* Kilidi serbest bırak */
            return 0;
        }
        current = current->next;
    }

    /* Anahtar yoksa yeni bir düğüm (node) oluştur */
    Node* new_node = (Node*)malloc(sizeof(Node));
    if (!new_node) {
        /* malloc başarısız: bellek yetersiz, sistem hatası */
        perror("[HATA] malloc basarisiz, yeni node olusturulamadi");
        pthread_rwlock_unlock(&store->lock);
        return -1;
    }

    /* Anahtar ve değeri yeni düğüme kopyala */
    strncpy(new_node->key, key, MAX_KEY_LEN - 1);
    new_node->key[MAX_KEY_LEN - 1] = '\0'; /* null-terminator garantisi */
    strncpy(new_node->value, value, MAX_VAL_LEN - 1);
    new_node->value[MAX_VAL_LEN - 1] = '\0';

    /* Yeni düğümü listenin başına ekle (O(1) ekleme) */
    new_node->next = store->buckets[idx];
    store->buckets[idx] = new_node;

    pthread_rwlock_unlock(&store->lock); /* Yazma kilidini serbest bırak */
    return 0;
}

/*
 * kv_get() - Anahtara Karşılık Gelen Değeri Getir
 *
 * Senkronizasyon: pthread_rwlock_rdlock() kullanır.
 *   Okuma kilidi, aynı anda birden fazla thread'in veriyi okumasına izin verir.
 *   Bu, yazma kilidine (mutex) göre çok daha performanslı bir yaklaşımdır.
 *   Sadece bir thread yazma yaparken diğerleri okuyamaz.
 *
 * Dönüş: 0 bulundu (out_value dolduruldu), -1 bulunamadı
 */
int kv_get(KVStore* store, const char* key, char* out_value) {
    unsigned int idx = hash(key);

    /* Okuma kilidi al: Diğer thread'ler de aynı anda okuyabilir */
    pthread_rwlock_rdlock(&store->lock);

    Node* current = store->buckets[idx];
    while (current != NULL) {
        if (strcmp(current->key, key) == 0) {
            /* Anahtar bulundu, değeri çıkış tamponuna kopyala */
            strncpy(out_value, current->value, MAX_VAL_LEN - 1);
            out_value[MAX_VAL_LEN - 1] = '\0';
            pthread_rwlock_unlock(&store->lock);
            return 0; /* Başarı */
        }
        current = current->next;
    }

    pthread_rwlock_unlock(&store->lock);
    return -1; /* Anahtar bulunamadı */
}

/*
 * kv_delete() - Anahtarı ve Değerini Sil
 *
 * Senkronizasyon: pthread_rwlock_wrlock() kullanır.
 *   Silme işlemi veri yapısını değiştirdiğinden yazma kilidi gereklidir.
 *   Bağlı liste manipülasyonu sırasında başka bir thread'in listeyi okuması
 *   bellek bozulmasına (memory corruption) yol açardı.
 *
 * Dönüş: 0 silindi, -1 anahtar bulunamadı
 */
int kv_delete(KVStore* store, const char* key) {
    unsigned int idx = hash(key);

    /* Yazma kilidi al */
    pthread_rwlock_wrlock(&store->lock);

    Node* current = store->buckets[idx];
    Node* prev = NULL; /* Bağlı listede bir önceki düğüme işaretçi */

    while (current != NULL) {
        if (strcmp(current->key, key) == 0) {
            /* Silinecek düğüm bulundu, bağlı listeyi yeniden bağla */
            if (prev == NULL) {
                /* Silinecek düğüm listenin başıysa bucket işaretçisini güncelle */
                store->buckets[idx] = current->next;
            } else {
                /* Ortada ya da sondaysa önceki düğümün next'ini atla */
                prev->next = current->next;
            }
            free(current); /* Düğümün belleğini sisteme geri ver */
            pthread_rwlock_unlock(&store->lock);
            return 0; /* Başarıyla silindi */
        }
        prev = current;
        current = current->next;
    }

    pthread_rwlock_unlock(&store->lock);
    return -1; /* Anahtar bulunamadı */
}

/*
 * kv_list() - Depodaki Tüm Anahtar-Değer Çiftlerini Listele
 *
 * Senkronizasyon: pthread_rwlock_rdlock() kullanır.
 *   Sadece okuma yaptığımızdan okuma kilidi yeterlidir.
 *   Tüm bucket'ları gezerek formatlı string oluşturur.
 *
 * out_buffer: Çıktının yazılacağı tampon (caller tarafından ayrılmalıdır)
 * max_len: Taşmayı önlemek için maksimum yazılacak karakter sayısı
 */
void kv_list(KVStore* store, char* out_buffer, int max_len) {
    pthread_rwlock_rdlock(&store->lock);

    out_buffer[0] = '\0';
    int yazilan = 0; /* Şimdiye kadar tampona yazılan karakter sayısı */

    /* Tüm bucket'ları sırayla gez */
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        Node* current = store->buckets[i];
        while (current != NULL) {
            /* Yazılacak satırın kaç karakter tutacağını hesapla */
            int gerekli = snprintf(NULL, 0, "[%s: %s]\n", current->key, current->value);
            if (yazilan + gerekli < max_len - 1) {
                sprintf(out_buffer + yazilan, "[%s: %s]\n", current->key, current->value);
                yazilan += gerekli;
            } else {
                /* Tampon dolu, daha fazla yazmayı durdur */
                break;
            }
            current = current->next;
        }
    }

    /* Hiç veri yoksa bilgilendirici mesaj yaz */
    if (yazilan == 0) {
        snprintf(out_buffer, max_len, "Store is empty.\n");
    }

    pthread_rwlock_unlock(&store->lock);
}

/*
 * kv_destroy() - Depoyu Temizle ve Kaynakları Serbest Bırak
 *
 * Sunucu kapanmadan önce bu fonksiyon çağrılarak tüm dinamik bellek
 * serbest bırakılır ve pthread_rwlock destroy edilir.
 * Bu, bellek sızıntısını (memory leak) önler.
 */
void kv_destroy(KVStore* store) {
    pthread_rwlock_wrlock(&store->lock);
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        Node* current = store->buckets[i];
        while (current != NULL) {
            Node* temp = current;
            current = current->next;
            free(temp); /* Her düğümün belleğini serbest bırak */
        }
        store->buckets[i] = NULL;
    }
    pthread_rwlock_unlock(&store->lock);
    /* Kilidi tamamen yok et */
    pthread_rwlock_destroy(&store->lock);
}
